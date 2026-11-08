#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the actual production firmware format validator; optional private inputs stay local."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('files',type=Path,nargs='*');args=p.parse_args()
out=root/'out/firmware-test';out.mkdir(parents=True,exist_ok=True)
command=[os.environ.get('CC','clang'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-I'+str(root/'include'),str(root/'src/qbt_firmware.c'),str(root/'tests/firmware.c'),'-o',str(out/'firmware')]
subprocess.run(command,check=True)
before={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in args.files}
r=subprocess.run([str(out/'firmware'),*map(str,args.files)],capture_output=True,text=True)
(out/'output.txt').write_text(r.stdout+r.stderr)
if r.returncode:raise SystemExit(r.stdout+r.stderr)
assert before=={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in args.files}
(out/'report.json').write_text(json.dumps({'passed':True,'hardware_verified':False,'command':command,'inputs':before,'input_files_unchanged':True,'output':r.stdout},indent=2)+'\n')
print(r.stdout,end='')
