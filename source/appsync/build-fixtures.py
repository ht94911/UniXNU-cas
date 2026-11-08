#!/usr/bin/env python3
"""Build three disposable arm64 IPAs for the AppSync acceptance sequence."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
out = parser.parse_args().output.resolve()
out.mkdir(parents=True, exist_ok=False)
rows = []
variants = [('signed', '1', True), ('unsigned', '1', False), ('unsigned', '2', False)]
sdk = subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
for label, version, signed in variants:
    identifier = 'org.unixnu.nezha.appsync.probe.' + label
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        app = root / 'Payload/AppSyncProbe.app'
        app.mkdir(parents=True)
        program = app / 'AppSyncProbe'
        plist = dict(CFBundleIdentifier=identifier, CFBundleExecutable=program.name,
                     CFBundleName='AppSyncProbe', CFBundleDisplayName='Probe ' + label,
                     CFBundlePackageType='APPL', CFBundleVersion=version,
                     CFBundleShortVersionString=version+'.0', MinimumOSVersion='27.0',
                     CFBundleSupportedPlatforms=['iPhoneOS'], LSRequiresIPhoneOS=True,
                     UIDeviceFamily=[1], UIApplicationSceneManifest={
                         'UIApplicationSupportsMultipleScenes':False})
        (app/'Info.plist').write_bytes(plistlib.dumps(plist))
        cmd = ['xcrun','clang','-target','arm64-apple-ios27.0','-isysroot',sdk,'-fobjc-arc',
               '-framework','UIKit','-framework','Foundation','-Wl,-no_adhoc_codesign',
               str(Path(__file__).with_name('FixtureApp.m')),'-o',str(program)]
        subprocess.run(cmd,check=True)
        if signed:
            subprocess.run(['codesign','--force','--sign','-','--pagesize','4096',
                            '--digest-algorithm','sha256',str(app)],check=True)
            subprocess.run(['codesign','--verify','--strict',str(app)],check=True)
        load = subprocess.check_output(['xcrun','otool','-l',str(program)],text=True)
        if ('LC_CODE_SIGNATURE' in load) != signed:
            raise ValueError('fixture signature state differs')
        bundle = out / f'appsync-probe-{label}-v{version}.ipa'
        with zipfile.ZipFile(bundle,'x',compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(app.rglob('*')):
                if path.is_file():
                    archive.write(path,path.relative_to(root))
        rows.append(dict(ipa=bundle.name, identifier=identifier, version=version,
                         signature='ad-hoc' if signed else 'unsigned',
                         ipa_sha256=hashlib.sha256(bundle.read_bytes()).hexdigest()))
(out/'fixtures.json').write_text(json.dumps(rows,indent=2)+'\n')
