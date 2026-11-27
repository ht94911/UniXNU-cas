#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Audit a Wave 12 temporary-boot plan and optionally emit its two host modules.

The plan pins the base_gpuaddon, base_trustcache and each member of
native_artifacts with {"path": "...", "sha256": "..."}. Paths are relative to
the plan file unless absolute. --audit-plan alone performs the full byte-level
rebuild and readback in memory. --out-dir emits the checked modules and report
into a new host directory for the separate temporary-boot assembly step.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import posixpath
import re
import stat
import struct
import uuid
from xml.parsers.expat import ExpatError
import zlib


CACHE_ROOT = Path("/home/hua/nezha-work/ios27/adreno840-native/.cache/w12-l4")
HOST_TRIALS = Path("/mnt/i/Code/UniXNU-nezha/artifacts/host-trials")
EXCLUDED_ROOTS = ("/mnt/c", "/tmp", "/var/tmp", "/private/tmp",
                  "/home/hua/nezha-work/ios27/gpu27-astra/integration088")
BOOT_POLICY = {"full_temporary_boot": True, "phone_payload_write_required": False,
               "Data_generation": 1, "no_Data_creation_or_reset": True}
HOST_FLAGS = {"device_verified": False, "hardware_acceptance": False}

BUNDLE = "addons/NativeAdrenoMetal.bundle"
BUNDLE_BINARY = "Adreno840Metal"
BUNDLE_INSTALL_PATH = "/System/Library/Extensions/NativeAdrenoMetal.bundle"
PUBLISH = "addons/NativeAdrenoMetal.publish"
DAEMON_EXECUTABLE = "/usr/sbin/NezhaShaderCompiler"
DAEMON_LABEL = "org.unixnu.nezha.shader-compiler"
DAEMON_PLIST = f"/System/Library/LaunchDaemons/{DAEMON_LABEL}.plist"
DAEMON_ADDON_PLIST = f"daemons/{DAEMON_LABEL}.plist"
PACK_PATH = "/System/Library/Extensions/NativeAdrenoMetal.pack.bin"
SOURCE_PACK_PATH = "/usr/share/nezha/adreno840-system-pack.bin"
PACK_MEMBER = "addons/NativeAdrenoMetal.pack.bin"
LAUNCHD_CACHE = "xpc/launchd.plist"
LAUNCHD_SIGNATURE = LAUNCHD_CACHE + ".sig"
ADDON_OUTPUT = "kernel.macho.gpuaddon"
TRUST_OUTPUT = "kernel.macho.trustcache"
MEMORY_LIMIT_KIB = 262144
BUNDLE_INFO = {
    "CFBundleIdentifier": "org.nezha.Adreno840Metal",
    "CFBundleExecutable": BUNDLE_BINARY,
    "CFBundlePackageType": "BNDL",
    "NSPrincipalClass": "NativeAdrenoMetalDevice",
    "MinimumOSVersion": "27.0",
}
ARTIFACTS = {
    "bundle_info": (f"{BUNDLE}/Info.plist", 0o644),
    "bundle_executable": (f"{BUNDLE}/{BUNDLE_BINARY}", 0o755),
    "bundle_code_resources": (f"{BUNDLE}/_CodeSignature/CodeResources", 0o644),
    "shader_compiler": ("bluetooth-bin/NezhaShaderCompiler", 0o755),
    "launch_daemon": (DAEMON_ADDON_PLIST, 0o644),
    "pack": (PACK_MEMBER, 0o444),
    "gpu_test": ("bluetooth-bin/NezhaGpuTest", 0o755),
    "gpu_test_metallib": ("bluetooth-bin/NezhaGpuTest.metallib", 0o644),
}
LEGACY_NAME = re.compile(
    r"turnip|vulkan|moltenvk|freedreno[._-]?icd|native[._-]?bridge[._-]?metal[._-]?frontend"
    r"|m2v[._-]?(?:pack|runtime)", re.I)
LEGACY_BINARY = re.compile(
    rb"libvulkan|libturnip|metal2vulkan[-_]runtime|libmoltenvk|moltenvk\.framework|freedreno[._-]?icd|"
    rb"native[._-]?bridge[._-]?metal[._-]?frontend|\bturnip\b|"
    rb"vulkan[._/-](?:loader|icd)|(?:^|\x00)_*(?-i:vk[A-Z]\w*|tu_[a-z]\w*)\x00", re.I)
