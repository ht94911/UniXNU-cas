#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Stage B package changes in the generated tree; original component checkouts stay intact."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tomllib

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def package_hashes(root):
    return {str(p.relative_to(root)):sha(p) for p in sorted(root.rglob('*'))
            if p.is_file() and '.git' not in p.parts and '__pycache__' not in p.parts}

def verify(root, records):
    for name, digest in records.items():
        if sha(root/name) != digest:
            raise ValueError('owner source differs: '+name)

def soc_source(explicit):
    if explicit:
        return Path(explicit).resolve()
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    entries = tomllib.loads((device/'local.toml').read_text()).get('source', [])
    for entry in entries:
        if entry.get('name') == 'sm8850' and entry.get('kind') == 'soc' and entry.get('local'):
            return (device/entry['local']).resolve()
    raise ValueError('select the fixed sm8850 local source')

def stage(tree, soc=None, check=False):
    tree=Path(tree).resolve();output=tree/'build/nezha27-baseband-stage'
    kit=tree/'kits/nezha';ufs=tree/'drivers/ShengUfs';smmu=tree/'kits/ArmSmmu4K'
    # The GPU archive compiles NezhaScm.c through its own component link, not through kits/nezha.
    gpu=tree/'drivers/Adreno840'
    if (output/'stage.json').exists():
        report=json.loads((output/'stage.json').read_text())
        if kit.resolve()!=output/'device/package' or ufs.resolve()!=output/'ShengUfs' or (tree/'scripts/unixnu').resolve()!=output/'framework/scripts/unixnu' or gpu.resolve()!=output/'device/package/gpu':
            raise ValueError('staged links differ')
        if package_hashes(kit.with_name('nezha.before-baseband')) != report['package_inputs']:
            raise ValueError('package source changed; stage a fresh generated tree')
        for name,digest in report['outputs'].items():
            if sha(output/name)!=digest:raise ValueError('staged source changed: '+name)
        return report
    package=kit.resolve();storage=ufs.resolve();common=soc_source(soc)
    lock=json.loads((package/'baseband27/integration/owners.json').read_text())
    verify(storage,lock['ufs']);verify(common,lock['soc']);verify(smmu,lock['smmu']);verify(tree,lock['framework'])
    verify(package,{name.removeprefix('package/'):digest for name,digest in lock['board'].items()})
    if not gpu.is_symlink() or gpu.resolve()!=package/'gpu':raise ValueError('GPU component is not the selected package source')
    if check:return {'checked':True,'soc':str(common),'ufs':str(storage)}
    if output.exists():raise ValueError('incomplete stage exists; inspect it before retrying')
    output.mkdir(parents=True)
    shutil.copytree(package,output/'device/package',ignore=shutil.ignore_patterns('.git','__pycache__'))
    shutil.copytree(storage,output/'ShengUfs',ignore=shutil.ignore_patterns('.git','__pycache__'))
    for directory in ['include','src']:shutil.copytree(common/directory,output/'soc'/directory)
    shutil.copytree(tree/'scripts/unixnu',output/'framework/scripts/unixnu',ignore=shutil.ignore_patterns('__pycache__'))
    integration=output/'device/package/baseband27/integration'
    for line in (integration/'series').read_text().splitlines():
        name=line.split('#',1)[0].strip()
        if name:
            subprocess.run(['git','apply','--check',str(integration/name)],cwd=output/'device',check=True)
            subprocess.run(['git','apply',str(integration/name)],cwd=output/'device',check=True)
    subprocess.run(['git','apply',str(integration/'soc-stream-count.patch')],cwd=output/'soc',check=True)
    subprocess.run(['git','apply',str(integration/'format2-baseband-node.patch')],cwd=output/'framework',check=True)
    source=output/'ShengUfs/NativeUfs.cpp';body=source.read_text();marker='static int loadRootExtents(const char **why) {'
    if body.count(marker)!=1:raise ValueError('ShengUfs insertion point differs')
    source.write_text(body.replace(marker,'#include "NbUfsReadOnly.inc"\n\n'+marker))
    shutil.copyfile(integration/'NbUfsReadOnly.inc',output/'ShengUfs/NbUfsReadOnly.inc')
    # Format 2 builds read these generated-tree links. Keep the old link or
    # directory beside each one, so this staging step is locally reversible.
    for link,target in [(kit,output/'device/package'),(ufs,output/'ShengUfs'),(tree/'scripts/unixnu',output/'framework/scripts/unixnu'),(gpu,output/'device/package/gpu')]:
        backup=link.with_name(link.name+'.before-baseband')
        if backup.exists() or backup.is_symlink():raise ValueError('stage backup exists: '+str(backup))
        link.rename(backup);link.symlink_to(target,target_is_directory=True)
    report={'package_inputs':package_hashes(kit.with_name('nezha.before-baseband')),'soc':str(output/'soc'),'source_soc':str(common),'source_ufs':str(storage),'phone_operations':False,
            'outputs':{str(p.relative_to(output)):sha(p) for p in sorted(output.rglob('*')) if p.is_file()}}
    (output/'stage.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--tree',type=Path,default=Path(__file__).resolve().parents[1]);parser.add_argument('--soc',type=Path);parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    try:
        report=stage(args.tree,args.soc,args.check)
    except (OSError,ValueError,KeyError,subprocess.CalledProcessError) as error:
        parser.exit(1,str(error)+'\n')
    print('B package owner inputs checked' if args.check else 'B package generated sources staged')
if __name__=='__main__':main()
