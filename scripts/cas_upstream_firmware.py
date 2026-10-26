#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Require a reviewed candidate and stage its guarded loader for the standard firmware step."""
from pathlib import Path
import hashlib,json,os,subprocess,sys
def required(name):
 value=os.environ.get(name)
 if not value:raise SystemExit(name+' is required; no unbound cas firmware build')
 return Path(value).resolve()
device=required('UNIXNU_DEVICE');framework=required('UNIXNU_FRAMEWORK');uefi=required('UNIXNU_UEFI');edk2=required('UNIXNU_EDK2');tree=required('UNIXNU_BUILD_TREE');work=required('UNIXNU_BUILD_WORK')
if edk2!=(uefi/'Mu_Basecore').resolve():raise SystemExit('cas must use its pinned Mu_Basecore ABI; pass --edk2 UEFI/Mu_Basecore')
audit=required('CAS_ADT_AUDIT');contract_path=required('CAS_CPU0_CONTRACT');contract=json.loads(contract_path.read_text());report=json.loads(audit.read_text())
image=work/'payload/payload.img';payload_report=json.loads((work/'payload/payload.json').read_text())
if contract.get('payload_sha256')!=hashlib.sha256(image.read_bytes()).hexdigest() or payload_report['payload_sha256']!=contract['payload_sha256']:raise SystemExit('Actual RAM FAT differs from the candidate contract')
inputs=work/'cas-candidate-inputs'
expected={'kernel.macho':contract['kernel']['sha256'],'kernel.macho.adt':report['adt_sha256']}
for key,name in [('ramdisk','scratch-data.img'),('trustcache','kernel.macho.trustcache')]:
 if contract.get(key):expected[name]=contract[key]['sha256']
if not inputs.exists():
 inputs.mkdir(mode=0o700)
 for name in expected:subprocess.run(['mcopy','-i',str(image),'::/EFI/UniXNU/Payloads/'+name,str(inputs/name)],check=True)
if {p.name for p in inputs.iterdir()}!=set(expected):raise SystemExit('Unexpected extracted candidate inputs')
for name,digest in expected.items():
 path=inputs/name
 if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise SystemExit('Extracted candidate input differs: '+name)
kernel=inputs/'kernel.macho';adt=inputs/'kernel.macho.adt'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
if contract['kernel']['sha256']!=sha(kernel) or report['adt_sha256']!=sha(adt):raise SystemExit('Actual payload differs from candidate contract/audit')
if contract.get('cpu_count')!=1 or contract.get('internal_storage') or contract.get('Data_volume') or contract.get('requires_recovered_validation') is not False:raise SystemExit('Expected single-core RAM-only candidate without repeated prior-boot validation')
for key,name in [('ramdisk','scratch-data.img'),('trustcache','kernel.macho.trustcache')]:
 entry=contract.get(key)
 if entry:
  path=kernel.parent/name
  if not path.is_file() or entry['sha256']!=sha(path) or entry['bytes']!=path.stat().st_size:raise SystemExit('Actual RAM-root input differs: '+key)
link=tree/'drivers/DarwinBoot';staged=work/'cas-guarded-loader'
if staged.exists():raise SystemExit('Use a fresh firmware candidate directory; existing loader stage is preserved')
sys.path.insert(0,str(device/'scripts'));import cas_cpu0
cas_cpu0.ADT_AUDIT=audit;cas_cpu0.MODE='execute';cas_cpu0.prepare(framework,staged,uefi,contract_path)
p=staged/'DarwinBootPkg/Driver/DarwinBoot.inf';text=p.read_text();text+='\n[BuildOptions]\n  *_CLANGPDB_*_CC_FLAGS = -D CAS_GENERATED_OWNERSHIP_GUARD=1 -D CAS_XNU_DIAGNOSTICS=1 -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function\n';p.write_text(text)
target=staged/'DarwinBootPkg/Driver/DarwinBoot.inf'
if not target.is_file() or 'CasCpu0ProbeHeld' not in (target.parent/'DarwinBoot.c').read_text():raise SystemExit('Guarded loader absent')
if not link.is_symlink():raise SystemExit('Refusing to replace an unidentified source checkout')
link.unlink();link.symlink_to(staged,target_is_directory=True)
(work/'firmware-guard-stage.json').write_text(json.dumps({'kernel_sha256':sha(kernel),'adt_sha256':sha(adt),'contract_sha256':sha(contract_path),'private_audit_sha256':sha(audit),'guarded_loader_inf_sha256':sha(target),'source_checkout_mutated':False},indent=2)+'\n')
print('cas guarded firmware: exact candidate identity, pinned EDK2 and fresh runtime held-probe retained')
