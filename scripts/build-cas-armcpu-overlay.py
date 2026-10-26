#!/usr/bin/env python3
"""Stage cas-only diagnostic additions in the generated tree, never the source module."""
from pathlib import Path
import hashlib,json,shutil,subprocess
ROOT=Path(__file__).resolve().parents[1];inputs=ROOT/'kits/cas/CasTrace';spec=json.loads((inputs/'armcpu.json').read_text());link=ROOT/'drivers/ArmCpu';output=ROOT/'build/cas-armcpu-overlay';stamp=output/'cas-overlay.json'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
if not link.is_symlink():raise SystemExit('ArmCpu source must remain an identified generated-tree symlink')
original=link.resolve()
if original==output.resolve():
 report=json.loads(stamp.read_text())
 if sha(output/'ArmCpu.cpp')!=spec['after_sha256'] or report['include_sha256']!=sha(output/spec['include']):raise SystemExit('Staged ArmCpu changed')
 raise SystemExit(0)
if output.exists():raise SystemExit('Unrecognized ArmCpu staging output already exists')
for key in ['patch','include']:
 if sha(inputs/spec[key])!=spec[key+'_sha256']:raise SystemExit('Diagnostic input hash mismatch: '+key)
shutil.copytree(original,output,ignore=shutil.ignore_patterns('.git','build','__pycache__'))
for name in [spec['asm_patch'],spec['patch']]:
 for flags in [['--check'],[]]:subprocess.run(['git','apply',*flags,str(inputs/name)],cwd=output,check=True)
if sha(output/'ArmCpu.cpp')!=spec['after_sha256']:raise SystemExit('ArmCpu overlay result mismatch')
shutil.copyfile(inputs/spec['include'],output/spec['include'])
report={'original_source':str(original),'original_source_sha256':sha(original/'ArmCpu.cpp'),'overlay_sha256':sha(output/'ArmCpu.cpp'),'include_sha256':sha(output/spec['include']),'module_checkout_mutated':False}
stamp.write_text(json.dumps(report,indent=2)+'\n');link.unlink();link.symlink_to(output,target_is_directory=True)
print('cas ArmCpu: versioned diagnostic overlay staged; source module untouched')
