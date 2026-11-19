#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production PCM queue with the reused native LRA gain policy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',required=True,type=Path);a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=False)
    D=Path(__file__).resolve().parents[1];Q=D/'package/haptics27/NezhaHapticsQueue.c'
    T=D/'package/haptics27/tests/envelope_cases.c'
    command=[os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',D/'components',
             '-I',D/'components/sm8850/kits',Q,T,'-o',a.out/'envelope']
    subprocess.run(list(map(str,command)),check=True)
    result=subprocess.run([str(a.out/'envelope')],capture_output=True,text=True,check=True)
    (a.out/'execution.log').write_text(result.stdout+result.stderr)
    report=dict(passed=True,production_queue_executed=True,all_int16_magnitudes_checked=True,
        queue_source_sha256=hashlib.sha256(Q.read_bytes()).hexdigest(),
        cases_sha256=hashlib.sha256(T.read_bytes()).hexdigest(),
        policy_source='HakiOS redwood gain_for@f128f1d8e2ef1f588002787c5efb94ec134ddeac',
        motor_limit_mv=2000,maximum_drive_ms=500,phone_operations=False,physical_strength_pending=True)
    (a.out/'qualified.json').write_text(json.dumps(report,indent=2)+'\n')
    print(result.stdout,end='')
if __name__=='__main__':main()
