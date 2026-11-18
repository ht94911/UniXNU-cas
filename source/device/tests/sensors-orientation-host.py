#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the production orientation classifier and HID report producer."""
import argparse,json,os,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
D=Path(__file__).resolve().parents[1];a.output.mkdir(parents=True,exist_ok=True)
flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
       '-I'+str(D/'tests'),'-I'+str(D/'package/sensors'),'-I'+str(D/'components/QcomSscSensors/include'),
       '-I'+str(D/'package/als-owner/upstream')]
obj=a.output/'orientation.o';binary=a.output/'orientation'
link=[]
if sys.platform!='darwin':flags+=['-fno-pie'];link=['-no-pie']
subprocess.run(['cc','-std=c11',*flags,'-c',str(D/'package/sensors/NezhaSensorOrientation.c'),'-o',str(obj)],check=True)
subprocess.run(['c++','-std=c++17',*flags,*link,str(D/'tests/sensors-orientation-host.cpp'),str(obj),'-o',str(binary)],check=True)
result=subprocess.run([str(binary)],check=True,text=True,capture_output=True,
                     env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
log=result.stdout+result.stderr
for case in ['missing-interface','duplicate-interface','missing-parser','parser-start-failed',
             'parser-without-orientation','parser-without-service-identity']:
    result=subprocess.run([str(binary),case],check=True,text=True,capture_output=True,
                         env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    log+=result.stdout+result.stderr
(a.output/'run.log').write_text(log);print(log,end='')
(a.output/'qualified.json').write_text(json.dumps(dict(passed=True,ASan_UBSan=True,production_source=True,
                                                     real_IOHID_parser_pending=True,target_and_hardware_pending=True),indent=2)+'\n')
