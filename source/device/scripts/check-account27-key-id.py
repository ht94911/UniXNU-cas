#!/usr/bin/env python3
"""Test the real ID1 publication/getter with a narrow registry double."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", required=True, type=Path)
args = parser.parse_args()
device = Path(__file__).resolve().parents[1]
source = (device / "package/KeyStoreProbe.cpp").read_text()
start = source.index("size_t native_aks_device_id(")
end = source.index("bool native_aks_state_enabled(", start)
args.out.mkdir(parents=True, exist_ok=False)
(args.out / "actual-id.inc").write_text(source[start:end])
subprocess.run([
    "c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
    "-I", str(args.out), str(device / "tests/account27-key-id.cpp"),
    "-o", str(args.out / "test"),
], check=True)
subprocess.run([str(args.out / "test")], check=True)
