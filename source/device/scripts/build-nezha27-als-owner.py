#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile the selected fresh ADSP/IPCRTR ALS owner with native XNU records.

Install beside kernel_target.py and build-sheng-source-drivers.py in a new
fixed source snapshot. The ALS frontend/backend archive uses its own recipe;
Both recipes resolve this same board kit's als-owner/upstream directory.
"""
from pathlib import Path
import hashlib, importlib.util, json, os, shlex, subprocess, tomllib
from kernel_target import ARCH, IOS_VERSION, check_object

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT/'kits/nezha/als-owner'
OUT = ROOT/'build/nezha27-als-owner'
OUT.mkdir(parents=True, exist_ok=True)
LOCK = json.loads(Path(__file__).with_name('nezha27-als-owner-inputs.json').read_text())

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def require(ok, why):
    if not ok:
        raise SystemExit(why)

def load(path):
    spec = importlib.util.spec_from_file_location('audio_compile_helper', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def run(command, **kwargs):
    return subprocess.check_output([str(x) for x in command], timeout=300, **kwargs)

require(ARCH == 'arm64e' and IOS_VERSION.split('.')[0] == '27',
        'ALS owner requires the selected arm64e/iOS27 native build')
helper = load(ROOT/'scripts/build-sheng-source-drivers.py')
device = Path(os.environ['UNIXNU_DEVICE']).resolve()
qrtr = (SRC/'upstream').resolve()
require(qrtr == (SRC/'upstream').resolve(), 'ALS archive must use this sole owner QRTR header')
als = device/'package/als'
smmu = ROOT/'kits/ArmSmmu4K'
for name, digest in LOCK['ArmSmmu4K']['files'].items():
    require(sha(smmu/name) == digest, 'selected shared ArmSmmu4K identity differs: '+name)
local_sources = tomllib.loads((device/'local.toml').read_text())['source']
soc_source = next(x for x in local_sources if x['name'] == 'sm8850' and x['kind'] == 'soc')
soc = (device/soc_source['local']).resolve()
for name, digest in LOCK['soc_resource_owner']['files'].items():
    require(sha(soc/name) == digest, 'selected SM8850 common owner differs: '+name)
for name, digest in LOCK['sources'].items():
    require(sha(SRC/name) == digest, 'fixed owner source differs: '+name)
for name in ['upstream/QcomQrtr.h', 'platform/AudioPdMapper.h', 'NezhaAlsNativeCaller.h']:
    require(sha(SRC/name) == LOCK['headers'][name], 'fixed owner header differs: '+name)

base = ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
record_paths = {False: base/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json',
                True: base/'bsd/DEVELOPMENT/bsd_init.o.json'}
records = {c: json.loads(path.read_text().strip().rstrip(',')) for c, path in record_paths.items()}
sources = [SRC/name for name in LOCK['sources']]
# These two objects are the final image's common-resource implementation.
# Existing USB/GPU consumers resolve this copy; no other archive may define it.
sources += [soc/'src/Sm8850Resources.c', soc/'src/Sm8850ResourcesNative.cpp']
objects, rows, errors = [], [], []
for source in sources:
    relative = str(source.relative_to(SRC)) if source.is_relative_to(SRC) else 'soc/'+str(source.relative_to(soc))
    name = relative.replace('/', '_')
    obj = OUT/(name+'.o')
    record = records[source.suffix == '.c']
    extra = ['-I', str(ROOT/'.cache/xnu-work/osfmk'), '-I', str(SRC),
             '-I', str(als), '-I', str(qrtr), '-I', str(smmu), '-I', str(soc/'include'),
             '-I', str(ROOT/'drivers/QcomXnuUsbDebug/include'),
             '-DNEZHA_ALS_NATIVE_OWNER=1', '-DNEZHA_ALS_CONTROL_ONLY=1',
             '-Wno-unknown-warning-option']
    command = helper.compile_command(record, source, obj, [], extra)
    (OUT/(name+'-command.json')).write_text(json.dumps(command, indent=2)+'\n')
    cp = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    (OUT/(name+'.log')).write_text(cp.stdout+cp.stderr)
    row = {'source': relative, 'source_sha256': sha(source), 'returncode': cp.returncode,
           'kernel_recipe_source': record['file'],
           'kernel_record_sha256': sha(record_paths[source.suffix == '.c'])}
    if cp.returncode:
        errors.append(name)
    else:
        check_object(obj.read_bytes(), name)
        row['object_sha256'] = sha(obj)
        deps = {}
        for item in shlex.split(obj.with_suffix('.d').read_text().replace(chr(92)+chr(10), '').split(':', 1)[1]):
            dep = Path(item)
            dep = dep if dep.is_absolute() else Path(record['directory'])/dep
            deps[str(dep.resolve())] = sha(dep)
        used_qrtr = [path for path in deps if Path(path).name == 'QcomQrtr.h']
        if used_qrtr:
            require(used_qrtr == [str((qrtr/'QcomQrtr.h').resolve())],
                    'owner consumers must use exactly one matching QRTR header')
            row['QRTR_header_sha256'] = deps[used_qrtr[0]]
        (OUT/(name+'-headers.json')).write_text(json.dumps(deps, indent=2)+'\n')
        objects.append(obj)
    rows.append(row)

report = {'objects': rows, 'errors': errors, 'native_sources': len(sources),
          'native_framework_abi_only': True, 'control_only': True, 'entry_boot_arg': 'na=2',
          'QRTR_header_sha256': sha(qrtr/'QcomQrtr.h'), 'GLINK_receive_bytes': 1056,
          'GLINK_channels': ['IPCRTR','fastrpcglink-apps-dsp'], 'HAL': 'excluded', 'PCM': 'excluded',
          'microphone': 'excluded', 'TAS_execution': 'excluded',
          'shared_resources': LOCK['soc_resource_owner'], 'shared_component': LOCK['ArmSmmu4K'],
          'duplicate_ArmSmmu_objects': False, 'hardware_started': False,
          'private_firmware_uploaded': False, 'input_lock_sha256': sha(Path(__file__).with_name('nezha27-als-owner-inputs.json'))}
(OUT/'build-report.json').write_text(json.dumps(report, indent=2)+'\n')
if errors:
    raise SystemExit('ALS control owner native source/API qualification failed: '+','.join(errors))
archive = OUT/'libNezha27AlsOwner.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, *objects], env=dict(os.environ, ZERO_AR_DATE='1'))
report.update(archive_sha256=sha(archive), objects_list=[p.name for p in objects])
(OUT/'defined-symbols.txt').write_bytes(run(['xcrun', 'nm', '-gU', archive]))
(OUT/'undefined-symbols.txt').write_bytes(run(['xcrun', 'nm', '-u', archive]))
(OUT/'build-report.json').write_text(json.dumps(report, indent=2)+'\n')
print('ALS fresh ADSP/IPCRTR owner archive compiled; strict image/package closure remains separate')
