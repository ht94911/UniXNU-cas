"""cas ADT generation, source traceability and static consumer contracts.

NVRAM layout and early-tree structure follow the cited UniXNU-umi/sheng
references; all physical values come from the cas facts file.
Copyright (c) 2026 UniXNU contributors.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import zlib

from adt_codec import Node, decode, string, strings, u32, u64

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN = {
    "unique-chip-id", "chip-id", "serial-number", "mlb-serial-number",
    "platform-uuid", "mac-address", "local-mac-address", "root-matching",
    "boot-uuid", "boot-manifest-hash", "boot-nonce", "RAMDisk", "PackedRAMDisk",
    "TrustCache", "GpuAddon",
}



def private_directory(path):
    """Create a new local directory only where POSIX access modes take effect."""
    path = Path(path)
    if os.name != "posix":
        raise ValueError("private output requires POSIX permissions; use a WSL Linux filesystem")
    path.mkdir(mode=0o700, parents=True, exist_ok=False)
    path.chmod(0o700)
    if path.stat().st_mode & 0o077:
        raise ValueError("private directory permissions are not enforced; use a WSL Linux filesystem")


def private_write(path, raw):
    """Refuse a filesystem that makes a private file readable to other users."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as stream:
        if os.fstat(stream.fileno()).st_mode & 0o077:
            raise ValueError("private file permissions are not enforced")
        stream.write(raw)

