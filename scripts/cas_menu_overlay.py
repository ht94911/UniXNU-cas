#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage a hash-guarded local menu viewport overlay without changing framework."""
import hashlib,json,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stage(framework,out):
 framework=Path(framework).resolve();out=Path(out).resolve();patches=ROOT/'patches/cpu0-menu';lock=json.loads((patches/'source-lock.json').read_text())
 if subprocess.check_output(['git','rev-parse','HEAD'],cwd=framework,text=True).strip()!=lock['framework_commit'] or subprocess.check_output(['git','status','--porcelain'],cwd=framework).strip():raise ValueError('menu overlay requires clean pinned framework')
 if sha(patches/'scroll.patch')!=lock['patch_sha256'] or sha(patches/'MenuViewport.h')!=lock['viewport_sha256']:raise ValueError('menu overlay patch identity mismatch')
 for name,item in lock['files'].items():
  if sha(framework/'package/UniXNUPkg/Library/UniXNULib'/name)!=item['before_sha256']:raise ValueError('menu base source mismatch: '+name)
 out.mkdir(parents=True,exist_ok=False);pkg=out/'UniXNUPkg';shutil.copytree(framework/'package/UniXNUPkg',pkg)
 for flags in (['--check'],[]):subprocess.run(['git','apply',*flags,patches/'scroll.patch'],cwd=pkg,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True)
 shutil.copyfile(patches/'MenuViewport.h',pkg/'Library/UniXNULib/MenuViewport.h')
 for name,item in lock['files'].items():
  if sha(pkg/'Library/UniXNULib'/name)!=item['after_sha256']:raise ValueError('menu staged source mismatch: '+name)
 result={'profile':'cpu0-menu-scroll','source_lock':lock,'source_lock_sha256':sha(patches/'source-lock.json'),'patch_sha256':sha(patches/'scroll.patch'),'source_framework_unchanged':not subprocess.check_output(['git','status','--porcelain'],cwd=framework).strip(),'hardware_tested':False}
 (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n');return result
