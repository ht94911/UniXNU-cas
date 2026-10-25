#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Validate private CPU0 inputs and create a read-only FAT payload in local RAM boot."""
import hashlib,json,os,stat,tomllib
from pathlib import Path
from fat16_payload import build as fat_build
from handoff_payload import pe
from adt_codec import decode
from cas_adt import audit_tree,load_json,load_schema
ROOT=Path(__file__).resolve().parents[1]
def sha(raw):return hashlib.sha256(raw).hexdigest()
def build(config,diag,driver,kernel,adt,viewer,*,adt_report,loader_report,overlay):
 contract=json.loads((ROOT/'config/cpu0-contract.json').read_text());cfg=tomllib.loads(config.decode())
 if sha(config)!=contract['menu_sha256'] or cfg['boot']['default']!='trace' or cfg['boot']['timeout']!=-1:raise ValueError('CPU0 menu identity/default mismatch')
 if [x['id'] for x in cfg['entry']]!=['trace','diag','validate','cpu0','android','firmware']:raise ValueError('CPU0 entries mismatch')
 for role in ['validate','cpu0']:
  item=next(x for x in cfg['entry'] if x['id']==role)
  if item['type']!='xnu' or item['path']!='\\XNU\\kernel.macho' or item['args']!=contract['command_lines']['execute' if role=='cpu0' else 'validate']:raise ValueError('CPU0 arguments/path mismatch')
 pe(diag,10);pe(driver,11);pe(viewer,10)
 if len(kernel)!=contract['kernel']['bytes'] or sha(kernel)!=contract['kernel']['sha256']:raise ValueError('actual trace kernel identity mismatch')
 if loader_report.get('result')!='pass' or not loader_report.get('sources_unchanged') or loader_report['efi']['sha256']!=sha(driver):raise ValueError('loader build identity mismatch')
 if overlay.get('profile')!='cas-cpu0-execute' or not overlay.get('fresh_exact_range_probe_required'):raise ValueError('CPU0 guard profile missing')
 expected_tag=int.from_bytes(hashlib.sha256((contract['profile']+'execute'+contract['kernel']['sha256']+adt_report['adt_sha256']).encode()).digest()[:8],'little')
 if not overlay.get('recovered_validation_required') or overlay.get('candidate_tag')!=hex(expected_tag) or overlay.get('private_adt_sha256')!=adt_report['adt_sha256']:raise ValueError('candidate recovery tag mismatch')
 if adt_report.get('mode')!='private' or adt_report.get('cpu_profile')!='cpu0' or adt_report.get('entropy_bytes')!=64 or not adt_report.get('roundtrip_verified') or adt_report['adt_sha256']!=sha(adt) or adt_report['source_audit']['result']!='pass':raise ValueError('private CPU0 ADT provenance mismatch')
 tree=decode(adt);audit=audit_tree(tree,load_json(ROOT/'config/cas-adt-facts.json'),load_schema(ROOT/'config/cas-adt-schema.json'),cpu_profile='cpu0',mode='private')
 if audit['result']!='pass' or tree.encode()!=adt:raise ValueError('CPU0 ADT audit failed')
 content=[('EFI/UniXNU/unixnu.toml','UNIXNU','unixnu.toml',b'UNIXNU~1TOM',config),
  ('EFI/Tools/CasDiag.efi','TOOLS','CasDiag.efi',b'CASDIAG EFI',diag),
  ('EFI/Drivers/DarwinBoot.efi','DRIVERS','DarwinBoot.efi',b'DARWIN~1EFI',driver),
  ('EFI/Tools/CasTraceView.efi','TOOLS','CasTraceView.efi',b'CASTRA~1EFI',viewer),
  ('XNU/kernel.macho','XNU','kernel.macho',b'KERNEL~1MAC',kernel),
  ('XNU/kernel.macho.adt','XNU','kernel.macho.adt',b'KERNEL~1ADT',adt)]
 raw,files=fat_build(content,label=b'CAS_CPU0   ',serial=0x43415007)
 return raw,files,{'profile':contract['profile'],'private_entropy_bytes':64,'ADT_audit':'pass','requires_runtime_guard':True,'kernel_executed':False}