MACHO_HEADER = struct.Struct("<8I")
ADDON_HEADER = struct.Struct("<8sIIQ")
ADDON_ENTRY = struct.Struct("<IIQQ")
TRUST_HEADER = struct.Struct("<I16sI")
CODE_DIRECTORY = struct.Struct(">9I4BI")
PAGE_BYTES = 4096


def require(condition, message: str) -> None:
    if not condition:
        raise ValueError(message)


def fields(value, name: str, required, optional=()) -> dict:
    require(type(value) is dict, f"{name} must be an object")
    missing = set(required) - value.keys()
    unexpected = value.keys() - set(required) - set(optional)
    require(not missing and not unexpected,
            f"{name} fields differ (missing {sorted(missing)}, unexpected {sorted(unexpected)})")
    return value


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def check_sha256(value, name: str) -> str:
    require(type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value),
            f"{name} requires a full lowercase SHA-256")
    return value


def parse_json(data: bytes) -> dict:
    def unique_object(pairs):
        obj = dict(pairs)
        require(len(obj) == len(pairs), "duplicate JSON field")
        return obj

    def nonfinite(value):
        raise ValueError(f"non-finite JSON number: {value}")

    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object, parse_constant=nonfinite)


def checked_path(raw: str, base: Path) -> Path:
    require(type(raw) is str and raw and not any(ord(c) < 32 for c in raw)
            and "\\" not in raw and ":" not in raw, f"invalid host path: {raw!r}")
    path = base / raw
    require(path.is_absolute() and ".." not in path.parts, f"invalid host path: {raw}")
    def check_location(candidate):
        lowered = Path(str(candidate).casefold())
        require(not any(lowered.is_relative_to(root) for root in EXCLUDED_ROOTS),
                f"forbidden host path: {candidate}")

    check_location(path)
    resolved = path.resolve()
    check_location(resolved)
    return resolved


def read_file(path: Path) -> bytes:
    require(stat.S_ISREG(path.stat().st_mode), f"input must be a regular file: {path}")
    return path.read_bytes()


def read_pinned(ref, base: Path, name: str) -> bytes:
    item = fields(ref, name, ("path", "sha256"))
    digest = check_sha256(item["sha256"], name)
    data = read_file(checked_path(item["path"], base))
    require(sha256(data) == digest, f"{name} SHA-256 mismatch")
    return data


def check_boot_policy(plan: dict) -> None:
    for name, expected in BOOT_POLICY.items():
        actual = plan.get(name)
        require(type(actual) is type(expected) and actual == expected, f"{name} must be {expected}")
    require(plan.get("payload_write", False) is False, "payload_write must be false")


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & -alignment


def read_addon(data: bytes) -> dict[str, tuple[int, bytes]]:
    magic, count, zero, length = ADDON_HEADER.unpack_from(data)
    require(magic == b"NGPUADD2" and zero == 0 and length == len(data)
            and 0 < count <= (length - ADDON_HEADER.size) // ADDON_ENTRY.size,
            "invalid NGPUADD2 header")
    entries, seen = {}, set()
    offset = ADDON_HEADER.size
    expanded = 0
    for _ in range(count):
        name_size, mode, size, packed_size = ADDON_ENTRY.unpack_from(data, offset)
        name_start = offset + ADDON_ENTRY.size
        packed_start = name_start + align(name_size, 8)
        offset = packed_start + align(packed_size, 8)
        expanded += size
        require(2 <= name_size <= 160 and 0 <= mode <= 0o777 and packed_size > 0
                and offset <= length and expanded <= 512 * 1024 * 1024,
                "invalid NGPUADD2 entry bounds or mode")
        raw_name = data[name_start:name_start + name_size]
        require(raw_name[-1:] == b"\0" and not any(data[name_start + name_size:packed_start])
                and not any(data[packed_start + packed_size:offset]), "invalid NGPUADD2 entry padding")
        name = raw_name[:-1].decode("utf-8")
        require(all(part and part not in {".", ".."} for part in name.split("/"))
                and not any(ord(c) < 32 for c in name) and "\\" not in name and ":" not in name,
                f"unsafe NGPUADD2 member: {name!r}")
        require(name.casefold() not in seen, f"duplicate NGPUADD2 member: {name}")
        seen.add(name.casefold())
        require("payload.img" not in name.casefold().split("/"),
                f"NGPUADD2 member may not address payload.img: {name}")
        stream = zlib.decompressobj()
        body = stream.decompress(data[packed_start:packed_start + packed_size], size + 1)
        require(len(body) == size and stream.eof and not stream.unused_data,
                f"invalid NGPUADD2 compressed stream: {name}")
        entries[name] = (mode, body)
    require(offset == length, "trailing NGPUADD2 bytes")
    for name in seen:
        parents = (name[:at] for at, char in enumerate(name) if char == "/")
        require(not any(parent in seen for parent in parents), f"NGPUADD2 file/directory collision: {name}")
    return entries


