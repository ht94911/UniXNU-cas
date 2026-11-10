#!/usr/bin/env python3
"""Run retained98 and explicit-budget models against the actual SoC header."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

S=Path(__file__).resolve().parents[1]
O=S/'build/dsi-fifo005-host';O.mkdir(parents=True,exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
common=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer']
results={}
for name in ['dsi-fifo','dsi-fifo-budget']:
    source=S/'tests'/(name+'.c');binary=O/name
    for phase,command in [('compile',common+[str(source),'-o',str(binary)]),('run',[str(binary)])]:
        result=subprocess.run(command,capture_output=True,text=True)
        (O/(name+'-'+phase+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,result.stdout+result.stderr
    cases=int(re.search(r'PASS (\d+) actual-source',result.stdout).group(1))
    results[name]=dict(cases=cases,source_SHA256=sha(source),run_log=str(O/(name+'-run.log')))
assert results['dsi-fifo']['cases']==98 and results['dsi-fifo-budget']['cases']==18
report=dict(passed=True,ASan_UBSan=True,actual_source_header_SHA256=sha(S/'include/Sm8850DsiFifo.h'),
            retained_protocol_cases=98,new_budget_DONE_cases=18,models=results,
            existing_default_budget_us=5000,explicit_single_budget_test_us=1200000,
            default_observe_poll_IO_sequences_equal_to_explicit5000=True,
            STATUS_read_retained_on_DONE=True,completion_predicate_DONE_only=True,
            preflight_DMA_BUSY_refusal_retained=True,errors_lease_cleanup_W1C_no_retry_retained=True,
            caller_owned_began_us_origin_tested=True,timeout_layout_members_added=False,
            OEM_repository='https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850',
            OEM_revision='5ab2a689ff87d7d28c511f1762cf41c1b90d965a',
            hardware=False,phone_operations=False,native_layout_ABI_and_hardware_pending=True)
(O/'qualified.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
