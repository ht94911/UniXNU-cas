#!/usr/bin/env python3
"""Independently decode a cas boot image; no mkbootimg imports or device I/O."""
import argparse
import hashlib
import json
import struct
import tomllib
import zlib
from pathlib import Path

from check_memory import validate_sizes


def align(value, boundary):
    return (value + boundary - 1) & -boundary


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def inspect_fd(fd, base):
    require(len(fd) == 0x800000 and fd[40:44] == b"_FVH", "missing/excess cas FD or FV header")
    length = struct.unpack_from("<Q", fd, 32)[0]
    header = struct.unpack_from("<H", fd, 48)[0]
    require(length == len(fd) and 56 <= header <= 4096 and header % 2 == 0 and fd[55] == 2, "invalid FV length/header")
    require(sum(struct.unpack_from("<" + "H" * (header // 2), fd)) & 0xffff == 0, "FV header checksum mismatch")
    blocks = 0
    for offset in range(56, header - 7, 8):
        count, size = struct.unpack_from("<II", fd, offset)
        if count == size == 0:
            break
        require(count > 0 and size > 0, "invalid FV block map")
        blocks += count * size
    require(blocks == len(fd), "FV block map length mismatch")
    branch = struct.unpack_from("<I", fd)[0]
    require(branch & 0xfc000000 == 0x14000000, "FD reset vector is not an AARCH64 branch")
    imm = branch & 0x3ffffff
    branch_target = (imm - (1 << 26) if imm & (1 << 25) else imm) * 4
    require(0 < branch_target < len(fd), "FD reset branch outside image")
    pos, sec_entry, files = align(header, 8), None, []
    while pos + 24 <= len(fd):
        hdr = fd[pos:pos + 24]
        if hdr == b"\xff" * 24:
            require(fd[pos:] == b"\xff" * (len(fd) - pos), "non-erased trailing FV bytes")
            break
        require(not (hdr[19] & 1), "unexpected large FFS in minimum FD")
        size = int.from_bytes(hdr[20:23], "little")
        require(24 <= size <= len(fd) - pos, "FFS size outside FV")
        require((sum(hdr) - hdr[17] - hdr[23]) & 255 == 0, "FFS header checksum mismatch")
        if hdr[19] & 0x40:
            require((sum(fd[pos + 24:pos + size]) + hdr[17]) & 255 == 0, "FFS data checksum mismatch")
        else:
            require(hdr[17] == 0xaa, "FFS fixed checksum mismatch")
        files.append({"offset": pos, "bytes": size, "type": hdr[18], "guid_bytes_le": hdr[:16].hex()})
        if hdr[18] == 3:  # EFI_FV_FILETYPE_SECURITY_CORE
            require(sec_entry is None, "multiple SEC files")
            s = pos + 24
            while s + 4 <= pos + size:
                ss = int.from_bytes(fd[s:s + 3], "little")
                require(4 <= ss <= pos + size - s and ss != 0xffffff, "bad SEC section size")
                if fd[s + 3] == 0x12:
                    te = s + 4
                    require(ss >= 44 and fd[te:te + 2] == b"VZ", "invalid SEC TE header")
                    machine, = struct.unpack_from("<H", fd, te + 2)
                    stripped, = struct.unpack_from("<H", fd, te + 6)
                    entry, = struct.unpack_from("<I", fd, te + 8)
                    image_base, = struct.unpack_from("<Q", fd, te + 16)
                    sec_entry = te + 40 - stripped + entry
                    require(machine == 0xaa64 and image_base == base + te + 40 - stripped, "SEC machine/base mismatch")
                    require(te <= sec_entry < s + ss, "SEC entry outside TE section")
                s = align(s + ss, 4)
        pos = align(pos + size, 8)
    require(sec_entry is not None and branch_target == sec_entry, "FD reset vector does not reach SEC entry")
    return {"bytes": len(fd), "header_bytes": header, "header_checksum": "pass", "files": files,
            "reset_branch_offset": branch_target, "sec_entry_physical": hex(base + sec_entry)}


def inspect(image, fd, shim, dtb, ramdisk, config, memory):
    sizes = {"fd": len(fd), "dtb": len(dtb), "shim": len(shim), "ramdisk": len(ramdisk), "boot_image": len(image)}
    validate_sizes(memory, sizes)
    require(len(image) >= 4096 and image[:8] == b"ANDROID!", "truncated or non-Android boot image")
    values = struct.unpack_from("<10I", image, 8)
    keys = ("kernel_size", "kernel_address", "ramdisk_size", "ramdisk_address", "second_size", "second_address",
            "tags_address", "pagesize", "header_version", "os_version")
    h = dict(zip(keys, values))
    recovery_size, recovery_offset, header_size, dtb_size, dtb_address = struct.unpack_from("<IQIIQ", image, 1632)
    h.update(recovery_dtbo_size=recovery_size, recovery_dtbo_offset=recovery_offset,
             header_size=header_size, dtb_size=dtb_size, dtb_address=dtb_address)
    b = config["boot_image"]
    require(b["header_version"] == h["header_version"] == 2 and b["pagesize"] == h["pagesize"] == 4096, "wrong header version/page size")
    require(h["header_size"] == 1660 and h["kernel_size"] > 0, "invalid header/kernel size")
    for field, expected in {"kernel_address": b["base"] + b["kernel_offset"],
                            "ramdisk_size": len(ramdisk),
                            "ramdisk_address": b["base"] + b["ramdisk_offset"] if ramdisk else 0,
                            "tags_address": b["base"] + b["tags_offset"],
                            "dtb_address": b["base"] + b["dtb_offset"], "dtb_size": len(dtb),
                            "second_size": 0, "second_address": 0, "recovery_dtbo_size": 0,
                            "recovery_dtbo_offset": 0, "os_version": 0}.items():
        require(h[field] == expected, "boot header field mismatch: " + field)
    require(image[48:64] == b"cas" + bytes(13), "unexpected boot board name")
    require(b["cmdline"] == "" and not any(image[64:576] + image[608:1632]), "unexpected boot command line")
    require(not any(image[1660:4096]), "nonzero boot header padding")
    require(config["boot_image_kernel"] == {"kernel_compression": "gzip", "append_dtb": False}, "unsupported kernel packaging policy")
    pos, parts, ranges = 4096, {}, {}
    for name, size in (("kernel", h["kernel_size"]), ("ramdisk", len(ramdisk)), ("dtb", len(dtb))):
        end, padded = pos + size, pos + align(size, 4096)
        require(padded <= len(image), "truncated boot section: " + name)
        require(not any(image[end:padded]), "nonzero section padding: " + name)
        parts[name] = image[pos:end]
        ranges[name] = {"file_offset": pos, "bytes": size, "sha256": hashlib.sha256(parts[name]).hexdigest()}
        pos = padded
    require(pos == len(image), "unexpected trailing boot section/footer")
    digest = hashlib.sha1()
    for part in (parts["kernel"], parts["ramdisk"], b"", b"", parts["dtb"]):
        digest.update(part)
        digest.update(struct.pack("<I", len(part)))
    require(image[576:608] == digest.digest() + bytes(12), "Android image ID mismatch")
    require(parts["ramdisk"] == ramdisk and parts["dtb"] == dtb, "ramdisk/DTB content mismatch")
    require(len(dtb) >= 40 and dtb[:4] == b"\xd0\x0d\xfe\xed" and struct.unpack_from(">I", dtb, 4)[0] == len(dtb), "not exactly one complete DTB")
    _, _, structure, strings, reserve, version, compatible, _, strings_size, structure_size = struct.unpack_from(">10I", dtb)
    require(version == 17 and compatible <= 17 and 40 <= structure <= len(dtb) and 40 <= strings <= len(dtb)
            and 40 <= reserve <= len(dtb) - 16 and structure_size <= len(dtb) - structure
            and strings_size <= len(dtb) - strings, "invalid DTB block bounds")
    tree_end = max(structure + structure_size, strings + strings_size)
    padding = config["boot_image_dtb"]["padding"]
    require(padding == 8192 and len(dtb) - tree_end == padding and not any(dtb[tree_end:]), "DTB expansion padding missing or corrupt")
    require(len(shim) >= 64 and len(shim) % 4096 == 0 and shim[56:60] == b"ARM\x64", "invalid ARM64 shim")
    text_offset, image_size, flags = struct.unpack_from("<QQQ", shim, 8)
    require(text_offset == config["boot_shim"]["text_offset"] == 0x80000, "ARM64 text_offset mismatch")
    require(image_size == len(shim) + len(fd) and flags == 0 and not any(shim[32:56] + shim[60:64]), "ARM64 image size/flags mismatch")
    gz = parts["kernel"]
    require(len(gz) > 18 and gz[:4] == b"\x1f\x8b\x08\x00" and gz[4:8] == bytes(4), "gzip must have deterministic header and no optional fields")
    try:
        decoder = zlib.decompressobj(31)
        raw = decoder.decompress(gz, image_size + 1)
    except zlib.error as exc:
        raise ValueError("invalid gzip stream or CRC") from exc
    require(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail, "gzip is truncated, concatenated or exceeds kernel bound")
    require(raw == shim + fd, "decompressed kernel is not exact shim+FD (appended DTB/padding forbidden)")
    require(config["boot_image_dtb"]["source"] == "stock-soc-dtb" and
            hashlib.sha256(dtb).hexdigest() == config["boot_image_dtb"]["prepared_sha256"],
            "DTB does not match the pinned prepared stock tree")
    fd_report = inspect_fd(fd, config["uefi_fd"]["base"])
    parts.update(kernel_decompressed=raw, shim=raw[:len(shim)], fd=raw[len(shim):])
    return {"result": "pass", "hardware_proven": False, "sizes": sizes, "header": h, "parts": ranges,
            "arm64": {"text_offset": text_offset, "image_size": image_size, "flags": flags},
            "fd": fd_report, "dtb_expansion_padding": padding, "dtb_source": "stock-soc-dtb",
            "boot_sha256": hashlib.sha256(image).hexdigest()}, parts


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("image", "fd", "shim", "dtb", "ramdisk", "config", "map"):
        ap.add_argument("--" + name, type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    report, parts = inspect(*(getattr(a, name).read_bytes() for name in ("image", "fd", "shim", "dtb", "ramdisk")),
                            tomllib.loads(a.config.read_text()), json.loads(a.map.read_text()))
    a.out.mkdir(parents=True, exist_ok=False)
    for name, data in parts.items():
        (a.out / (name + ".bin")).write_bytes(data)
    (a.out / "check.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
