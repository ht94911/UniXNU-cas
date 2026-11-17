#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run production-source tests after the human chooses a fixed test baseline."""
from pathlib import Path
import argparse
import subprocess
p=argparse.ArgumentParser();p.add_argument('--qrtr-source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[1];a.output.mkdir(parents=True,exist_ok=True)
binary=a.output/'ssc-sensors'
command=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(root/'include'),'-I',str(a.qrtr_source),str(root/'tests/ssc-sensors.c'),*[str(x) for x in sorted((root/'src').glob('*.c'))],str(a.qrtr_source/'QcomQrtr.c'),'-o',str(binary)]
subprocess.run(command,check=True);subprocess.run([binary],check=True)
