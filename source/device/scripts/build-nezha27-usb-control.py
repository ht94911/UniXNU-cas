#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the board USB reset/snapshot adapter with actual XNU27 BSD flags."""
from pathlib import Path
import json,subprocess,hashlib
from kernel_target import check_object
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/nezha27-usb-control';OUT.mkdir(parents=True,exist_ok=True)
source=ROOT/'kits/nezha/Nezha27UsbControl.c';obj=OUT/'Nezha27UsbControl.o';record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','));command=[];args=record['arguments'];i=0
while i<len(args):
 value=args[i]
 if value==record['file']:command.append(str(source))
 elif value=='-o':command += ['-o',str(obj)];i+=1
 elif value in ('-MF','-MT','-MJ'):i+=1
 elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
 elif not value.startswith('-W'):command.append(value)
 i+=1
command+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-variable','-Wno-unknown-warning-option','-Wno-missing-designated-field-initializers','-mgeneral-regs-only','-I',str(ROOT/'drivers/QcomXnuUsbDebug/include')]
r=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True);(OUT/'compile.log').write_text(r.stdout+r.stderr);(OUT/'compile-command.json').write_text(json.dumps(command,indent=2));assert r.returncode==0,r.stdout+r.stderr;check_object(obj.read_bytes(),'nezha27 USB controlled reboot and retained-mapping RGB snapshot');archive=OUT/'libNezha27UsbControl.a';archive.unlink(missing_ok=True);subprocess.run(['xcrun','ar','rcs',archive,obj],check=True);(OUT/'build-report.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),no_fixed_timer=True,RAM_only_direct_reset_requires_session_owner_generation_drain=True,persistent_Data_requires_normal_XNU_shutdown_and_healthy_store=True,framebuffer_is_retained_existing_CPU_mapping=True,hardware_qualified=False),indent=2));print('actual27 USB board adapter compiled')
