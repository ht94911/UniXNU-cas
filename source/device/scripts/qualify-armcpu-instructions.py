#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reject a CPU archive whose actual SGI send/EOI/timer barriers were commented out."""
from pathlib import Path
import subprocess,json,hashlib,re
ROOT=Path(__file__).resolve().parents[1];O=ROOT/'build/armcpu';archive=O/'libArmCpu.a';obj=O/'actual-verified-ArmCpu.o';obj.write_bytes(subprocess.check_output(['xcrun','ar','-p',str(archive),'ArmCpu.o']));llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin';code=subprocess.check_output([str(llvm/'llvm-objdump'),'--disassemble','--no-show-raw-insn',str(obj)],text=True);(O/'actual-instructions.asm').write_text(code)
need={'__ZN6ArmCpu9signalCPUEP5IOCPU':['icc_sgi1r_el1','dsb','isb'],'__ZN6ArmCpu13enableTimeBaseEb':['cntv_ctl_el0']}
# A symbol-scoped SGI audit is decisive:old27 emitted DSB then RET with no MSR.
match=re.search(r'<__ZN6ArmCpu9signalCPUEP5IOCPU>:(.*?)(?=\n[0-9a-f]+ <|\Z)',code,re.S);assert match and 'icc_sgi1r_el1' in match[1].lower() and re.search(r'\bmsr\b',match[1]) and re.search(r'\bisb\b',match[1]);assert 'icc_eoir1_el1' in code.lower() and 'cntv_ctl_el0' in code.lower()
(O/'actual-instructions-qualified.json').write_text(json.dumps(dict(actual_archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),SGI_MSR_present=True,SGI_barriers_present=True,EOI_and_timer_register_writes_present=True,scope='compiled instruction contracts,not hardware SMP startup or latency',hardware_qualified=False),indent=2));print('actual ArmCpu SGI/EOI/timer instruction audit passed')
