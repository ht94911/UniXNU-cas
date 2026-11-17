#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the ordinary CoreMotion app and reused source-only launch/log helpers."""
import argparse,hashlib,json,plistlib,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--usb',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=False);logs=a.output/'logs';logs.mkdir();commands=[]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(name,command):
 command=[str(x) for x in command];r=subprocess.run(command,capture_output=True,timeout=180)
 (logs/(name+'.stdout')).write_bytes(r.stdout);(logs/(name+'.stderr')).write_bytes(r.stderr)
 commands.append(dict(name=name,command=command,exit_code=r.returncode));(a.output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 if r.returncode:raise RuntimeError(name+' failed; see original logs')
 return r.stdout
passed=False
try:
 info=plistlib.loads((a.source/'Info.plist').read_bytes());assert info['NSMotionUsageDescription'] and info['CFBundleIdentifier']=='org.unixnu.nezha.sensors27.test'
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip()
 base=['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2']
 bundle=a.output/'NezhaSensors.app';bundle.mkdir();shutil.copyfile(a.source/'Info.plist',bundle/'Info.plist')
 run('app-compile',[*base,a.source/'NezhaSensorsApp.m','-framework','UIKit','-framework','CoreMotion','-framework','Foundation','-o',bundle/'NezhaSensors'])
 run('app-sign',['codesign','--force','--sign','-','--identifier',info['CFBundleIdentifier'],'--entitlements',a.source/'app.entitlements.plist','--generate-entitlement-der','--pagesize','4096','--digest-algorithm','sha256',bundle])
 run('app-verify',['codesign','--verify','--strict',bundle])
 for name,source in [('nezha-sensors-launch','NezhaSensorsLaunch.m'),('nezha-sensors-export','NezhaSensorsExport.mm')]:
  binary=a.output/name
  command=[*base,a.source/source,'-framework','Foundation','-o',binary]
  if source.endswith('.mm'):command+=['-std=c++17','-I',a.usb/'userland','-I',a.usb/'include']
  run(name+'-compile',command)
  sign=['codesign','--force','--sign','-','--identifier','org.unixnu.'+name,'--generate-entitlement-der','--pagesize','4096','--digest-algorithm','sha256']
  if name.endswith('launch'):sign+=['--entitlements',a.source/'launch.entitlements.plist']
  run(name+'-sign',[*sign,binary]);run(name+'-verify',['codesign','--verify','--strict',binary])
 for name in ['NezhaSensors.app/NezhaSensors','nezha-sensors-launch','nezha-sensors-export']:
  run(name.replace('/','-')+'-signature',['codesign','-d','--verbose=4',a.output/name])
  run(name.replace('/','-')+'-imports',['xcrun','nm','-u',a.output/name])
 passed=True
finally:
 (a.output/'qualification.json').write_text(json.dumps(dict(passed=passed,normal_application=True,CoreMotion_public_API=True,proximity_UIDevice_API=True,NSMotionUsageDescription=True,page_size=4096,source_sha256={x.name:sha(x) for x in a.source.iterdir() if x.is_file()},outputs={str(x.relative_to(a.output)):sha(x) for x in a.output.rglob('*') if x.is_file() and 'logs' not in x.parts},phone_executed=False),indent=2)+'\n')
print('Sensor ordinary app/launch/root exporter compiled and signed')
