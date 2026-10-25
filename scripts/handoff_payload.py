#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Deterministic independent 32MiB FAT16 for an X06 host-only dry run."""
import argparse,hashlib,json,struct,tomllib
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
TOTAL=65536;FAT_SECTORS=128;ROOT_SECTOR=257;DATA_SECTOR=289;CLUSTER=1024
DATE=((2026-1980)<<9)|(10<<5)|25
KERNEL_SHA='6c4993dea6d94544bc6f0c89bd72c6d292401ce2e19caaf65725749434c0c648'
DRIVER_SHA='ff6712315585ee940ba08ac049c396af945cf554927203f6f02d3c27b17db8d2'
FIXTURE_SHA='b0d09551ce2e8098953fa6679f52c63777f71b23812ce5aea21ca9186c65b9a4'
def sha(data):return hashlib.sha256(data).hexdigest()
def pe(raw,subsystem):
 if len(raw)<64 or raw[:2]!=b'MZ':raise ValueError('PE header required')
 offset=struct.unpack_from('<I',raw,60)[0]
 if offset+94>len(raw) or raw[offset:offset+4]!=b'PE\0\0' or struct.unpack_from('<H',raw,offset+4)[0]!=0xaa64 or struct.unpack_from('<H',raw,offset+92)[0]!=subsystem:raise ValueError('AARCH64 PE subsystem mismatch')
from fat16_payload import build as fat_build
def build(config,diag,driver,kernel,adt,viewer):
 parsed=tomllib.loads(config.decode())
 expected=tomllib.loads((ROOT/'config/handoff-dryrun.toml').read_text())
 if sha(config)!='9fff86baae6752c8905af6b6f59aa3e264db92ee4342ce193baa45b5f90ee607':raise ValueError('dry-run menu identity mismatch')
 if parsed!=expected:raise ValueError('fixed dry-run menu required')
 pe(diag,10);pe(driver,11);pe(viewer,10)
 if sha(driver)!=DRIVER_SHA:raise ValueError('fixed dry-run driver identity mismatch')
 if sha(kernel)!=KERNEL_SHA or sha(adt)!=FIXTURE_SHA:raise ValueError('X06 host kernel/fixture identity mismatch')
 content=[('EFI/UniXNU/unixnu.toml','UNIXNU','unixnu.toml',b'UNIXNU~1TOM',config),
          ('EFI/Tools/CasDiag.efi','TOOLS','CasDiag.efi',b'CASDIAG EFI',diag),
          ('EFI/Drivers/DarwinBoot.efi','DRIVERS','DarwinBoot.efi',b'DARWIN~1EFI',driver),
          ('EFI/Tools/CasTraceView.efi','TOOLS','CasTraceView.efi',b'CASTRA~1EFI',viewer),
          ('XNU/kernel.macho','XNU','kernel.macho',b'KERNEL~1MAC',kernel),
          ('XNU/kernel.macho.adt','XNU','kernel.macho.adt',b'KERNEL~1ADT',adt)]
 return fat_build(content)

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for name in ('diag','driver','kernel','adt','viewer','out'):ap.add_argument('--'+name,type=Path,required=True)
 a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False)
 data,files=build((ROOT/'config/handoff-dryrun.toml').read_bytes(),a.diag.read_bytes(),a.driver.read_bytes(),a.kernel.read_bytes(),a.adt.read_bytes(),a.viewer.read_bytes())
 (a.out/'payload-fat.img').write_bytes(data)
 (a.out/'manifest.json').write_text(json.dumps({'profile':'cas-x06-host-fixture','fat_bytes':len(data),'fat_sha256':sha(data),'files':files,
  'boot_ready':False,'kernel_executed':False,'host_fixture_contains_entropy':False,'phone_boot_forbidden':True,
  'warning':'X03 fixture has no fresh entropy. This payload is host-only and cannot be promoted to a phone candidate.'},indent=2)+'\n')
 print(json.dumps({'result':'built','bytes':len(data),'sha256':sha(data),'phone_boot_forbidden':True}))
if __name__=='__main__':main()
