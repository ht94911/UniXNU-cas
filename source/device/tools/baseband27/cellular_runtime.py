#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Stage one cellular launch job in a locally supplied original iOS27 cache."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct

LABEL = "org.unixnu.cellular-demand021"
JOB_KEY = "/System/Library/LaunchDaemons/" + LABEL + ".plist"
HELPER_SHA256 = "73a53ddc7724db130cf538a53ccb5cad0f3ff204a7aa791cce25e9fbd7329eca"
CACHE_SHA256 = "f9e009500651792dffabb6bc218efcd02b0b960b5b645bbe8c0a0b7be5863ef3"
JOB = {
    "Label": LABEL,
    "ProgramArguments": ["/usr/libexec/cellular-demand021"],
    "RunAtLoad": True,
    "LaunchOnlyOnce": True,
    # The native initial launch condition schedules the job; LaunchOnlyOnce
    # removes it after its first exit, including an unsuccessful request.
    "KeepAlive": True,
    "UserName": "root",
    "LimitLoadToSessionType": "System",
    "ProcessType": "Background",
    "StandardOutPath": "/dev/null",
    "StandardErrorPath": "/dev/null",
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def counted(marker, count):
    if count < 15:
        return bytes([marker | count])
    width = next(size for size in (1, 2, 4, 8) if count < 1 << (size * 8))
    return bytes([marker | 15, 0x10 | (width.bit_length() - 1)]) + count.to_bytes(width, "big")


def dictionary_refs(data, offset, ref_size):
    marker = data[offset]
    if marker >> 4 != 13:
        raise ValueError("Expected the original cache dictionary")
    count, at = marker & 15, offset + 1
    if count == 15:
        width = 1 << (data[at] & 15)
        if data[at] >> 4 != 1 or width > 8:
            raise ValueError("Invalid cache dictionary count")
        count = int.from_bytes(data[at + 1:at + 1 + width], "big")
        at += 1 + width
    refs = [int.from_bytes(data[at + i * ref_size:at + (i + 1) * ref_size], "big")
            for i in range(count * 2)]
    return refs[:count], refs[count:], at


def merge_cache(source):
    """Preserve original object bytes and its four-byte reference representation."""
    original = plistlib.loads(source)
    if source[:8] != b"bplist00" or original.get("VersionNumber") != 7:
        raise ValueError("Expected the selected iOS27 binary service cache")
    offset_size, ref_size, count, root, table = struct.unpack(">6xBBQQQ", source[-32:])
    if (offset_size, ref_size) != (4, 4) or root >= count:
        raise ValueError("Expected the original four-byte cache tables")
    offsets = [int.from_bytes(source[table + 4 * i:table + 4 * i + 4], "big") for i in range(count)]
    jobs = original["LaunchDaemons"]
    if JOB_KEY in jobs or any(job.get("Label") == LABEL for job in jobs.values()):
        raise ValueError("Cellular job already exists; start from the original cache")
    keys, values, root_at = dictionary_refs(source, offsets[root], ref_size)
    position = list(original).index("LaunchDaemons")
    job_keys, job_values, _ = dictionary_refs(source, offsets[values[position]], ref_size)
    if len(job_keys) != len(jobs):
        raise ValueError("Original job count differs from the cached dictionary")
    body = bytearray(source[:table])

    def append(value):
        if isinstance(value, dict):
            names = [append(key) for key in value]
            fields = [append(item) for item in value.values()]
            encoded = counted(0xd0, len(names)) + b"".join(struct.pack(">I", i) for i in names + fields)
        elif isinstance(value, list):
            fields = [append(item) for item in value]
            encoded = counted(0xa0, len(fields)) + b"".join(struct.pack(">I", i) for i in fields)
        elif isinstance(value, str):
            raw = value.encode("ascii")
            encoded = counted(0x50, len(raw)) + raw
        elif isinstance(value, bool):
            encoded = b"\x09" if value else b"\x08"
        else:
            raise ValueError("Unsupported owner launch job value")
        index = len(offsets)
        offsets.append(len(body))
        body.extend(encoded)
        return index

    key, job = append(JOB_KEY), append(JOB)
    new_jobs = len(offsets)
    offsets.append(len(body))
    body.extend(counted(0xd0, len(jobs) + 1))
    body.extend(b"".join(struct.pack(">I", i) for i in job_keys + [key] + job_values + [job]))
    root_slot = root_at + (len(keys) + position) * ref_size
    body[root_slot:root_slot + 4] = struct.pack(">I", new_jobs)
    new_table = len(body)
    body.extend(b"".join(struct.pack(">I", i) for i in offsets))
    body.extend(struct.pack(">6xBBQQQ", 4, 4, len(offsets), root, new_table))
    merged = bytes(body)
    decoded = plistlib.loads(merged)
    if decoded["LaunchDaemons"].pop(JOB_KEY) != JOB or decoded != original:
        raise ValueError("Original cache content changed outside the owner job")
    if merged[:root_slot] != source[:root_slot] or merged[root_slot + 4:table] != source[root_slot + 4:table]:
        raise ValueError("Original cache objects changed outside their dictionary reference")
    return merged


def detached_signature(data):
    """Use the original detached SuperBlob, SHA256 and 4KiB page-hash formats."""
    identifier = (LABEL + ".launchd-cache").encode() + b"\0"
    requirements = struct.pack(">III", 0xfade0c01, 12, 0)
    count = (len(data) + 4095) // 4096
    hash_offset = 96 + len(identifier) + 64
    cd = bytearray(struct.pack(
        ">9I4BI", 0xfade0c02, hash_offset + 32 * count, 0x20500, 2,
        hash_offset, 96, 2, count, len(data), 32, 2, 0, 12, 0,
    ))
    cd.extend(struct.pack(">IIIQQQQII", 0, 0, 0, 0, 0, 0, 0, 0, 0))
    cd.extend(identifier)
    cd.extend(hashlib.sha256(requirements).digest() + bytes(32))
    cd.extend(b"".join(hashlib.sha256(data[i:i + 4096]).digest() for i in range(0, len(data), 4096)))
    slots = [(0, bytes(cd)), (2, requirements), (0x10000, struct.pack(">II", 0xfade0b01, 8))]
    start = 12 + len(slots) * 8
    entries, content = [], bytearray()
    for kind, blob in slots:
        entries.append(struct.pack(">II", kind, start + len(content)))
        content.extend(blob)
    signature = struct.pack(">III", 0xfade0cc0, start + len(content), len(slots))
    signature += b"".join(entries) + content
    return signature, hashlib.sha256(cd).hexdigest()[:40]


def verify_signature(data, signature):
    """Check the supplied detached format and every page against its CodeDirectory."""
    magic, length, count = struct.unpack_from(">III", signature)
    if magic != 0xfade0cc0 or length != len(signature) or count > 16:
        raise ValueError("Invalid detached signature")
    blobs = {}
    for i in range(count):
        kind, offset = struct.unpack_from(">II", signature, 12 + 8 * i)
        size = struct.unpack_from(">I", signature, offset + 4)[0]
        if offset + size > len(signature) or kind in blobs:
            raise ValueError("Invalid detached signature slot")
        blobs[kind] = signature[offset:offset + size]
    cd = blobs[0]
    fields = struct.unpack_from(">9I4BI", cd)
    magic, length, version, flags, offset, ident, special, pages, limit, size, typ, platform, shift, spare = fields
    if magic != 0xfade0c02 or length != len(cd) or typ != 2 or size != 32 or shift != 12:
        raise ValueError("Unexpected selected CodeDirectory format")
    if limit != len(data) or pages != (limit + 4095) // 4096 or offset + size * pages != len(cd):
        raise ValueError("Detached signature coverage differs from the cache")
    for i in range(pages):
        if cd[offset + i * 32:offset + (i + 1) * 32] != hashlib.sha256(data[i * 4096:(i + 1) * 4096]).digest():
            raise ValueError("Detached signature page hash mismatch")
    if special >= 2 and cd[offset - 64:offset - 32] != hashlib.sha256(blobs[2]).digest():
        raise ValueError("Detached signature requirements hash mismatch")
    return hashlib.sha256(cd).hexdigest()[:40]


def stage(cache, original_signature, helper, output):
    raw, signature, binary = cache.read_bytes(), original_signature.read_bytes(), helper.read_bytes()
    if sha(raw) != CACHE_SHA256 or sha(binary) != HELPER_SHA256:
        raise ValueError("Expected the pinned original iOS27 cache and existing signed helper")
    verify_signature(raw, signature)
    output.mkdir(parents=True, exist_ok=False)
    (output / "cellular-xpc").mkdir()
    (output / "cellular-bin").mkdir()
    (output / "daemons").mkdir()
    merged = merge_cache(raw)
    signed, cdhash = detached_signature(merged)
    if verify_signature(merged, signed) != cdhash:
        raise ValueError("Owner cache signature differs")
    changed = bytearray(merged)
    changed[100] ^= 1
    try:
        verify_signature(changed, signed)
    except ValueError:
        pass
    else:
        raise ValueError("Changed owner cache page was accepted")
    files = {
        "cellular-xpc/launchd.plist": (0o644, merged),
        "cellular-xpc/launchd.plist.sig": (0o644, signed),
        "cellular-bin/cellular-demand021": (0o755, binary),
        "daemons/" + LABEL + ".plist": (0o644, plistlib.dumps(JOB)),
    }
    for name, (mode, content) in files.items():
        path = output / name
        path.write_bytes(content)
        path.chmod(mode)
    result = dict(
        source_cache_sha256=sha(raw), original_jobs=len(plistlib.loads(raw)["LaunchDaemons"]),
        original_jobs_preserved=True, original_object_bytes_preserved=True,
        original_ref_and_offset_sizes=[4, 4], owner_job=JOB, cdhash=cdhash,
        helper_byte_identical=True, helper_sha256=sha(binary), cache_sha256=sha(merged),
        signature_sha256=sha(signed), page_hash_check_passed=True, changed_page_rejected=True,
        target_Apple_loader_acceptance_pending=True, phone_operations=False,
        files={name: dict(mode=mode, bytes=len(content), sha256=sha(content))
               for name, (mode, content) in files.items()},
    )
    (output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original-cache", type=Path, required=True)
    parser.add_argument("--original-signature", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(stage(args.original_cache, args.original_signature, args.helper, args.output), indent=2))


if __name__ == "__main__":
    main()
