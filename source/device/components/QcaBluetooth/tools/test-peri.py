#!/usr/bin/env python3
"""Exercise PERI/TME and fenced TX/fast RX using HAL-derived frames, without device I/O."""
from pathlib import Path
import hashlib
import json
import os
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "out/peri-test"
out.mkdir(parents=True, exist_ok=True)
sources = ["src/qbt_peri.c", "src/qbt_core.c", "tests/peri.c"]
inputs = sources + ["include/qbt_peri.h", "include/qbt.h", "src/qbt_internal.h", "tools/test-peri.py"]
command = [os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-I" + str(root / "include"), "-I" + str(root / "src"),
           *[str(root / source) for source in sources], "-o", str(out / "peri")]
subprocess.run(command, check=True)
result = subprocess.run([out / "peri"], text=True, capture_output=True)
(out / "output.txt").write_text(result.stdout + result.stderr)
report = dict(passed=result.returncode == 0, controller_simulated=True, hardware_verified=False,
              device_operations=False, private_firmware_used=False, command=command,
              inputs={source: hashlib.sha256((root / source).read_bytes()).hexdigest() for source in inputs},
              output=result.stdout + result.stderr)
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
if result.returncode:
    raise SystemExit((result.stdout + result.stderr)[-5000:] or str(result.returncode))
print(result.stdout, end="")