def _keys(value, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ValueError(f"unexpected object keys; expected {sorted(keys)}")


def _int(value, bits=64, minimum=0):
    if type(value) is not int or not minimum <= value < 1 << bits:
        raise ValueError(f"expected u{bits} integer >= {minimum}")
    return value


def _cstring(value):
    if not value.endswith(b"\0") or b"\0" in value[:-1]:
        raise ValueError("expected one terminated string")
    result = value[:-1].decode("ascii")
    string(result)
    return result


def _json_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def load_json(path):
    raw = Path(path).read_bytes()
    if len(raw) > 1 << 20:
        raise ValueError("JSON byte limit")
    def constant(_):
        raise ValueError("non-finite JSON constant")
    try:
        return json.loads(raw, object_pairs_hook=_json_pairs, parse_constant=constant)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("invalid JSON") from error


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_schema(path=ROOT / "config/cas-adt-schema.json"):
    schema = load_json(path)
    if (schema.get("schema_version"), schema.get("endianness"), schema.get("alignment")) != (1, "little", 4):
        raise ValueError("unsupported ADT schema")
    if schema.get("property_name_bytes") != 32 or schema.get("limits") != {
        "blob_bytes": 1048576, "depth": 32, "nodes": 1024,
        "properties_per_node": 128, "children_per_node": 256, "node_name_bytes": 63,
    }:
        raise ValueError("schema does not match bounded codec")
    if schema.get("private_only_optional_properties") != {"/chosen": {"random-seed": "64bytes"}}:
        raise ValueError("unsupported private property contract")
    required = {"/", "/chosen", "/chosen/memory-map", "/cpus", "/arm-io", "/arm-io/intc",
                "/psci", "/timer", "/memory", "/reserved-memory", "/defaults", "/options"}
    if not required <= schema["nodes"].keys():
        raise ValueError("schema omits a required consumer node")
    return schema


def _interval(item, alignment=4096):
    base, size = _int(item["base"]), _int(item["size"], minimum=1)
    if base % alignment or size % alignment or base + size >= 1 << 64:
        raise ValueError("unaligned, empty or overflowing range")
    return base, base + size


def _overlap(left, right):
    return left[0] < right[1] and right[0] < left[1]


def _disjoint(items):
    ranges = sorted((_interval(item), item.get("name", "bank")) for item in items)
    for (left, left_name), (right, right_name) in zip(ranges, ranges[1:]):
        if _overlap(left, right):
            raise ValueError(f"overlapping ranges: {left_name} and {right_name}")


def validate_facts(facts):
    _keys(facts, {"schema_version", "board", "profile_id", "observed_at", "evidence_scope",
                  "sources", "model", "compatible", "cpus", "psci", "timer", "gic",
                  "memory_banks", "reservations", "mmio", "framebuffer", "owned_memory"})
    if type(facts["schema_version"]) is not int or facts["schema_version"] != 1 or facts["board"] != "cas":
        raise ValueError("not a cas facts profile")
    for key in ("profile_id", "observed_at", "evidence_scope", "model"):
        string(facts[key])
    strings(facts["compatible"])
    _keys(facts["sources"], {"board", "memory", "display"})
    for source in facts["sources"].values():
        _keys(source, {"kind", "path", "sha256"})
        path = PurePosixPath(source["path"])
        if (source["kind"] not in ("management", "repository") or path.is_absolute()
                or ".." in path.parts or "\\" in source["path"] or not path.parts
                or not re.fullmatch("[0-9a-f]{64}", source["sha256"])):
            raise ValueError("invalid source reference")
    cpus = facts["cpus"]
    if not isinstance(cpus, list) or len(cpus) != 8:
        raise ValueError("cas requires eight observed affinity entries")
    for cpu in cpus:
        _keys(cpu, {"affinity", "compatible"})
        _int(cpu["affinity"], 32)
        strings(cpu["compatible"])
    if [cpu["affinity"] for cpu in cpus] != [i << 8 for i in range(8)]:
        raise ValueError("unexpected cas affinity ordering")
    _keys(facts["psci"], {"compatible", "method"})
    if facts["psci"] != {"compatible": "arm,psci-1.0", "method": "smc"}:
        raise ValueError("cas PSCI must retain the observed SMC conduit")
    timer = facts["timer"]
    _keys(timer, {"compatible", "frequency_hz", "interrupt_cells", "virtual_ppi"})
    _int(timer["frequency_hz"], 32, 1)
    if timer["compatible"] != "arm,armv8-timer" or timer["virtual_ppi"] != 27:
        raise ValueError("fixed ArmCpu virtual timer contract differs")
    cells = timer["interrupt_cells"]
    if not isinstance(cells, list) or len(cells) != 12:
        raise ValueError("expected four FDT timer interrupt triples")
    for cell in cells:
        _int(cell, 32)
    if any(cells[i] != 1 or cells[i + 1] > 15 for i in range(0, 12, 3)) or cells[7] + 16 != 27:
        raise ValueError("timer PPI cells do not match ArmCpu INTID 27")
    gic = facts["gic"]
    _keys(gic, {"compatible", "distributor", "redistributor", "redistributor_stride"})
    if gic["compatible"] != "arm,gic-v3":
        raise ValueError("ArmCpu requires arm,gic-v3")
    for region in (gic["distributor"], gic["redistributor"]):
        _keys(region, {"base", "size"})
        _interval(region)
    stride = _int(gic["redistributor_stride"], minimum=1)
    if stride not in (0x20000, 0x30000, 0x40000) or gic["redistributor"]["size"] // stride < len(cpus):
        raise ValueError("redistributor geometry cannot describe the eight hardware CPUs")
    if gic["distributor"]["size"] < 0x10000:
        raise ValueError("distributor range too small")
    banks = facts["memory_banks"]
    if not isinstance(banks, list) or not 1 <= len(banks) <= 16:
        raise ValueError("invalid bank list")
    for bank in banks:
        _keys(bank, {"base", "size"})
        _interval(bank)
    _disjoint(banks)
    _int(sum(bank["size"] for bank in banks), minimum=1)
    if [bank["base"] for bank in banks] != sorted(bank["base"] for bank in banks):
        raise ValueError("memory banks must be sorted")
    reservations, mmio = facts["reservations"], facts["mmio"]
    for items, roles in ((reservations, {"protected", "firmware"}), (mmio, {"mmio"})):
        if not isinstance(items, list) or not 1 <= len(items) <= 128:
            raise ValueError("invalid reservation/MMIO list")
        for item in items:
            _keys(item, {"name", "base", "size", "role"})
            string(item["name"])
            _interval(item)
            if item["role"] not in roles:
                raise ValueError("incorrect range role")
        if len({item["name"] for item in items}) != len(items):
            raise ValueError("duplicate range name")
        _disjoint(items)
    for item in reservations:
        r = _interval(item)
        if not any(bank["base"] <= r[0] and r[1] <= bank["base"] + bank["size"] for bank in banks):
            raise ValueError("reservation outside a single observed RAM bank")
    for item in mmio:
        if any(_overlap(_interval(item), _interval(bank)) for bank in banks):
            raise ValueError("MMIO overlaps RAM")
    for key, name in (("distributor", "gicd"), ("redistributor", "gicr")):
        matches = [item for item in mmio if item["name"] == name]
        if len(matches) != 1 or _interval(matches[0]) != _interval(gic[key]):
            raise ValueError("GIC ranges differ from the frozen MMIO design")
    fb = facts["framebuffer"]
    _keys(fb, {"width", "height", "enclosure_name", "base", "stride", "format", "handoff", "state"})
    _int(fb["width"], 32, 1); _int(fb["height"], 32, 1)
    if (fb["state"] != "runtime-required" or fb["handoff"] != "boot_args.Video from current GOP"
            or any(fb[key] is not None for key in ("base", "stride", "format"))):
        raise ValueError("framebuffer must be discovered from current GOP; no fixed address/format")
    splash = [item for item in reservations if item["name"] == fb["enclosure_name"] and item["role"] == "protected"]
    if len(splash) != 1:
        raise ValueError("framebuffer lacks a conservative protected enclosure")
    owned = facts["owned_memory"]
    _keys(owned, {"candidate_base", "candidate_size", "request_size", "state", "basis"})
    if owned["state"] != "candidate":
        raise ValueError("historical profile cannot assert runtime memory qualification")
    string(owned["basis"])
    candidate = {"base": owned["candidate_base"], "size": owned["candidate_size"]}
    cr = _interval(candidate, 0x200000)
    request = _int(owned["request_size"], minimum=0x10000000)
    if request > min(0x200000000, candidate["size"]) or request % 0x200000:
        raise ValueError("loader request must be 256 MiB..8 GiB, in 2 MiB steps, within candidate")
    if not any(bank["base"] <= cr[0] and cr[1] <= bank["base"] + bank["size"] for bank in banks):
        raise ValueError("candidate crosses a RAM bank or gap")
    if any(_overlap(cr, _interval(item)) for item in reservations + mmio):
        raise ValueError("candidate overlaps a reservation or MMIO")
    return {
        "bank_count": len(banks), "bank_total": sum(bank["size"] for bank in banks),
        "reservation_count": len(reservations), "mmio_count": len(mmio),
        "candidate": {"base": hex(cr[0]), "end_exclusive": hex(cr[1]), "request_size": hex(request)},
        "ranges_static_pass": True, "current_memory_qualified": False,
        "framebuffer": "Entire historical splash excluded; actual base/stride/format require current GOP.",
    }


def empty_nvram_proxy():
    """Empty, deterministic CHRP v1 bank; contains no saved device variables."""
    def header(name, length):
        raw = bytearray(struct.pack("<BBH12s", 0x70, 0, length // 16, name))
        total = sum(raw)
        while total > 255:
            total = (total & 255) + (total >> 8)
        raw[1] = total
        return raw
    image = bytearray(8192)
    image[:16] = header(b"nvram", 32)
    image[32:48] = header(b"common", 2064)
    image[2096:2112] = header(b"free", 8192 - 2096)
    struct.pack_into("<I", image, 16, zlib.adler32(image[20:]))
    return bytes(image)


def validate_nvram(image):
    if type(image) is not bytes or len(image) != 8192:
        raise ValueError("NVRAM bank must be exactly 8192 bytes")
    if struct.unpack_from("<I", image, 16)[0] != zlib.adler32(image[20:]):
        raise ValueError("NVRAM Adler32 mismatch")
    if any(image[20:32]):
        raise ValueError("unexpected generation or private NVRAM header bytes")
    offset, partitions = 0, []
    while offset < len(image):
        sig, check, blocks, name_raw = struct.unpack_from("<BBH12s", image, offset)
        checksum = sig + sum(image[offset + 2:offset + 16])
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        if sig != 0x70 or check != checksum or blocks == 0:
            raise ValueError("invalid CHRP signature, checksum or zero length")
        name, terminator, pad = name_raw.partition(b"\0")
        if not terminator or any(pad):
            raise ValueError("invalid CHRP partition name")
        end = offset + blocks * 16
        if end > len(image):
            raise ValueError("CHRP partition exceeds bank")
        partitions.append((name, offset, end))
        if name != b"nvram" and any(image[offset + 16:end]):
            raise ValueError("NVRAM proxy must contain no private variable data")
        offset = end
    if partitions != [(b"nvram", 0, 32), (b"common", 32, 2096), (b"free", 2096, 8192)]:
        raise ValueError("unexpected CHRP empty-bank partition layout")
    return {"version": "CHRP v1", "bank_bytes": 8192, "variables": 0, "checksums_valid": True}


def _pack_ranges(items):
    return b"".join(u64(item["base"]) + u64(item["size"]) for item in items)


def build_tree(facts, *, cpu_profile="cpu0", mode="fixture"):
    validate_facts(facts)
    if cpu_profile not in ("cpu0", "all") or mode not in ("fixture", "private"):
        raise ValueError("unsupported CPU profile or output mode")
    trace = {}
    def node(path, properties=None, children=None, basis="schema", pointer="", status="policy"):
        name = "device-tree" if path == "/" else path.rsplit("/", 1)[1]
        props = properties or {}
        for key in ("name", *props):
            trace[path + "::" + key] = {"basis": basis, "pointer": pointer, "status": status}
        return Node(name, props, children or [])
    def source(path, key, pointer, transform="identity", status="historical"):
        trace[path + "::" + key] = {"basis": "cas-facts", "pointer": pointer, "transform": transform, "status": status}
    banks, gic, timer = facts["memory_banks"], facts["gic"], facts["timer"]
    selected = facts["cpus"] if cpu_profile == "all" else facts["cpus"][:1]
    cpu_nodes = []
    for index, cpu in enumerate(selected):
        path = f"/cpus/cpu{index}"
        cpu_nodes.append(node(path, {"device_type": string("cpu"), "state": string("running" if index == 0 else "waiting"),
                                    "reg": u32(cpu["affinity"]), "compatible": strings(cpu["compatible"]),
                                    "timebase-frequency": u32(timer["frequency_hz"])}))
        source(path, "reg", f"/cpus/{index}/affinity", "u32 little-endian")
        source(path, "compatible", f"/cpus/{index}/compatible", "NUL-terminated string list")
        source(path, "timebase-frequency", "/timer/frequency_hz", "u32 little-endian")
    chosen = {"osenvironment": string("main"), "dram-base": u64(min(bank["base"] for bank in banks)),
              "dram-size": u64(sum(bank["size"] for bank in banks)),
              "darwinboot-memory-size": u64(facts["owned_memory"]["request_size"]),
              "nvram-bank-size": u32(8192), "nvram-bank-count": u32(1), "nvram-current-bank": u32(0),
              "nvram-proxy-data": empty_nvram_proxy()}
    if mode == "private":
        chosen["random-seed"] = os.urandom(64)
        if len(chosen["random-seed"]) != 64 or not any(chosen["random-seed"]):
            raise ValueError("OS entropy source failed")
    dist, redist = gic["distributor"], gic["redistributor"]
    bus_base = min(dist["base"], redist["base"])
    bus_size = max(dist["base"] + dist["size"], redist["base"] + redist["size"]) - bus_base
    reservations = sorted(enumerate(facts["reservations"]), key=lambda pair: (pair[1]["base"], pair[1]["name"]))
    reserve_nodes = []
    for index, (fact_index, region) in enumerate(reservations):
        path = f"/reserved-memory/region{index}"
        reserve_nodes.append(node(path, {"reg": _pack_ranges([region])}))
        source(path, "reg", f"/reservations/{fact_index}", "base,size u64 little-endian")
    children = [
        node("/chosen", chosen, [node("/chosen/memory-map")]),
        node("/cpus", children=cpu_nodes), node("/defaults", {"sleep-disabled": u32(1)}), node("/options"),
        node("/arm-io", {"device_type": string("arm-io"), "#address-cells": u32(2), "#size-cells": u32(2),
                         "ranges": struct.pack("<QQQ", 0, bus_base, bus_size)},
             [node("/arm-io/intc", {"compatible": string(gic["compatible"]),
                                  "reg": struct.pack("<QQQQ", dist["base"] - bus_base, dist["size"], redist["base"] - bus_base, redist["size"]),
                                  "redistributor-stride": u64(gic["redistributor_stride"])})]),
        node("/psci", {key: string(value) for key, value in facts["psci"].items()}),
        node("/timer", {"compatible": string(timer["compatible"]), "clock-frequency": u32(timer["frequency_hz"]),
                        "interrupts": b"".join(u32(cell) for cell in timer["interrupt_cells"])}),
        node("/memory", {"device_type": string("memory"), "reg": _pack_ranges(banks)}),
        node("/reserved-memory", {"#address-cells": u32(2), "#size-cells": u32(2), "ranges": b""}, reserve_nodes),
    ]
    tree = node("/", {"model": string(facts["model"]), "compatible": strings(facts["compatible"]),
                      "#address-cells": u32(2), "#size-cells": u32(2)}, children)
    source("/", "model", "/model"); source("/", "compatible", "/compatible", "NUL-terminated string list")
    source("/chosen", "dram-base", "/memory_banks", "minimum base; informational")
    source("/chosen", "dram-size", "/memory_banks", "sum(size); informational; never free memory")
    source("/chosen", "darwinboot-memory-size", "/owned_memory/request_size", "u64 requested allocation; no fixed base", "candidate")
    for key in ("nvram-bank-size", "nvram-bank-count", "nvram-current-bank", "nvram-proxy-data"):
        trace["/chosen::" + key] = {"basis": "empty-CHRP-v1", "pointer": "schema/policies/nvram_bank_bytes", "status": "software-input"}
    if mode == "private":
        trace["/chosen::random-seed"] = {"basis": "OS CSPRNG", "pointer": "", "status": "fresh-private; value never included in report"}
    source("/arm-io", "ranges", "/gic", "logical bus window enclosing GICD/GICR only")
    for key, pointer, transform in [("compatible", "/gic/compatible", "string"), ("reg", "/gic", "relative base,size pairs u64"),
                                    ("redistributor-stride", "/gic/redistributor_stride", "u64 little-endian")]:
        source("/arm-io/intc", key, pointer, transform)
    for key in facts["psci"]:
        source("/psci", key, "/psci/" + key)
    for key, pointer in [("compatible", "compatible"), ("clock-frequency", "frequency_hz"), ("interrupts", "interrupt_cells")]:
        source("/timer", key, "/timer/" + pointer, "informational; ArmCpu directly uses architectural virtual timer INTID 27")
    source("/memory", "reg", "/memory_banks", "base,size u64 pairs; informational")
    return tree, trace


def _check_type(value, kind):
    if type(value) is not bytes:
        raise ValueError("non-byte property")
    if kind == "cstring":
        _cstring(value)
    elif kind == "strings":
        if not value.endswith(b"\0") or not value[:-1]:
            raise ValueError("invalid string list")
        for item in value[:-1].split(b"\0"):
            string(item.decode("ascii"))
    elif kind == "nvram":
        validate_nvram(value)
    elif kind == "ranges64":
        if not value or len(value) % 16:
            raise ValueError("invalid base,size u64 array")
    else:
        lengths = {"u32": 4, "u64": 8, "2xu64": 16, "3xu64": 24, "4xu64": 32,
                   "12xu32": 48, "empty": 0, "64bytes": 64}
        if kind not in lengths or len(value) != lengths[kind]:
            raise ValueError("wrong integer/property width")


def audit_tree(tree, facts, schema, *, cpu_profile="cpu0", mode="fixture"):
    ranges = validate_facts(facts)
    if cpu_profile not in ("cpu0", "all") or mode not in ("fixture", "private"):
        raise ValueError("unknown audit mode")
    # Canonical reparse also bounds programmatically supplied trees before walking.
    tree = decode(tree.encode())
    seen = set()
    property_count = 0
    for path, item in tree.walk():
        matches = [rule for pattern, rule in schema["nodes"].items() if re.fullmatch(pattern, path)]
        if len(matches) != 1:
            raise ValueError(f"unknown/ambiguous node contract {path}")
        rule = dict(matches[0])
        if mode == "private" and path == "/chosen":
            rule["random-seed"] = "64bytes"
        if set(item.properties) != set(rule) or (FORBIDDEN | set(schema["forbidden_properties"])) & item.properties.keys():
            raise ValueError(f"missing, forbidden or unexpected property {path}")
        for key, kind in rule.items():
            _check_type(item.properties[key], kind)
        property_count += len(rule) + 1
        seen.add(path)
    for path in ("/", "/chosen", "/chosen/memory-map", "/cpus", "/arm-io", "/arm-io/intc", "/psci", "/timer", "/memory", "/reserved-memory", "/defaults", "/options"):
        if path not in seen:
            raise ValueError(f"required node missing: {path}")
    def expect(path, key, value):
        if tree.at(path).properties[key] != value:
            raise ValueError(f"property differs from cas facts/policy: {path}::{key}")
    expect("/", "model", string(facts["model"]))
    expect("/", "compatible", strings(facts["compatible"]))
    for path in ("/", "/arm-io", "/reserved-memory"):
        for key in ("#address-cells", "#size-cells"):
            expect(path, key, u32(2))
    chosen = tree.at("/chosen").properties
    for key, value in {"osenvironment": string("main"), "dram-base": u64(facts["memory_banks"][0]["base"]),
                       "dram-size": u64(ranges["bank_total"]), "darwinboot-memory-size": u64(facts["owned_memory"]["request_size"]),
                       "nvram-bank-size": u32(8192), "nvram-bank-count": u32(1), "nvram-current-bank": u32(0)}.items():
        expect("/chosen", key, value)
    if mode == "private" and not any(chosen["random-seed"]):
        raise ValueError("zero entropy is forbidden")
    cpus = tree.at("/cpus").children
    selected = facts["cpus"] if cpu_profile == "all" else facts["cpus"][:1]
    if len(cpus) != len(selected):
        raise ValueError("CPU profile count mismatch")
    for index, (cpu, fact) in enumerate(zip(cpus, selected)):
        if cpu.name != f"cpu{index}":
            raise ValueError("CPU names/order mismatch")
        for key, value in {"reg": u32(fact["affinity"]), "device_type": string("cpu"),
                           "state": string("running" if index == 0 else "waiting"), "compatible": strings(fact["compatible"]),
                           "timebase-frequency": u32(facts["timer"]["frequency_hz"])}.items():
            expect("/cpus/" + cpu.name, key, value)
    expect("/arm-io", "device_type", string("arm-io"))
    bus = struct.unpack("<QQQ", tree.at("/arm-io").properties["ranges"])
    regions = struct.unpack("<QQQQ", tree.at("/arm-io/intc").properties["reg"])
    if bus[0] != 0 or bus[1] + bus[2] >= 1 << 64:
        raise ValueError("invalid logical bus translation")
    for offset, region in zip((0, 2), (facts["gic"]["distributor"], facts["gic"]["redistributor"])):
        relative, size = regions[offset:offset + 2]
        if relative < bus[0] or relative - bus[0] > bus[2] or size > bus[2] - (relative - bus[0]):
            raise ValueError("GIC range outside bus")
        if (bus[1] + relative - bus[0], size) != (region["base"], region["size"]):
            raise ValueError("GIC physical translation mismatch")
    dist, redist = facts["gic"]["distributor"], facts["gic"]["redistributor"]
    minimum = min(dist["base"], redist["base"])
    envelope = max(dist["base"] + dist["size"], redist["base"] + redist["size"]) - minimum
    if bus != (0, minimum, envelope):
        raise ValueError("bus includes unexplained address space")
    expect("/arm-io/intc", "compatible", string(facts["gic"]["compatible"]))
    expect("/arm-io/intc", "redistributor-stride", u64(facts["gic"]["redistributor_stride"]))
    for key, value in facts["psci"].items():
        expect("/psci", key, string(value))
    expect("/timer", "compatible", string(facts["timer"]["compatible"]))
    expect("/timer", "clock-frequency", u32(facts["timer"]["frequency_hz"]))
    expect("/timer", "interrupts", b"".join(u32(cell) for cell in facts["timer"]["interrupt_cells"]))
    expect("/memory", "device_type", string("memory"))
    expect("/memory", "reg", _pack_ranges(facts["memory_banks"]))
    expected_reservations = sorted(facts["reservations"], key=lambda region: (region["base"], region["name"]))
    children = tree.at("/reserved-memory").children
    if len(children) != len(expected_reservations):
        raise ValueError("reservation count differs")
    for index, (child, region) in enumerate(zip(children, expected_reservations)):
        if child.name != f"region{index}":
            raise ValueError("reservation names/order differ")
        expect("/reserved-memory/" + child.name, "reg", _pack_ranges([region]))
    expect("/defaults", "sleep-disabled", u32(1))
    return {
        "result": "pass", "board": "cas", "cpu_profile": cpu_profile, "mode": mode,
        "nodes": len(seen), "properties_including_names": property_count,
        "cpu_affinities": [cpu["affinity"] for cpu in selected], "timebase_hz": facts["timer"]["frequency_hz"],
        "gic": facts["gic"], "nvram": validate_nvram(chosen["nvram-proxy-data"]), "ranges": ranges,
        "entropy_bytes": len(chosen.get("random-seed", b"")), "identity_fields": [], "root_volume_fields": [],
        "boot_ready": False, "hardware_tested": False,
        "pending": ["X-05 current FDT/SMEM/UEFI owned-memory validation", "current GOP boot_args.Video and exception level",
                    "X-06 loader/kernel integration", "XNU runtime verification"] +
                   (["Fresh private entropy required; fixture must never be booted"] if mode == "fixture" else []) +
                   (["Eight-CPU tree is host-audit only; SMP candidate requires X-11/X-12"] if cpu_profile == "all" else []),
    }


def audit_sources(facts, repository=ROOT, evidence_root=None):
    """Check pinned source bytes and independently derive selected cas values."""
    validate_facts(facts)
    sources = {}
    for key, source in facts["sources"].items():
        if source["kind"] == "management" and evidence_root is None:
            raise ValueError("management evidence root required for source verification")
        root = Path(evidence_root) if source["kind"] == "management" else Path(repository)
        path = (root / source["path"]).resolve()
        if not path.is_relative_to(root.resolve()) or sha256(path) != source["sha256"]:
            raise ValueError("source path/hash mismatch")
        sources[key] = load_json(path)
    board, memory, display = sources["board"], sources["memory"], sources["display"]
    if board["board"] != "cas" or memory["board"] != "cas" or display["board"] != "cas":
        raise ValueError("source evidence is not cas")
    def number(value):
        if type(value) is int:
            return _int(value)
        if isinstance(value, str) and re.fullmatch(r"0x[0-9a-fA-F]+", value):
            return _int(int(value, 16))
        raise ValueError("unexpected source integer")
    def regions(items, role):
        return [{"name": item.get("name", item.get("source_node")), "base": number(item["base"]),
                 "size": number(item["size"]), "role": role} for item in items]
    def props(node):
        return board["nodes"][node]["properties"]
    cpus = []
    for path, node in board["nodes"].items():
        if path.startswith("/cpus/cpu@"):
            reg = node["properties"]["reg"]
            cpus.append({"affinity": (number(reg[0]) << 32) | number(reg[1]), "compatible": node["properties"]["compatible"]})
    gic = props("/soc/interrupt-controller@17a00000")
    timer = props("/soc/timer")
    expected = {
        "model": props("/")["model"][0], "compatible": props("/")["compatible"],
        "cpus": sorted(cpus, key=lambda cpu: cpu["affinity"]),
        "psci": {"compatible": props("/psci")["compatible"][0], "method": props("/psci")["method"][0]},
        "timer": {"compatible": timer["compatible"][0], "frequency_hz": number(timer["clock-frequency"][0]),
                  "interrupt_cells": [number(cell) for cell in timer["interrupts"]], "virtual_ppi": 16 + number(timer["interrupts"][7])},
        "gic": {"compatible": gic["compatible"][0], "distributor": {"base": number(gic["reg"][0]), "size": number(gic["reg"][1])},
                "redistributor": {"base": number(gic["reg"][2]), "size": number(gic["reg"][3])},
                "redistributor_stride": (number(gic["redistributor-stride"][0]) << 32) | number(gic["redistributor-stride"][1])},
        "memory_banks": [{"base": number(bank["base"]), "size": number(bank["size"])} for bank in board["memory_banks"]],
        "reservations": regions(memory["protected"], "protected") + regions(memory["regions"], "firmware"),
        "mmio": regions(memory["mmio"], "mmio"),
    }
    for key, value in expected.items():
        if facts[key] != value:
            raise ValueError(f"facts differ from pinned cas evidence: {key}")
    if facts["observed_at"] != board["observed_at"]:
        raise ValueError("observation timestamp differs")
    if (facts["framebuffer"]["width"], facts["framebuffer"]["height"]) != (display["width"], display["height"]):
        raise ValueError("framebuffer geometry differs from cas design")
    return {"result": "pass", "sources": facts["sources"], "matched_sections": list(expected),
            "candidate_sections": ["owned_memory", "framebuffer runtime fields"], "current_device_observation": False}
