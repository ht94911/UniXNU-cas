#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the selected production integration with the existing ALS model."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
D=Path(__file__).resolve().parents[1];C=D/'components/QcomSscSensors';Q=D/'package/als-owner/upstream'
a.output.mkdir(parents=True,exist_ok=True)
flags=['-O1','-g','-fno-pie','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(C/'include'),'-I'+str(Q),'-I'+str(D/'tests'),'-I'+str(D/'package/sensors')]
sources=[D/'package/sensors/NezhaSensors.c',*sorted((C/'src').glob('*.c')),Q/'QcomQrtr.c']
commands=[];objects=[]
for s in sources:
 o=a.output/(s.stem+'.o');command=['cc','-std=c11',*flags,'-c',str(s),'-o',str(o)]
 subprocess.run(command,check=True);commands.append(command);objects.append(o)
command=['c++','-std=c++17',*flags,'-no-pie','-pthread',str(D/'tests/sensors-integration-host.cpp'),*[str(o) for o in objects],'-o',str(a.output/'integration')]
subprocess.run(command,check=True);commands.append(command)
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
r=subprocess.run([str(a.output/'integration')],check=True,env=env,capture_output=True,text=True)
late=subprocess.run([str(a.output/'integration'),'stop-before-install'],check=True,env=env,capture_output=True,text=True)
(a.output/'run.log').write_text(r.stdout+r.stderr+late.stdout+late.stderr);print(r.stdout+late.stdout,end='')
sources+=list((D/'package/sensors').glob('*.cpp'))+list((D/'package/sensors').glob('*.h'))+list((D/'tests').glob('sensors*'))
(a.output/'qualified.json').write_text(json.dumps(dict(passed=True,ASan_UBSan=True,boot_lifetime_retained=True,native_and_hardware_pending=True,commands=commands,source_sha256={str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}),indent=2)+'\n')