def encode_addon(entries: dict[str, tuple[int, bytes]]) -> bytes:
    content = bytearray()
    for name, (mode, data) in sorted(entries.items()):
        raw = name.encode("utf-8") + b"\0"
        packed = zlib.compress(data, 9)
        content += ADDON_ENTRY.pack(len(raw), mode, len(data), len(packed))
        content += raw + bytes(-len(raw) % 8)
        content += packed + bytes(-len(packed) % 8)
    return ADDON_HEADER.pack(b"NGPUADD2", len(entries), 0, ADDON_HEADER.size + len(content)) + content


def sign_blobs(blobs: dict[int, bytes]) -> bytes:
    offset = 12 + 8 * len(blobs)
    index = bytearray()
    for slot, blob in blobs.items():
        index += struct.pack(">II", slot, offset)
        offset += len(blob)
    return struct.pack(">III", 0xFADE0CC0, offset, len(blobs)) + index + b"".join(blobs.values())


def unpack_signature(raw: bytes) -> dict[int, bytes]:
    magic, length, count = struct.unpack_from(">III", raw)
    table_end = 12 + count * 8
    require(magic == 0xFADE0CC0 and 0 < count <= 32 and table_end <= length <= len(raw)
            and not any(raw[length:]), "invalid signature SuperBlob")
    blobs, regions = {}, []
    for slot, offset in struct.iter_unpack(">II", raw[12:table_end]):
        require(slot not in blobs and table_end <= offset <= length - 8, "invalid signature slot")
        size, = struct.unpack_from(">I", raw, offset + 4)
        require(8 <= size <= length - offset, "truncated signature slot")
        blobs[slot] = raw[offset:offset + size]
        regions.append((offset, offset + size))
    regions.sort()
    require(all(a[1] <= b[0] for a, b in zip(regions, regions[1:])), "overlapping signature slots")
    require(0 in blobs, "missing primary CodeDirectory")
    require(not any(0x1000 <= slot <= 0x1005 for slot in blobs), "ambiguous alternate CodeDirectory")
    return blobs


def verify_code_directory(data: bytes, signature: bytes, special_files=None) -> dict:
    blobs = unpack_signature(signature)
    cd = blobs[0]
    magic, size, version, _, hashes, ident, special, pages, limit, width, kind, _, shift, _ = (
        CODE_DIRECTORY.unpack_from(cd))
    start = hashes - special * 32
    require(magic == 0xFADE0C02 and size == len(cd) and kind == 2 and width == 32 and shift == 12,
            "requires a SHA-256 4 KiB CodeDirectory")
    require(limit == len(data) and pages == (limit + PAGE_BYTES - 1) // PAGE_BYTES and pages > 0
            and CODE_DIRECTORY.size <= ident < start and hashes + 32 * pages <= size,
            "invalid CodeDirectory coverage or hash offsets")
    if version >= 0x20100:
        require(ident >= 48 and not struct.unpack_from(">I", cd, 44)[0],
                "unsupported scattered CodeDirectory")
    end = cd.find(b"\0", ident, start)
    require(end > ident, "invalid CodeDirectory identifier")
    identifier = cd[ident:end].decode("utf-8")
    covered = {slot: blobs[slot] for slot in (2, 5, 7) if slot in blobs}
    covered.update(special_files or {})
    for slot, value in covered.items():
        at = hashes - slot * 32
        require(special >= slot and cd[at:at + 32] == hashlib.sha256(value).digest(),
                f"CodeDirectory special slot {slot} hash mismatch")
    for page, offset in enumerate(range(0, limit, PAGE_BYTES)):
        at = hashes + 32 * page
        require(cd[at:at + 32] == hashlib.sha256(data[offset:offset + PAGE_BYTES]).digest(),
                f"4 KiB code page {page} hash mismatch")
    return {"cdhash": sha256(cd)[:40], "pages_verified": pages, "page_bytes": PAGE_BYTES,
            "identifier": identifier}


