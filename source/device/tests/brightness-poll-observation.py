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
soc=Path('/home/hua/nezha-work/ios27/brightness27-codex/ci-brightness003/source/soc/include')
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
assert sha(soc/'Sm8850DsiFifo.h')=='5f9a2ce924585f327c088bcdaf03e6f30fe4b1573b65b3df10eb02b71bc3442f'
def retention():
    source=(D/'package/backlight/BacklightProbe.cpp').read_text()
    for line in ['        const uint64_t faultGeneration=activeGeneration;\n',
                 '            nezha_dsi_observation_poll_begin(&dsi,activeGeneration,now,commands.acknowledged);\n',
                 '            nezha_dsi_observation_poll_end(&dsi,activeGeneration,result,&dsi.transfer);\n',
                 '            nezha_dsi_observation_begin(&dsi,activeGeneration,activeRequest,sending,commands.acknowledged,&dsi.transfer);\n']:
        assert source.count(line)==1
        source=source.replace(line,'',1)
    first=source.index('        struct nezha_dsi_observation observation;\n')
    last=source.index('\n    }\n    bool publishLevel',first)+1
    source=source[:first]+source[last:]
    source_digest=hashlib.sha256(source.encode()).hexdigest()
    assert source_digest=='1626d8d430f68398bf9e110d099c73ddbd588369f492db0b6c3600336aac8170'
    header=(D/'package/backlight/NezhaDsi0.h').read_text()
    include='#include "NezhaDsiObservation.h"\n';assert header.count(include)==1
    header=header.replace(include,'',1)
    new='        __asm__ volatile("dsb sy" ::: "memory");*value=self->words[offset/4];\n        nezha_dsi_observation_read(cookie,offset,*value);return true;'
    old='        __asm__ volatile("dsb sy" ::: "memory");*value=self->words[offset/4];return true;'
    assert header.count(new)==1;header=header.replace(new,old,1)
    header_digest=hashlib.sha256(header.encode()).hexdigest()
    assert header_digest=='28a964006beffe03238af70bc9eed4c479ca1aeaef574d4583f18f96fd1690e1'
    return dict(exact_source_after_observation_removal_matches003=True,
                source_after_observation_removal_SHA256=source_digest,
                exact_header_after_observation_removal_matches003=True,
                header_after_observation_removal_SHA256=header_digest)
retained=retention()
common=['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-omit-frame-pointer','-I'+str(D/'package/backlight'),'-I'+str(soc)]
def checked(name,command,directory):
    result=subprocess.run(command,capture_output=True,text=True)
    (directory/(name+'.log')).write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
    return result.stdout
checked('compile',common+[str(D/'tests/brightness-poll-observation.cpp'),'-o',str(O/'model')],O)
output=checked('run',[str(O/'model')],O)
assert 'PASS 12 selected-FIFO observation scenarios' in output

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
adapter=dict(actual_adapter_body_extracted=True,ASan_UBSan=True,scenarios=13,register_offset_cases=8192,
             source_SHA256=sha(D/'package/backlight/NezhaDsi0.h'),helper_SHA256=sha(D/'package/backlight/NezhaDsiObservation.h'),
             selected_SoC_header_SHA256=sha(soc/'Sm8850DsiFifo.h'),surrogate_layout_delta_bytes=0,hardware=False)
(AD/'qualified.json').write_text(json.dumps(adapter,indent=2)+'\n')
report=dict(passed=True,ASan_UBSan=True,actual_selected_FIFO=True,scenarios=12,
            actual_source_SHA256=sha(D/'package/backlight/BacklightProbe.cpp'),
            actual_header_SHA256=sha(D/'package/backlight/NezhaDsi0.h'),actual_helper_SHA256=sha(D/'package/backlight/NezhaDsiObservation.h'),
            selected_SoC_header_SHA256=sha(soc/'Sm8850DsiFifo.h'),actual_test_SHA256=sha(D/'tests/brightness-poll-observation.cpp'),
            exact_fakeIO_operations_and_results=True,guard_owner_generation=True,
            per_generation_poll_count_and_maxgap=True,cleanup_preserves_first_pair=True,
            partial_read_valid_masks=True,timeout_threshold_unchanged=True,one_fault_snapshot_no_IO=True,
            current_adapter_qualification=adapter,actual_native_layout_and_PAC_pending=True,hardware=False,phone_operations=False)
report.update(retained)
(O/'qualified.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
