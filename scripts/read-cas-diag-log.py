#!/usr/bin/env python3
"""Decode a saved cas RAM diagnostic slot. No device or storage-write operations.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import argparse
import json
from cas_diag_log import CAPACITY,decode

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path")
    ap.add_argument("--build",default="cas-diag-007")
    ap.add_argument("--current-boot",type=lambda value:int(value,0))
    args=ap.parse_args()
    try:
        with open(args.path,"rb") as stream:raw=stream.read(CAPACITY+1)
        report=decode(raw,args.build,args.current_boot)
        print(json.dumps(report,indent=2))
        return 0 if report["result"]=="valid" else 1
    except (OSError,ValueError) as error:
        ap.exit(2,f"diagnostic record refused: {error}\n")
if __name__=="__main__":raise SystemExit(main())
