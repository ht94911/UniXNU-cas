from pathlib import Path
import argparse,hashlib,json,subprocess,plistlib,shutil
from artifacts import seal
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--sealed',type=Path,required=True);a=p.parse_args();R=Path(__file__).resolve().parents[1];a.out.mkdir(parents=True,exist_ok=False);L=a.out/'logs';L.mkdir();m=json.loads((R/'inputs.json').read_text());commands=[];passed=False
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def run(n,c):
 c=[str(x) for x in c];r=subprocess.run(c,capture_output=True,timeout=180);(L/(n+'.stdout')).write_bytes(r.stdout);(L/(n+'.stderr')).write_bytes(r.stderr);commands.append(dict(name=n,command=c,exit_code=r.returncode));assert r.returncode==0,n;return r.stdout
app=a.out/'NezhaSensors.app';app.mkdir();binary=app/'NezhaSensors'
try:
 assert all(sha(R/n)==h for n,h in m['files'].items())
 sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip()
 run('app-compile',['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-O2',R/'source/NezhaSensorsApp.m','-framework','UIKit','-framework','CoreMotion','-framework','Foundation','-o',binary])
 shutil.copyfile(R/'source/Info.plist',app/'Info.plist')
 run('app-sign',['codesign','--force','--sign','-','--identifier','org.unixnu.nezha.sensors27.test','--entitlements',R/'source/app.entitlements.plist','--generate-entitlement-der','--pagesize','4096','--digest-algorithm','sha256',app])
 run('app-verify',['codesign','--verify','--strict',app]);run('signature',['codesign','-d','--verbose=4',binary]);run('imports',['xcrun','nm','-u',binary]);passed=True
finally:
 (a.out/'inputs.json').write_bytes((R/'inputs.json').read_bytes())
 (a.out/'qualification.json').write_text(json.dumps(dict(passed=passed,unit='app',built_units=['NezhaSensors.app/NezhaSensors'] if passed else [],source_revision=m['device_revision'],source_sha256={n:sha(R/'source'/n) for n in ['NezhaSensorsApp.m','Info.plist','app.entitlements.plist']},outputs={'NezhaSensors.app/NezhaSensors':sha(binary)} if passed else {},commands=commands,phone_operations=False),indent=2)+'\n')
 seal(a.out,sorted(x.relative_to(a.out).as_posix() for x in a.out.rglob('*') if x.is_file()),R/'ci/artifact-recipient.pem',a.sealed)
if not passed:raise SystemExit(1)
print('Sensor008b ordinary app-only strict build passed')
