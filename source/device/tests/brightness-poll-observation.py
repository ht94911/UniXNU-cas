#!/usr/bin/env python3
"""Qualify board observation against actual selected FIFO and adapter source."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

D=Path(__file__).resolve().parents[1]
O=D/'build/brightness-poll-host';O.mkdir(parents=True,exist_ok=True)
AD=D/'build/brightness-dsi0-host';AD.mkdir(parents=True,exist_ok=True)
soc=Path('/home/hua/.codex/worktrees/brightness27-dsi005/soc-dsi067/include')
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
assert sha(soc/'Sm8850DsiFifo.h')=='d98354f8ebd13b492292fa9c480985b4ec0eeed6a26c4e4397fe4e3be5c68def'
#005 changes production completion/budget/start behavior. Preserve the actual
#004 adapter/helper bytes,without reusing004's observation-only source claim.
import subprocess as source_git
baseline='63fb5730e04563066a83803cb5f19716fd731326'
for name in ['NezhaDsi0.h','NezhaDsiObservation.h']:
    previous=source_git.check_output(['git','-C',str(D),'show',baseline+':package/backlight/'+name])
    assert previous==(D/'package/backlight'/name).read_bytes(),name
retained=dict(exact_adapter_and_observer_helper_bytes_match004=True,
              observer_helper_SHA256=sha(D/'package/backlight/NezhaDsiObservation.h'),
              production_behavior_changed=True,single_budget_us=1200000,
              completion_predicate='DMA_DONE-only',clock_origin='successful trigger return')
common=['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-omit-frame-pointer','-I'+str(D/'package/backlight'),'-I'+str(soc)]
def checked(name,command,directory):
    result=subprocess.run(command,capture_output=True,text=True)
    (directory/(name+'.log')).write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    return result.stdout
checked('compile',common+[str(D/'tests/brightness-poll-observation.cpp'),'-o',str(O/'model')],O)
output=checked('run',[str(O/'model')],O)
assert 'PASS 14 selected-FIFO observation scenarios' in output

# Preserve the prior13 actual board-map cases and8192 offset cases, substituting
# the current actual struct. ARM barriers alone are replaced by host counters.
previous=Path('/home/hua/nezha-work/private/ios27/g6-backlight068-adapter-host-v2/adapter.cpp').read_text()
old_start=previous.index('struct NezhaDsi0 {');old_end=previous.index('\nint main()')
head,old_body,tail=previous[:old_start],previous[old_start:old_end],previous[old_end:]
source=(D/'package/backlight/NezhaDsi0.h').read_text()
body=source[source.index('struct NezhaDsi0 {'):]
body=re.sub(r'__asm__ volatile\("dsb (?:sy|oshst)" ::: "memory"\);','host_barrier();',body)
head+='#include "NezhaDsiObservation.h"\n'
baseline=old_body.replace('NezhaDsi0','BaselineNezhaDsi0')
tail=tail.replace('int main(){unsigned cases=0;','int main(){unsigned cases=0;assert(sizeof(NezhaDsi0)==sizeof(BaselineNezhaDsi0));')
model=AD/'adapter.cpp';model.write_text(head+baseline+body+tail)
checked('compile',common+[str(model),'-o',str(AD/'model')],AD)
output=checked('run',[str(AD/'model')],AD)
assert 'PASS 13 adapter scenarios and8192 register offsets' in output
adapter=dict(passed=True,actual_adapter_body_extracted=True,ASan_UBSan=True,scenarios=13,register_offset_cases=8192,
             source_SHA256=sha(D/'package/backlight/NezhaDsi0.h'),helper_SHA256=sha(D/'package/backlight/NezhaDsiObservation.h'),
             selected_SoC_header_SHA256=sha(soc/'Sm8850DsiFifo.h'),actual_test_SHA256=sha(model),
             generator_SHA256=sha(Path(__file__)),baseline_test_SHA256=sha(Path('/home/hua/nezha-work/private/ios27/g6-backlight068-adapter-host-v2/adapter.cpp')),
             surrogate_layout_delta_bytes=0,hardware=False)
(AD/'qualified.json').write_text(json.dumps(adapter,indent=2)+'\n')
report=dict(passed=True,ASan_UBSan=True,actual_selected_FIFO=True,scenarios=14,retained_observation_scenarios=12,
            actual_source_SHA256=sha(D/'package/backlight/BacklightProbe.cpp'),
            actual_header_SHA256=sha(D/'package/backlight/NezhaDsi0.h'),actual_helper_SHA256=sha(D/'package/backlight/NezhaDsiObservation.h'),
            selected_SoC_header_SHA256=sha(soc/'Sm8850DsiFifo.h'),actual_test_SHA256=sha(D/'tests/brightness-poll-observation.cpp'),
            generator_SHA256=sha(Path(__file__)),
            exact_fakeIO_operations_and_results=True,guard_owner_generation=True,
            per_generation_poll_count_and_maxgap=True,cleanup_preserves_first_pair=True,
            partial_read_valid_masks=True,old_wrapper_timeout_us=5000,explicit_single_budget_us=1200000,one_fault_snapshot_no_IO=True,
            current_adapter_qualification=adapter,actual_native_layout_and_PAC_pending=True,hardware=False,phone_operations=False)
report.update(retained)
(O/'qualified.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
