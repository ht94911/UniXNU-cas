#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build/sign the isolated Actuator bundle and explicit bounded test program."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

def run(args):return subprocess.check_output(list(map(str,args)),text=True).strip()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();source=args.source.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    ios=run(['xcrun','--sdk','iphoneos','--show-sdk-path']);mac=Path(run(['xcrun','--sdk','macosx','--show-sdk-path']))
    bundle=out/'NezhaHaptics27.driver';bundle.mkdir(exist_ok=True);binary=bundle/'NezhaHaptics'
    with tempfile.TemporaryDirectory() as tmp:
        headers=Path(tmp)/'CoreAudio';headers.mkdir()
        for name in ('AudioServerPlugIn.h','AudioHardware.h','AudioHardwareBase.h','AudioHardwareDeprecated.h'):
            (headers/name).symlink_to(mac/'System/Library/Frameworks/CoreAudio.framework/Headers'/name)
        command=['xcrun','clang','-target','arm64e-apple-ios27.0','-isysroot',ios,'-I',tmp,
                 '-O2','-g0','-Wall','-Wextra','-Werror','-bundle','-fvisibility=hidden','-framework','CoreFoundation',
                 str(source/'hal/NezhaHapticsPlugIn.c'),'-o',str(binary)]
        cp=subprocess.run(command,capture_output=True,text=True)
        (out/'HAL-command.json').write_text(json.dumps(command,indent=2)+'\n');(out/'HAL-build.log').write_text(cp.stdout+cp.stderr)
        if cp.returncode:raise SystemExit('HAL target compile failed')
        command=['xcrun','clang','-target','arm64e-apple-ios27.0','-isysroot',ios,'-O2','-g0','-Wall','-Wextra','-Werror',
                 str(source/'hal/NezhaHapticsProbe.c'),'-o',str(out/'NezhaHapticTest')]
        cp=subprocess.run(command,capture_output=True,text=True);(out/'probe-build.log').write_text(cp.stdout+cp.stderr)
        if cp.returncode:raise SystemExit('bounded hardware probe compile failed')
    shutil.copy2(source/'hal/Info.plist',bundle/'Info.plist')
    outputs=[]
    for path,identifier in [(bundle,'org.unixnu.nezha.haptics27'),(out/'NezhaHapticTest','org.unixnu.nezha.haptic-test')]:
        run(['codesign','--force','--sign','-','--pagesize','4096','--identifier',identifier,path]);run(['codesign','--verify','--strict',path])
        cp=subprocess.run(['codesign','--display','--verbose=4',path],capture_output=True,text=True,check=True)
        file=binary if path==bundle else path
        match=re.search(r'^CDHash=([0-9a-f]{40})$',cp.stderr,re.M);assert match
        outputs.append({'file':str(file),'sha256':hashlib.sha256(file.read_bytes()).hexdigest(),'cdhash':match[1]})
        (out/(file.name+'-signature.txt')).write_text(cp.stderr)
    (out/'HAL-report.json').write_text(json.dumps({'target':'arm64e-apple-ios27.0','sdk':ios,'header_sdk':str(mac),
        'files':outputs,'private_inputs':False,'hardware_verified':False},indent=2)+'\n')
    print('Haptics27 Actuator and bounded probe target-built and signed')

if __name__=='__main__':main()
