#!/usr/bin/env python3
"""Check the production adapter against the selected native hook and trace ABI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--native", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
args.out.mkdir(parents=True, exist_ok=False)
header = root / "package/gpu/NezhaGpuBoPhases.h"
publisher = args.native / "NativeAdrenoPublish.cpp"
source = publisher.read_text()
actual = re.search(r"struct NativeAdrenoBoTrace \{(.*?)\};", source, re.S)
adapter = re.search(r"struct NativeAdrenoBoTrace \{(.*?)\};", header.read_text(), re.S)
assert actual and adapter
compact = lambda text: re.sub(r"\s+", "", text)
assert compact(actual[1]) == compact(adapter[1]), "Selected native hook ABI differs"
phase = source[source.index("if(alloc && request.bytes>="):source.index(
    "    native_adreno_metal_diag.inflight=0;", source.index("if(alloc && request.bytes>="))]
assert "hook(&trace);" in phase and "trace.entered=began;" in phase
board = (root / "package/gpu/NezhaAdreno840.cpp").read_text()
assert board.index("if(native_gpu_trace_enabled() && !native_adreno_bo_hook)") < board.index(
    'checkpoint(14,publishMetal()')
assert "native_adreno_bo_hook=nezhaGpuBoPhases;" in board
assert "thread_tid(current_thread())" in board
binary = args.out / "gpu-bo-phases"
command = [
    os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror",
    "-I" + str(args.native), str(root / "tests/gpu-bo-phases.cpp"),
    "-o", str(binary),
]
build = subprocess.run(command, text=True, capture_output=True)
(args.out / "build.log").write_text(build.stdout + build.stderr)
assert build.returncode == 0, build.stderr
test = subprocess.run([str(binary)], text=True, capture_output=True)
(args.out / "execution.log").write_text(test.stdout + test.stderr)
assert test.returncode == 0, test.stderr
paths = [
    header, publisher, args.native / "NativeGpuTrace.h",
    args.native / "NativeGpuTraceStore.h",
    root / "package/gpu/NezhaAdreno840.cpp",
    root / "tests/gpu-bo-phases.cpp", Path(__file__),
]
report = dict(
    passed=True, actual_native_phase_layout_matched=True,
    existing_trace_store_executed=True, ASan_UBSan=True,
    source_files={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
    compile_command=command, execution_output=test.stdout,
    native_compilation=False, final_hook_store_verified=False,
    hardware_phase_capture=False, phone_operations=False,
)
(args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(test.stdout, end="")
