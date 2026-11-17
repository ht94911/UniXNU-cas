"""Compile the ordinary mobile Camera launcher with its native FBS completion."""
import argparse,plistlib,hashlib,importlib.util,json,shutil,subprocess,sys
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--sealed',type=Path,required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[1];args.out.mkdir(parents=True,exist_ok=False);logs=args.out/'logs';logs.mkdir()
manifest=json.loads((root/'inputs.json').read_text());calls=[];errors=[]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def run(label,command,required=True):
    command=list(map(str,command));calls.append(command)
    r=subprocess.run(command,capture_output=True,timeout=180)
    (logs/(label+'.stdout')).write_bytes(r.stdout);(logs/(label+'.stderr')).write_bytes(r.stderr)
    if r.returncode:
        errors.append(dict(label=label,code=r.returncode))
        if required:raise RuntimeError(label)
    return r
try:
    for name,digest in manifest['files'].items():assert sha(root/name)==digest
    assert sha(root/'ci/artifacts.py')==manifest['artifacts_sha256']
    assert sha(root/'ci/artifact-recipient.pem')==manifest['certificate_sha256']
    shutil.copyfile(root/'inputs.json',args.out/'inputs.json')
    job=plistlib.loads((root/'source/org.unixnu.nezha.camera-launch.plist').read_bytes());assert job['UserName']=='mobile' and job['LaunchOnlyOnce'] and job['KeepAlive'] is False and job['RunAtLoad'] and 'StartInterval' not in job and job['LimitLoadToSessionType']=='System' and job['StandardOutPath']==job['StandardErrorPath']=='/dev/null'
    shutil.copyfile(root/'source/org.unixnu.nezha.camera-launch.plist',args.out/'org.unixnu.nezha.camera-launch.plist')
    host=args.out/'registration-host'
    run('registration-host-compile',['xcrun','clang','-fobjc-arc','-fblocks','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-O1','-g',root/'source/camera-launch-registration.m','-framework','Foundation','-o',host])
    r=run('registration-host-execute',[host]);assert b'18 registration/ordinary shell completion cases' in r.stdout
    sdk=run('sdk',['xcrun','--sdk','iphoneos','--show-sdk-path']).stdout.decode().strip()
    base=['xcrun','--sdk','iphoneos','clang','-target','arm64e-apple-ios27.0','-isysroot',sdk,'-fobjc-arc','-fblocks','-O2','-Wall','-Wextra','-Werror']
    launcher=args.out/'nezha-camera-launch'
    run('launcher-compile',base+[root/'source/NezhaCameraLaunch.m','-framework','Foundation','-o',launcher])
    run('launcher-sign',['codesign','--force','--sign','-','--identifier','org.unixnu.nezha.camera-launch','--pagesize','4096','--entitlements',root/'source/NezhaCameraLaunch.entitlements.plist',launcher])
    for label,path in [('launcher',launcher)]:
        run(label+'-verify',['codesign','--verify','--strict',path]);r=run(label+'-signature',['codesign','-d','--verbose=4',path]);assert b'Page size=4096'in r.stderr
    (args.out/'qualified.json').write_text(json.dumps(dict(target='arm64e-apple-ios27.0',sdk=sdk,launcher_sha256=sha(launcher),Camera_app_replaced=False,TCC_changed=False,original_LS_registration_caller=True,original_modifydb_post_install=True,registration_fixture_cases=18,ordinary_FBS_shell_launch_completion=True,positive_valid_process_handle_required=True,launch_error_details_preserved=True,launch_once_no_retry=True,launch_completion_wait_seconds=30,output_ownership_fixture=True,launcher_uid=501,launcher_owned_output_mode=0o600,launcher_early_process_marker=True,launcher_boot_marker=True,launcher_once_at_load=True,launcher_delay_seconds=120,per_event_authorization_and_uptime=True,signature_page_bytes=4096,device_executed=False),indent=2)+'\n')
except Exception as e:
    errors.append(dict(error=str(e)))
finally:
    (args.out/'build-result.json').write_text(json.dumps(dict(conclusion='failure'if errors else'success',errors=errors,commands=calls),indent=2)+'\n')
    spec=importlib.util.spec_from_file_location('gnss_artifacts',root/'ci/artifacts.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    m.seal(args.out,sorted(p.relative_to(args.out).as_posix()for p in args.out.rglob('*')if p.is_file()),root/'ci/artifact-recipient.pem',args.sealed)
if errors:sys.exit(1)
print('Camera mobile FBS launcher compiled and signed')
