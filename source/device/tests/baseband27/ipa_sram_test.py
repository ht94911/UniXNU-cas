#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile the actual IPA SRAM writer with the minimum host XNU declarations."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nb-ipa-sram-") as name:
    out = Path(name)
    headers = {
        "sys/param.h": "#include <stdint.h>\n#include <stddef.h>\n",
        "sys/systm.h": "#include <stdio.h>\n#include <string.h>\nvoid delay(int);\n",
        "sys/errno.h": "#include <errno.h>\n",
        "machine/machine_routines.h": (
            "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n"
            "typedef uintptr_t vm_offset_t;\ntypedef size_t vm_size_t;\n"
        ),
        "kern/clock.h": "\n",
    }
    for name, text in headers.items():
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    subprocess.run([
        os.environ.get("CC", "gcc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-Wno-format", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", *(["-no-pie"] if sys.platform != "darwin" else []),
        "-I", str(out),
        str(ROOT / "tests/baseband27/ipa_sram_test.c"), "-o", str(out / "test"),
    ], check=True)
    subprocess.run([str(out / "test")], check=True)
