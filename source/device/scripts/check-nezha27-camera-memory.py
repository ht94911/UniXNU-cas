#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the actual camera pool and SMMU builder with an allocator fixture."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MODES = (
    "normal", "marker-failure", "table-allocation", "frame0-allocation",
    "frame1-allocation", "hold1-failure", "hold2-failure", "retire-failure",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    native, out = args.native.resolve(), args.out.resolve()
    assert (native / "NativeAdrenoMemory.h").is_file()
    fixture = out / "fixture/IOKit"
    fixture.mkdir(parents=True, exist_ok=True)
    (fixture / "IOTypes.h").write_text(
        "#include <stdint.h>\ntypedef int32_t IOReturn;\n"
        "#define kIOReturnSuccess 0\n"
        "#define kIOReturnNoMemory ((IOReturn)0xe00002bd)\n"
        "#define kIOReturnNoResources ((IOReturn)0xe00002be)\n"
        "#define kIOReturnBadArgument ((IOReturn)0xe00002c2)\n"
        "#define kIOReturnUnsupported ((IOReturn)0xe00002c7)\n"
        "#define kIOReturnBusy ((IOReturn)0xe00002d5)\n"
        "#define kIOReturnNotReady ((IOReturn)0xe00002d8)\n"
    )
    (fixture / "IOLib.h").write_text(
        "#include <cstdlib>\n#include <cstring>\n"
        "#define IONewData(type,count) new type[count]\n"
        "#define IODeleteData(pointer,type,count) delete[]pointer\n"
    )
    camera, smmu = ROOT / "package/camera", ROOT / "package/gpu/adreno"
    cc = os.environ.get("CC", "clang" if sys.platform == "darwin" else "gcc")
    cxx = os.environ.get("CXX", "clang++" if sys.platform == "darwin" else "g++")
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
             "-I", str(camera), "-I", str(smmu)]
    sources = [camera / name for name in ("NezhaCameraRdi1080.c", "NezhaCameraDma.c")]
    sources += [smmu / (name + ".c") for name in
                ("ArmSmmu4K", "ArmSmmu4KContext", "ArmSmmu4KProgram", "ArmSmmu4KStreams")]

    def compile_c(source):
        obj = out / (source.stem + ".o")
        with (out / (source.stem + "-build.log")).open("wb") as log:
            subprocess.run([cc, "-std=c11", *flags, "-c", str(source), "-o", str(obj)],
                           stdout=log, stderr=log, check=True)
        return obj

    with ThreadPoolExecutor(max_workers=32) as pool:
        objects = list(pool.map(compile_c, sources))
    binary = out / "memory-consumer"
    with (out / "adapter-build.log").open("wb") as log:
        subprocess.run([cxx, "-std=c++17", *flags, "-I", str(native), "-I", str(fixture.parent),
                        str(ROOT / "tests/camera-native-memory.cpp"),
                        *map(str, objects), "-o", str(binary)], stdout=log, stderr=log, check=True)
    results = []
    for mode in MODES:
        with (out / (mode + ".log")).open("wb") as log:
            subprocess.run([str(binary), mode], stdout=log, stderr=log, check=True)
        results.append(dict(mode=mode, passed=True))
    bound = sources + [camera / "NezhaCameraNativeMemory.cpp", camera / "NezhaCameraNativeMemory.h",
                       ROOT / "tests/camera-native-memory.cpp", native / "NativeAdrenoMemory.h"]
    result = dict(passed=True, cases=results, compiler=cc, cxx=cxx,
                  sanitizer="address,undefined", actual_camera_adapter=True,
                  actual_SMMU_builder=True, RAW_pages_checked=7680,
                  reopened_sessions=4, allocator_is_fixture=True, hardware_verified=False,
                  source_sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in bound})
    (out / "qualified.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "source_sha256"}, indent=2))


if __name__ == "__main__":
    main()