def verify_macho(data: bytes, file_type: int, special_files=None) -> dict:
    magic, cpu, subtype, kind, count, command_size, _, _ = MACHO_HEADER.unpack_from(data)
    require((magic, cpu, subtype, kind) == (0xFEEDFACF, 0x0100000C, 0x80000002, file_type),
            "requires a thin arm64e PAC00 Mach-O of the expected file type")
    offset = MACHO_HEADER.size
    end = offset + command_size
    require(end <= len(data) and 0 < count <= command_size // 8, "invalid Mach-O commands")
    deployment = code_signature = None
    for _ in range(count):
        cmd, size = struct.unpack_from("<II", data, offset)
        require(size >= 8 and size % 8 == 0 and offset + size <= end, "invalid Mach-O command size")
        if cmd == 0x32:
            require(size >= 24 and deployment is None, "invalid LC_BUILD_VERSION")
            platform, minimum, _, tools = struct.unpack_from("<4I", data, offset + 8)
            require(size == 24 + tools * 8, "invalid LC_BUILD_VERSION tools")
            deployment = (platform, minimum)
        if cmd == 0x1D:
            require(size == 16 and code_signature is None, "invalid LC_CODE_SIGNATURE")
            code_signature = struct.unpack_from("<II", data, offset + 8)
        offset += size
    require(offset == end and deployment == (2, 27 << 16),
            "requires LC_BUILD_VERSION targeting iOS 27.0")
    require(code_signature is not None, "missing LC_CODE_SIGNATURE")
    start, length = code_signature
    require(end <= start < len(data) and length >= 12 and start + length == len(data),
            "invalid Mach-O signature extent")
    return verify_code_directory(data[:start], data[start:], special_files)


def split_trustcache(raw: bytes) -> list[bytes]:
    count, = struct.unpack_from("<I", raw)
    require(1 <= count <= 16 and 4 + 4 * count < len(raw), "invalid trustcache module count")
    starts = struct.unpack_from(f"<{count}I", raw, 4)
    require(starts[0] == 4 + 4 * count and tuple(sorted(set(starts))) == starts
            and starts[-1] < len(raw), "invalid trustcache module offsets")
    modules = [raw[start:end] for start, end in zip(starts, (*starts[1:], len(raw)))]
    require(all(len(module) >= TRUST_HEADER.size for module in modules), "truncated trustcache module")
    return modules


def append_trustcache(modules: list[bytes], hashes: list[str]) -> bytes:
    require(len(modules) < 16, "no slot for a new owner trustcache module")
    entries = sorted({bytes.fromhex(digest) for digest in hashes})
    require(len(entries) == len(hashes) and all(len(digest) == 20 for digest in entries),
            "new trustcache requires distinct 20-byte CDHashes")
    owner = uuid.uuid5(uuid.NAMESPACE_URL, "org.unixnu.owner:" + "".join(d.hex() for d in entries)).bytes
    owner_module = TRUST_HEADER.pack(2, owner, len(entries))
    owner_module += b"".join(digest + b"\x02\x00\x00\x00" for digest in entries)
    modules = [*modules, owner_module]
    starts, offset = [], 4 + 4 * len(modules)
    for module in modules:
        starts.append(offset)
        offset += len(module)
    return struct.pack(f"<{len(modules) + 1}I", len(modules), *starts) + b"".join(modules)


def verify_trustcache(data: bytes, previous: list[bytes], hashes: list[str]) -> None:
    modules = split_trustcache(data)
    require(modules[:-1] == previous, "existing trustcache modules changed")
    version, owner, count = TRUST_HEADER.unpack_from(modules[-1])
    entries = list(struct.iter_unpack("<20sBBBB", modules[-1][TRUST_HEADER.size:]))
    require(version == 2 and count == len(entries) and count == len(hashes)
            and [item[0].hex() for item in entries] == sorted(hashes)
            and all(item[1:] == (2, 0, 0, 0) for item in entries)
            and all(TRUST_HEADER.unpack_from(module)[1] != owner for module in previous),
            "new owner trustcache does not register every verified SHA-256 CDHash")


def plist_dict(data: bytes, name: str) -> dict:
    value = plistlib.loads(data, dict_type=UniquePlistDict)
    require(type(value) is UniquePlistDict, f"{name} must be a plist dictionary")
    return value


class UniquePlistDict(dict):
    def __setitem__(self, key, value):
        require(key not in self, f"duplicate plist key: {key!r}")
        super().__setitem__(key, value)


def plist_strings(value):
    if isinstance(value, dict):
        for key, item in value.items():
            yield key
            yield from plist_strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from plist_strings(item)
    elif isinstance(value, str):
        yield value
    elif isinstance(value, bytes):
        yield value.decode("utf-8", errors="replace")


def verify_daemon(data: bytes) -> dict:
    job = plist_dict(data, "LaunchDaemon")
    for text in plist_strings(job):
        normalized = ["/" + posixpath.normpath(path).lstrip("/")
                      for path in re.findall(r"""(?<![\w/])/[^\s:;"'=]*""", text)]
        require(not any(re.search(r"/(?:private/)?var/ngpu(?:/|$)", path, re.I)
                        for path in [text, *normalized]), "LaunchDaemon uses /private/var/ngpu")
        require("payload.img" not in text.casefold(), "LaunchDaemon may not address payload.img")
        require(not LEGACY_NAME.search(text), "LaunchDaemon references a legacy GPU component")
    args = job.get("ProgramArguments")
    require(isinstance(args, list) and args and all(type(arg) is str and arg for arg in args)
            and args[0] == DAEMON_EXECUTABLE and job.get("Program", DAEMON_EXECUTABLE) == DAEMON_EXECUTABLE,
            f"LaunchDaemon must run {DAEMON_EXECUTABLE}")
    require(args.count("--pack") == 1 and args.index("--pack") + 1 < len(args)
            and args[args.index("--pack") + 1] in (PACK_PATH, SOURCE_PACK_PATH)
            and not any(arg.startswith("--pack=") for arg in args),
            f"LaunchDaemon must pass --pack {PACK_PATH} or its compiler-source path {SOURCE_PACK_PATH}")
    require(job.get("Label") == DAEMON_LABEL and job.get("RunAtLoad") is True and job.get("KeepAlive") is True,
            "LaunchDaemon needs its system label, RunAtLoad and KeepAlive")
    limits = job.get("HardResourceLimits")
    limit = limits.get("MemoryLimit") if isinstance(limits, dict) else None
    require(type(limit) is int and limit == MEMORY_LIMIT_KIB,
            "HardResourceLimits.MemoryLimit must be 262144 KiB (256 MiB)")
    soft = job.get("SoftResourceLimits", {})
    require(isinstance(soft, dict) and ("MemoryLimit" not in soft
            or (type(soft["MemoryLimit"]) is int and soft["MemoryLimit"] == MEMORY_LIMIT_KIB)),
            "conflicting SoftResourceLimits.MemoryLimit")
    return job


def count_prefix(kind: int, count: int) -> bytes:
    if count < 15:
        return bytes([kind | count])
    width = next(n for n in (1, 2, 4, 8) if count < 1 << (n * 8))
    return bytes([kind | 15, 0x10 | (width.bit_length() - 1)]) + count.to_bytes(width, "big")


def dictionary_refs(source: bytes, offset: int, width: int, table: int, objects: int):
    require(offset < table and source[offset] >> 4 == 0xD, "expected a launchd cache dictionary")
    count = source[offset] & 15
    offset += 1
    if count == 15:
        tag = source[offset]
        require(0x10 <= tag <= 0x13, "invalid launchd dictionary length")
        size = 1 << (tag & 15)
        offset += 1
        count = int.from_bytes(source[offset:offset + size], "big")
        offset += size
    require(offset + 2 * count * width <= table, "truncated launchd dictionary references")
    refs = [int.from_bytes(source[at:at + width], "big")
            for at in range(offset, offset + 2 * count * width, width)]
    require(all(ref < objects for ref in refs), "invalid launchd dictionary reference")
    return refs[:count], refs[count:], offset


def merge_launchd(source: bytes, job: dict) -> bytes:
    """Add the owner job without reserializing the existing cache objects."""
    original = plist_dict(source, "launchd cache")
    width, ref_width, count, root, table = struct.unpack(">6xBBQQQ", source[-32:])
    require(source[:8] == b"bplist00" and original.get("VersionNumber") == 7
            and width in (1, 2, 4, 8) and ref_width in (1, 2, 4, 8)
            and root < count and 8 <= table and table + count * width == len(source) - 32,
            "unsupported launchd cache layout")
    jobs = original.get("LaunchDaemons")
    require(isinstance(jobs, dict) and DAEMON_PLIST not in jobs and all(
        isinstance(value, dict) and value.get("Label") != DAEMON_LABEL for value in jobs.values()),
        "invalid or conflicting launchd cache job")
    offsets = [int.from_bytes(source[at:at + width], "big")
               for at in range(table, table + count * width, width)]
    require(all(8 <= offset < table for offset in offsets), "invalid launchd cache object offsets")
    keys, values, ref_start = dictionary_refs(source, offsets[root], ref_width, table, count)
    position = list(original).index("LaunchDaemons")
    job_keys, job_values, _ = dictionary_refs(source, offsets[values[position]], ref_width, table, count)
    require(len(job_keys) == len(jobs), "launchd cache job references differ from decoded jobs")
    body = bytearray(source[:table])

    def refs(indices):
        return b"".join(index.to_bytes(ref_width, "big") for index in indices)

    def add(value):
        if isinstance(value, dict):
            names = [add(key) for key in value]
            contents = [add(item) for item in value.values()]
            raw = count_prefix(0xD0, len(names)) + refs(names + contents)
        elif isinstance(value, list):
            children = [add(item) for item in value]
            raw = count_prefix(0xA0, len(children)) + refs(children)
        elif type(value) is str:
            encoding, tag, unit = ("ascii", 0x50, 1) if value.isascii() else ("utf-16be", 0x60, 2)
            text = value.encode(encoding)
            raw = count_prefix(tag, len(text) // unit) + text
        elif type(value) is bool:
            raw = b"\x09" if value else b"\x08"
        elif type(value) is int:
            size = next((n for n in (1, 2, 4, 8) if 0 <= value < 1 << (8 * n)), 8)
            raw = bytes([0x10 | (size.bit_length() - 1)]) + value.to_bytes(size, "big", signed=value < 0)
        elif type(value) is bytes:
            raw = count_prefix(0x40, len(value)) + value
        else:
            raise ValueError(f"unsupported owner plist value: {type(value).__name__}")
        index = len(offsets)
        offsets.append(len(body))
        body.extend(raw)
        return index

    key, value = add(DAEMON_PLIST), add(job)
    new_jobs = len(offsets)
    offsets.append(len(body))
    body.extend(count_prefix(0xD0, len(jobs) + 1))
    body.extend(refs(job_keys + [key] + job_values + [value]))
    slot = ref_start + (len(keys) + position) * ref_width
    body[slot:slot + ref_width] = refs([new_jobs])
    new_table = len(body)
    width = max(width, next(n for n in (1, 2, 4, 8) if new_table < 1 << (8 * n)))
    body.extend(b"".join(offset.to_bytes(width, "big") for offset in offsets))
    body.extend(struct.pack(">6xBBQQQ", width, ref_width, len(offsets), root, new_table))
    merged = bytes(body)
    decoded = plist_dict(merged, "merged launchd cache")
    require(decoded["LaunchDaemons"].pop(DAEMON_PLIST, None) == job and decoded == original,
            "launchd cache readback changed an existing job")
    return merged


def sign_launchd(data: bytes) -> bytes:
    identifier = (DAEMON_LABEL + ".launchd-cache").encode("ascii") + b"\0"
    requirements = struct.pack(">III", 0xFADE0C01, 12, 0)
    pages = [hashlib.sha256(data[at:at + PAGE_BYTES]).digest() for at in range(0, len(data), PAGE_BYTES)]
    hashes = 96 + len(identifier) + 64
    cd = CODE_DIRECTORY.pack(0xFADE0C02, hashes + 32 * len(pages), 0x20500, 2, hashes,
                             96, 2, len(pages), len(data), 32, 2, 0, 12, 0)
    cd += bytes(52) + identifier + hashlib.sha256(requirements).digest() + bytes(32) + b"".join(pages)
    return sign_blobs({0: cd, 2: requirements, 0x10000: struct.pack(">II", 0xFADE0B01, 8)})


def verify_pack(data: bytes) -> None:
    magic, control, bucket_log, count, abi, stack, chip, payload, digest = struct.unpack_from("<4sBBHQQII32s", data)
    require(magic == b"8PK1" and control == 129 and 1 <= bucket_log <= 16 and 0 < count <= 1 << (bucket_log - 1)
            and abi and stack and chip == 0x44050A31, "invalid Adreno 840 Tier-0 pack.bin header")
    start = align(64 + (4 << bucket_log), 128) + 128 * count
    require(payload > 0 and start + payload == len(data)
            and hashlib.sha256(data[:32] + bytes(32) + data[64:]).digest() == digest,
            "pack.bin size or integrity hash mismatch")


def replaced_member(name: str) -> bool:
    folded = name.casefold()
    return (LEGACY_NAME.search(name) is not None
            or any(part in {"nativeadrenometal.bundle", "adreno840metal.bundle"} for part in folded.split("/"))
            or folded.startswith("addons/nativeadrenometal.")
            or folded in {path.casefold() for path, _ in ARTIFACTS.values()})


def audit_inventory(entries: dict[str, tuple[int, bytes]]) -> None:
    for path, (_, data) in entries.items():
        require(not LEGACY_NAME.search(path) and not LEGACY_BINARY.search(data),
                f"legacy Turnip/Vulkan component in final gpuaddon: {path}")


def audit_plan(plan: dict, base: Path) -> tuple[dict, dict[str, bytes]]:
    fields(plan, "plan", ("schema_version", *BOOT_POLICY, "base_gpuaddon", "base_trustcache", "native_artifacts"),
           ("payload_write",))
    require(type(plan["schema_version"]) is int and plan["schema_version"] == 1, "unsupported plan schema")
    check_boot_policy(plan)
    raw_addon = read_pinned(plan["base_gpuaddon"], base, "base_gpuaddon")
    raw_trust = read_pinned(plan["base_trustcache"], base, "base_trustcache")
    original = read_addon(raw_addon)
    previous_trust = split_trustcache(raw_trust)
    require(f"{BUNDLE}/Info.plist" in original and LAUNCHD_CACHE in original and LAUNCHD_SIGNATURE in original,
            "base gpuaddon is missing the discovered bundle or signed launchd cache")
    refs = fields(plan["native_artifacts"], "native_artifacts",
                  ARTIFACTS.keys() - {"gpu_test_metallib"}, ("gpu_test_metallib",))
    inputs = {name: read_pinned(ref, base, name) for name, ref in refs.items()}
    info = plist_dict(inputs["bundle_info"], "bundle Info.plist")
    for key, value in BUNDLE_INFO.items():
        require(info.get(key) == value, f"bundle Info.plist {key} must be {value}")
    plist_dict(inputs["bundle_code_resources"], "bundle CodeResources")
    job = verify_daemon(inputs["launch_daemon"])
    args = job["ProgramArguments"]
    pack_arg = args.index("--pack") + 1
    source_pack = args[pack_arg]
    if source_pack != PACK_PATH:
        # The temporary image exposes addons/ through the Extensions union.
        args[pack_arg] = PACK_PATH
        inputs["launch_daemon"] = plistlib.dumps(job, sort_keys=False)
    verify_pack(inputs["pack"])
    if "gpu_test_metallib" in inputs:
        require(inputs["gpu_test_metallib"].startswith(b"MTLB"), "invalid NezhaGpuTest.metallib")

    signatures = {}
    for name, file_type in (("bundle_executable", 8), ("shader_compiler", 2), ("gpu_test", 2)):
        special = {1: inputs["bundle_info"], 3: inputs["bundle_code_resources"]} if file_type == 8 else None
        try:
            signatures[ARTIFACTS[name][0]] = verify_macho(inputs[name], file_type, special)
        except (ValueError, struct.error) as exc:
            raise ValueError(f"{name}: {exc}") from exc
    require(signatures[ARTIFACTS["bundle_executable"][0]]["identifier"] == BUNDLE_INFO["CFBundleIdentifier"],
            "bundle signature identifier must match CFBundleIdentifier")
    verify_code_directory(original[LAUNCHD_CACHE][1], original[LAUNCHD_SIGNATURE][1])
    merged = merge_launchd(original[LAUNCHD_CACHE][1], job)
    detached = sign_launchd(merged)
    signatures[LAUNCHD_CACHE] = verify_code_directory(merged, detached)
    hashes = [item["cdhash"] for item in signatures.values()]

    removed = sorted(name for name in original if replaced_member(name))
    updated = {name: value for name, value in original.items() if name not in removed}
    for name, data in inputs.items():
        path, mode = ARTIFACTS[name]
        updated[path] = (mode, data)
    updated.update({PUBLISH: (0o644, b"1\n"), LAUNCHD_CACHE: (0o644, merged),
                    LAUNCHD_SIGNATURE: (0o644, detached)})
    audit_inventory(updated)
    addon = encode_addon(updated)
    require(read_addon(addon) == updated, "NGPUADD2 roundtrip differs from the audited inventory")
    trust = append_trustcache(previous_trust, hashes)
    verify_trustcache(trust, previous_trust, hashes)
    outputs = {ADDON_OUTPUT: addon, TRUST_OUTPUT: trust}
    report = {
        "schema_version": 1, "status": "PASS", **BOOT_POLICY, **HOST_FLAGS,
        "payload_write": False, "phone_operations": False,
        "base_gpuaddon_sha256": sha256(raw_addon), "base_trustcache_sha256": sha256(raw_trust),
        "bundle_install_path": BUNDLE_INSTALL_PATH, "publish_marker": PUBLISH,
        "launch_daemon": {"path": DAEMON_PLIST, "memory_limit_kib": MEMORY_LIMIT_KIB},
        "pack": {"source_launchd_arg": source_pack, "path": PACK_PATH, "mode": "0444"},
        "removed_base_entries": removed,
        "preserved_base_entries": len(original.keys() - set(removed) - {LAUNCHD_CACHE, LAUNCHD_SIGNATURE}),
        "legacy_entry_count": 0,
        "entries": {name: {"mode": f"{mode:04o}", "sha256": sha256(data)}
                    for name, (mode, data) in sorted(updated.items())},
        "trustcache": {"modules_before": len(previous_trust), "modules_after": len(previous_trust) + 1,
                       "registered_signatures": signatures},
        "outputs": {name: {"size_bytes": len(data), "sha256": sha256(data)} for name, data in outputs.items()},
    }
    return report, outputs


def checked_output_dir(raw: str) -> Path:
    original = Path.cwd() / raw
    actual = checked_path(raw, Path.cwd())
    require(actual == original and any(root in actual.parents for root in (CACHE_ROOT, HOST_TRIALS)),
            "output must be a new directory in the W12-L4 cache or I: host-trials")
    require(not any(part.casefold() == "payload.img" for part in actual.parts),
            "output may not address payload.img")
    require(not actual.exists(), f"output directory already exists: {actual}")
    return actual


def json_output(report: dict) -> str:
    return json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"


def write_modules(output: Path, files: dict[str, bytes], report: dict) -> None:
    require(files.keys() == {ADDON_OUTPUT, TRUST_OUTPUT}, "unexpected host module output name")
    output = checked_output_dir(str(output))
    output.mkdir()
    created = []
    try:
        for name, data in (*files.items(), ("audit.json", json_output(report).encode("utf-8"))):
            path = output / name
            with path.open("xb") as destination:
                created.append(path)
                destination.write(data)
            require(path.read_bytes() == data, f"host readback differs: {path}")
    except (OSError, ValueError):
        for path in reversed(created):
            path.unlink()
        output.rmdir()
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit-plan", required=True, metavar="PLAN_JSON")
    parser.add_argument("--out-dir", metavar="NEW_HOST_DIRECTORY")
    args = parser.parse_args(argv)
    try:
        output = checked_output_dir(args.out_dir) if args.out_dir is not None else None
        path = checked_path(args.audit_plan, Path.cwd())
        report, artifacts = audit_plan(parse_json(read_file(path)), path.parent)
        if output is not None:
            write_modules(output, artifacts, report)
    except (OSError, ValueError, OverflowError, RuntimeError, IndexError, struct.error, zlib.error,
            plistlib.InvalidFileException, ExpatError) as exc:
        report = {"schema_version": 1, "status": "FAIL", **HOST_FLAGS,
                  "phone_operations": False, "error": str(exc)}
    print(json_output(report), end="")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
