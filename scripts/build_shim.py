#!/usr/bin/env python3
"""Build and independently check the cas-only position-independent ARM64 shim."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path


def run(command, log):
    result = subprocess.run([str(p) for p in command], text=True, capture_output=True)
    with log.open("a") as stream:
        stream.write("COMMAND " + repr([str(p) for p in command]) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}); see {log}")
    return result.stdout


def build(uefi, out, *, diagnostics=False):
    out.mkdir(parents=True, exist_ok=False)
    log = out / "commands.log"
    root = uefi.resolve()
    include = root / "Platforms/Xiaomi/casPkg/Include"
    display = root / "Platforms/Xiaomi/casPkg/Library/CasDisplayLib"
    sources = [root / "BootShim/CasBootShim.S", root / "BootShim/CasBootValidate.c",
               display / "DisplayDecode.c", display / "DisplayHardware.c"]
    objects = []
    for source in sources:
        obj = out / (source.stem + ".o")
        flags = ["--target=aarch64-none-elf", "-ffreestanding", "-fpie", "-fno-stack-protector", "-fno-builtin",
                 "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-mgeneral-regs-only",
                 # The MMU is off and SCTLR.A may be set. FDT values are only
                 # four-byte aligned, including fields decoded as 64 bits.
                 "-mstrict-align", "-O2", "-Wall", "-Wextra", "-Werror"]
        if diagnostics:
            flags += ["-DCAS_XNU_DIAGNOSTICS=1"]
        run(["clang", *flags, "-I", include, "-c", source, "-o", obj], log)
        objects.append(obj)
    elf = out / "shim.elf"
    run(["ld.lld", "-T", root / "BootShim/CasBootShim.lds", "--no-undefined", "-Map=" + str(out / "shim.map"),
         *objects, "-o", elf], log)
    run(["llvm-objcopy", "-O", "binary", elf, out / "shim.bin"], log)
    symbols = run(["llvm-nm", "-n", elf], log)
    parsed = {line.split()[2]: int(line.split()[0], 16) for line in symbols.splitlines() if len(line.split()) == 3}
    relocations = run(["llvm-readelf", "--relocs", elf], log)
    if re.search(r"R_AARCH64_", relocations):
        raise ValueError("unresolved shim relocations")
    data = (out / "shim.bin").read_bytes()
    if parsed["_Head"] != 0 or parsed["_Payload"] != len(data) or len(data) > 65536:
        raise ValueError("shim start/payload/literal-pool boundary mismatch")
    if data[56:60] != b"ARM\x64" or struct.unpack_from("<QQ", data, 8) != (0x80000, len(data) + 0x800000):
        raise ValueError("incorrect ARM64 text_offset/image_size")
    if parsed["_ScratchStackTop"] - parsed["_ScratchStackBase"] != 0x4000:
        raise ValueError("scratch stack size mismatch")
    report = {"result": "pass", "hardware_proven": False, "diagnostics": diagnostics, "shim_bytes": len(data),
              "sha256": hashlib.sha256(data).hexdigest(), "text_offset": "0x80000", "image_size": len(data) + 0x800000,
              "symbols": parsed, "relocations": relocations.strip(), "sources": [
                  {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in
                  [*sources, include / "CasPlatform.h", include / "CasDisplay.h", root / "BootShim/CasBootShim.lds"]]}
    (out / "check.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("result", "hardware_proven", "shim_bytes", "sha256", "text_offset", "image_size")}, indent=2))
    return out / "shim.bin"


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--uefi", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--diagnostics", action="store_true")
    options = ap.parse_args()
    build(options.uefi, options.out, diagnostics=options.diagnostics)
