#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib

ROOT=Path(__file__).resolve().parents[2]
SOC=Path(os.environ.get('BB_SOC_SOURCE',str(ROOT.parent/'soc-shared')))
UFS=Path(os.environ.get('BB_UFS_SOURCE',str(ROOT.parent.parent/'gpu27-astra/ufs-persist')))
FRAMEWORK=Path(os.environ.get('BB_FRAMEWORK_SOURCE',str(ROOT.parent.parent/'gpu27-astra/framework')))
SMMU=Path(os.environ.get('BB_SMMU_SOURCE',str(FRAMEWORK/'kits/ArmSmmu4K')))
def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module);return module
stage=load(ROOT/'scripts/prepare-nezha27-baseband.py','bb_stage_test')
builder=load(ROOT/'scripts/nezha_baseband_build.py','bb_build_test')
old=tomllib.loads(subprocess.check_output(['git','show','dee8208:board.toml'],cwd=ROOT,text=True))
board=tomllib.loads((ROOT/'board.toml').read_text())
assert old['payload']['kernel_args']==board['payload']['kernel_args']
assert old['kernel']['prelink']==board['kernel']['prelink']
assert board['adt']['chosen']['u32']['nezha-baseband']==1
archives=board['kernel']['archives']
assert archives.index('prepare-nezha27-baseband.py --tree .')<archives.index('component:ShengUfs')
# build-adreno840.py requires all other final-link archives before it starts.
assert archives.index('build-nezha27-input.py')<archives.index('build-nezha27-shared.py --tree .')<archives.index('build-nezha27-baseband.py --tree .')<archives.index('component:Adreno840')
assert len(archives)==len(set(archives))
protected=[ROOT/'package/gpu/NezhaScm.c',ROOT/'package/SourceStorage.cpp',UFS/'NativeUfs.cpp',SOC/'src/Sm8850Resources.c',FRAMEWORK/'scripts/unixnu/devicetree.py']
before={p:hashlib.sha256(p.read_bytes()).digest() for p in protected}
with tempfile.TemporaryDirectory(prefix='nb-integration-') as name:
    tree=Path(name)
    for folder in ['kits','drivers','scripts']:(tree/folder).mkdir()
    for path,target in [(tree/'kits/nezha',ROOT/'package'),(tree/'kits/ArmSmmu4K',SMMU),(tree/'drivers/ShengUfs',UFS),(tree/'scripts/unixnu',FRAMEWORK/'scripts/unixnu'),(tree/'drivers/Adreno840',ROOT/'package/gpu')]:path.symlink_to(target,target_is_directory=True)
    assert stage.stage(tree,SOC,True)['checked']
    report=stage.stage(tree,SOC);assert stage.stage(tree,SOC)==report
    pkg=tree/'kits/nezha'
    assert (pkg/'SourceStorage.cpp').read_text().count('    nezha_baseband_late_init();')==1
    assert 'return nezha_baseband_enabled();' in (pkg/'baseband27/bridge/coachz_baseband.cpp').read_text()
    assert '"coachz-baseband"' not in (pkg/'baseband27/bridge/coachz_baseband.cpp').read_text()
    assert 'baseband_on=nezha_baseband_enabled()==1;' in (pkg/'BootWatchdogPolicy.c').read_text()
    assert (tree/'drivers/ShengUfs/NativeUfs.cpp').read_text().count('#include "NbUfsReadOnly.inc"')==1
    # The GPU recipe compiles NezhaScm.c through drivers/Adreno840; it must be the staged unit.
    assert (tree/'drivers/Adreno840').resolve()==(pkg/'gpu').resolve()
    assert (tree/'drivers/Adreno840/NezhaScm.c').read_text().count('#include "../baseband27/integration/NbScmOwner.inc"')==1
    assert (tree/'drivers/Adreno840/../baseband27/integration/NbScmOwner.inc').is_file()
    assert 'bb_chosen' not in (pkg/'baseband27/bridge/coachz_baseband.cpp').read_text()
    selected=builder.sources(tree,'shared')+builder.sources(tree,'baseband')
    assert len(selected)==len(set(selected)) and all(p.is_file() for p in selected)
    for stem in ['QcomGlink.c','QcomSmem.c','QcomSmp2p.c','Sm8850Resources.c','Sm8850ResourcesNative.cpp']:
        assert sum(p.name==stem for p in selected)==1
    assert not any(p.name.startswith('ArmSmmu4K') for p in selected)
    stamp=tree/'build/nezha27-baseband-stage/stage.json'
    changed=copy.deepcopy(report);changed['package_inputs']['SourceStorage.cpp']='0'*64
    stamp.write_text(json.dumps(changed))
    try:
        stage.stage(tree,SOC)
    except ValueError as error:
        assert 'package source changed' in str(error)
    else:
        raise AssertionError('stale source identity accepted')
    stamp.write_text(json.dumps(report))
    # Exercise the existing Format 2 converter with the added typed root node.
    sys.path.insert(0,str(tree/'scripts'))
    from unixnu.devicetree import generate
    dtb=tree/'hardware.dtb'
    subprocess.run(['dtc','-I','dts','-O','dtb','-i',str(SOC/'dts'),'-o',str(dtb),str(ROOT/'dts/nezha.dts')],check=True,capture_output=True)
    encoded=generate(dtb,board,unique_chip_id='1')
    def decode(at):
        props,kids=struct.unpack_from('<II',encoded,at);at+=8;values={};children={}
        for _ in range(props):
            key=encoded[at:at+32].split(b'\0',1)[0].decode();size=struct.unpack_from('<I',encoded,at+32)[0];at+=36
            values[key]=encoded[at:at+size];at+=(size+3)&~3
        for _ in range(kids):
            child,at=decode(at);children[child[0]['name'].rstrip(b'\0').decode()]=child
        return (values,children),at
    decoded,end=decode(0);assert end==len(encoded)
    children=decoded[1];assert children['baseband'][0]['compatible']==b'baseband,n41\0'
    assert children['chosen'][0]['nezha-baseband']==struct.pack('<I',1)
    assert children['chosen'][0]['nezha-baseband-low-memory']==struct.pack('<QQ',0,5*1024*1024)
    assert children['product'][0]['baseband-chipset']==b'mav21\0'
assert all(hashlib.sha256(p.read_bytes()).digest()==digest for p,digest in before.items())
print('PASS package integration: staged patches, one transport copy, unchanged arguments/prelink, real Format 2 ADT')
