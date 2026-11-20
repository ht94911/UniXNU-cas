"""Build only the changed HAL with the existing SDK command and CMS envelope."""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, tempfile
import artifacts

ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'source/device/package/audio27'
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,value): path.write_text(json.dumps(value,indent=2)+'\n')
def build(out):
    out.mkdir(parents=True,exist_ok=False)
    manifest=json.loads((ROOT/'inputs.json').read_text())
    for name,digest in manifest['files'].items(): assert sha(ROOT/name)==digest,name
    report={'source_revision':manifest['device_revision'],'HAL_only':True,
            'phone_operations':False,'hardware_started':False,'errors':[]}
    def run(command): return subprocess.check_output(command,text=True).strip()
    ios=run(['xcrun','--sdk','iphoneos','--show-sdk-path'])
    mac=run(['xcrun','--sdk','macosx','--show-sdk-path'])
    bundle=out/'NezhaAudio27.driver';bundle.mkdir();exe=bundle/'NezhaAudio27'
    with tempfile.TemporaryDirectory() as tmp:
        headers=Path(tmp)/'CoreAudio';headers.mkdir()
        for name in ['AudioServerPlugIn.h','AudioHardwareBase.h','AudioHardware.h','AudioHardwareDeprecated.h']:
            (headers/name).symlink_to(Path(mac)/'System/Library/Frameworks/CoreAudio.framework/Headers'/name)
        common=['-O2','-g0','-Wall','-Wextra','-Werror','-fvisibility=hidden',
                '-framework','CoreFoundation','-framework','IOKit','-framework','CoreAudio']
        command=['xcrun','clang','-target','arm64e-apple-ios27.0','-isysroot',ios,'-I',tmp,
                 '-bundle',*common,str(SRC/'hal/NezhaAudioPlugIn.c'),str(SRC/'hal/AudioClock.c'),
                 str(SRC/'core/AudioRing.c'),'-o',str(exe)]
        save(out/'HAL-command.json',command)
        result=subprocess.run(command,capture_output=True,text=True)
        (out/'HAL-build.log').write_text(result.stdout+result.stderr)
        report['HAL']={'returncode':result.returncode,'sdk':ios,'header_sdk':mac}
        if result.returncode: report['errors'].append('HAL compile')
        else:
            shutil.copyfile(SRC/'hal/Info.plist',bundle/'Info.plist')
            run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.unixnu.nezha.audio27',str(bundle)])
            run(['codesign','--verify','--strict',str(bundle)])
            result=subprocess.run(['codesign','--display','--verbose=4',str(bundle)],capture_output=True,text=True,check=True)
            (out/'HAL-signature.txt').write_text(result.stderr)
            report['HAL'].update(status='compiled-signed',sha256=sha(exe))
        # Include the production HAL itself, with no Initialize/device call.
        test=out/'speaker-properties-test'
        command=['xcrun','clang','-isysroot',mac,'-I',tmp,*common,
                 '-fsanitize=address,undefined',str(SRC/'tests/speaker_properties_test.c'),
                 str(SRC/'hal/AudioClock.c'),str(SRC/'core/AudioRing.c'),'-o',str(test)]
        save(out/'host-test-command.json',command)
        result=subprocess.run(command,capture_output=True,text=True)
        (out/'host-test-build.log').write_text(result.stdout+result.stderr)
        report['host_test']={'compile_returncode':result.returncode,'phone_operations':False}
        if result.returncode: report['errors'].append('host SDK test compile')
        else:
            result=subprocess.run([str(test)],capture_output=True,text=True)
            (out/'host-test.log').write_text(result.stdout+result.stderr)
            report['host_test']['returncode']=result.returncode
            if result.returncode: report['errors'].append('host SDK test')
    shutil.copyfile(ROOT/'inputs.json',out/'inputs.json')
    save(out/'build-report.json',report)
    if report['errors']: raise SystemExit('; '.join(report['errors']))
    print('HAL native build/signature and production SDK host test passed; no kernel build or hardware start.')

def seal(root,out):
    names=[str(p.relative_to(root)) for p in sorted(root.rglob('*')) if p.is_file() and not p.is_symlink()]
    artifacts.seal(root,names,ROOT/'ci/artifact-recipient.pem',out)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['build','seal'])
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    if args.action=='build': build(args.root)
    else:
        assert args.out is not None
        seal(args.root,args.out)
