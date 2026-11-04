#!/usr/bin/env python3
"""Verify actual target artifacts contain the intended safe full-chain path."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True)
a=p.parse_args();root=a.unixnu.resolve()
bundle=root/'build/adreno-metal-plugin27/NativeAdrenoMetal.bundle'
data=(bundle/'NativeAdrenoMetal').read_bytes()
profile=json.loads((bundle/'gpu-build.json').read_text())
assert profile['target']=='ios27' and profile['iphone_24a446'] and not profile['ipad_private_layout_enabled']
for text in (b'PUB iPhone24A446: public policy; iPad raw offsets disabled',
             b'FULL begin: raw,Turnip,Metal checks; UI remains independent',
             b'FULL returned CP=%d; T/M/F are separate verdicts'):
    assert text in data,text
for text in (b'ca virtualized key %.*s = %u',b'ca virtualized key ? flags %llx = %u'):
    assert text not in data,'iPad-only raw reader survived in the target artifact'
subprocess.run(['codesign','--verify','--strict',str(bundle)],check=True)
assert b'M cache: reuse published runtime writable directory' in data
assert b'M cache rejected; test not started; UI remains independent' in data
library=root/'build/mesa-tu-ios27/src/freedreno/vulkan/libvulkan_freedreno.dylib'
symbols=subprocess.check_output(['nm',str(library)],text=True)
assert not re.search(r'_+ZL2dw(?:\b|\.)',symbols), 'old process-global connection survived'
assert 'dw_registry_lock' in symbols and 'darwin_states' in symbols
matches=[line.split()[-1] for line in symbols.splitlines() if 'tu_cmd_buffer_begin' in line and ' T ' in line]
assert len(matches)==1,matches
assembly=subprocess.check_output(['llvm-objdump','--disassemble-symbols='+matches[0],'--no-show-raw-insn',str(library)],text=True)
assert 'tu_cs_reserve_space' in assembly
assert re.search(r'mov\s+w1,\s*#(?:0x1000|4096)\b',assembly)
(a.logs/'actual-command-begin-guard.asm').write_text(assembly)
contexts=json.loads((a.logs/'device-context-qualified.json').read_text())
assert contexts['actual_prepared_sources_match'] and contexts['new_ownership_and_guard_cases_passed']
report=dict(actual_target_checked=True,plugin_sha256=hashlib.sha256(data).hexdigest(),
            profile=profile,required_markers_present=True,iPad_raw_reader_markers_absent=True,
            bundle_signature_verified=True,host_pixel_result='reuse unchanged-runtime host010 failure evidence;not passed',
            actual_device_registry_compiled=True,old_singleton_symbol_absent=True,
            actual_initial_command_reserve_compiled=True,fulltest_runtime_cache_handoff_compiled=True,
            actual_Mac_source_bound_context_tests=contexts,
            real_A840_shader_or_UI_verified=False,phone_operations=False)
(a.logs/'full-target-qualified.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='profile'},indent=2))
