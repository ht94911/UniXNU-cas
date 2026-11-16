#!/usr/bin/env python3
"""Retain the real backlight-class model and test only its trace quotas.

The IOService/DSI model is not Apple ABI or physical brightness evidence.
"""
import ast
import hashlib
import json
import re
from pathlib import Path
import subprocess

D=Path(__file__).resolve().parents[1]
O=D/'build/brightness-trace-host';O.mkdir(parents=True,exist_ok=True)
reference=Path('/home/hua/nezha-work/ios27/brightness27-codex/test-service005.py')
nodes=ast.parse(reference.read_text()).body
parts={n.targets[0].id:ast.literal_eval(n.value) for n in nodes
       if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name)
       and n.targets[0].id in ('head','tail')}
source=(D/'package/backlight/BacklightProbe.cpp').read_text()
body=source[source.index('class NativeNezhaBacklight :'):source.index('OSDefineMetaClassAndStructors(')]
before=subprocess.check_output(['git','-C',str(D),'show','208c32d:package/backlight/BacklightProbe.cpp'],text=True)
old_body=before[before.index('class NativeNezhaBacklight :'):before.index('OSDefineMetaClassAndStructors(')]
old_body=old_body.replace('NativeNezhaBacklight','TraceBaselineBacklight')
# Access only test counters. Every executed method remains the production body.
head=parts['head'].replace('#include <cstring>','#include <cstring>\n#include <cstdarg>\n#include <thread>')
head=head.replace('#define OSDeclareDefaultStructors(T) public:T()=default;private:',
                  '#define OSDeclareDefaultStructors(T) public:T()=default;public:')
constant=re.search(r'^static constexpr uint64_t NEZHA_DSI0_COMMAND_BUDGET_US=.*;$',source,re.M).group(0)
head+='\n'+constant+'\n'
head+=r'''
static unsigned traceLines,unsupportedLines;
static int tracePrintf(const char*format,...) {
 va_list args;va_start(args,format);
 if(!strncmp(format,"NEZHA BACKLIGHT TRACE:",21)) {
  (void)va_arg(args,unsigned long long);const char*phase=va_arg(args,const char*);
  if(!strcmp(phase,"unsupported"))__atomic_fetch_add(&unsupportedLines,1,__ATOMIC_RELAXED);
  else __atomic_fetch_add(&traceLines,1,__ATOMIC_RELAXED);
  va_end(args);return 1;
 }
 int result=vprintf(format,args);va_end(args);return result;
}
#define printf(...) tracePrintf(__VA_ARGS__)
'''
tail=parts['tail']
position=tail.index('int main()')
checks=r'''
static void quotaChecks() {
 assert(sizeof(NativeNezhaBacklight)==sizeof(TraceBaselineBacklight)); // new counter uses existing tail padding
 IOService provider;auto_complete=true;force_refused=false;
 native_source_storage_boot_state=3;nezha27_usb_reset_owner_ready=1;
 NativeNezhaBacklight b;assert(b.init(nullptr)&&b.start(&provider));pump(1);pump(100);
 assert(b.published);b.traceEnabled=1;traceLines=unsupportedLines=0;
 auto*unsupported=OSDictionary::withCapacity(1);
 unsigned requests=nezha_backlight_requests,writes=hardware_writes;
 for(unsigned i=0;i<10000;++i)assert(b.setProperties(unsupported)==kIOReturnUnsupported);
 assert(b.unsupportedTraceCount==8&&unsupportedLines==8&&b.traceCount==0&&traceLines==0);
 assert(nezha_backlight_requests==requests&&hardware_writes==writes);
 assert(b.setProperties(request(1234))==kIOReturnSuccess);pump(1);pump(100);assert(reported(b)==1234);
 assert(b.traceCount>0&&b.traceCount==traceLines); // Unsupported did not consume control-chain quota.
 for(unsigned i=0;i<10000;++i)b.trace("budget-model",1,1,1,0);
 assert(b.traceCount==8192&&traceLines==8192&&b.unsupportedTraceCount==8);
 b.traceCount=UINT32_MAX;b.unsupportedTraceCount=UINT32_MAX;
 b.trace("budget-model",1,1,1,0);assert(b.setProperties(unsupported)==kIOReturnUnsupported);
 assert(b.traceCount==UINT32_MAX&&b.unsupportedTraceCount==UINT32_MAX&&traceLines==8192&&unsupportedLines==8);
 b.free();
 NativeNezhaBacklight separate;separate.published=true;separate.traceEnabled=1;
 for(unsigned i=0;i<20;++i)assert(separate.setProperties(unsupported)==kIOReturnUnsupported);
 assert(separate.unsupportedTraceCount==8&&unsupportedLines==16); // Per instance, not a global quota.
 NativeNezhaBacklight disabled;disabled.published=true;
 disabled.trace("budget-model",1,1,1,0);assert(disabled.setProperties(unsupported)==kIOReturnUnsupported);
 assert(disabled.traceCount==0&&disabled.unsupportedTraceCount==0&&unsupportedLines==16&&traceLines==8192);
 NativeNezhaBacklight concurrent;concurrent.traceEnabled=1;traceLines=unsupportedLines=0;
 std::vector<std::thread> workers;
 for(unsigned worker=0;worker<8;++worker)workers.emplace_back([&] {
  for(unsigned i=0;i<2048;++i){concurrent.trace("budget-model",1,1,1,0);concurrent.trace("unsupported",0,0,0,kIOReturnUnsupported,true);}
 });
 for(auto&t:workers)t.join();
 assert(concurrent.traceCount==8192&&concurrent.unsupportedTraceCount==8&&traceLines==8192&&unsupportedLines==8);
 std::printf("TRACE LAYOUT MODEL: old=%zu new=%zu delta=%zu;surrogate base,not Apple ABI\n",
     sizeof(TraceBaselineBacklight),sizeof(NativeNezhaBacklight),sizeof(NativeNezhaBacklight)-sizeof(TraceBaselineBacklight));
}
'''
tail=tail[:position]+checks+tail[position:]
tail=tail.replace(' std::printf("PASS %u actual service-source model scenarios;',
                  ' quotaChecks();\n std::printf("PASS %u actual service-source model scenarios;')
model=O/'actual-service.cpp';model.write_text(head+old_body+body+'\n#undef printf\n'+tail)
command=['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
         '-fsanitize=address,undefined','-fno-omit-frame-pointer',
         '-I'+str(D/'package/backlight'),'-I/home/hua/.codex/worktrees/brightness27-dsi005/soc-dsi067/include',
         str(model),'-o',str(O/'model')]
subprocess.run(command,check=True)
result=subprocess.run([str(O/'model')],capture_output=True,text=True)
(O/'run.log').write_text(result.stdout+result.stderr)
assert result.returncode==0,result.stdout+result.stderr
report=dict(passed=True,actual_class_body_extracted=True,ASan_UBSan=True,
            source_SHA256=hashlib.sha256(source.encode()).hexdigest(),retained_service_scenarios=16,
            control_trace_budget=8192,unsupported_trace_budget=8,unsupported_quota_independent=True,
            counters_saturate=True,UINT32_MAX_does_not_wrap=True,concurrent_quota_checked=True,
            trace_disabled_does_not_advance=True,unsupported_return_and_no_queue_write_preserved=True,
            successful_control_after_unsupported_noise=True,per_instance_quota=True,
            surrogate_layout_delta_bytes=0,expected_original_Apple_base_bytes=880,
            expected_native_derived_bytes=2200,native_allocation_and_PAC_pending=True,
            hardware=False,phone_operations=0,behavior_revision="005",new_behavior_scenarios=6,selected_SoC_header_SHA256=hashlib.sha256(Path('/home/hua/.codex/worktrees/brightness27-dsi005/soc-dsi067/include/Sm8850DsiFifo.h').read_bytes()).hexdigest())
report.update(actual_header_SHA256=hashlib.sha256((D/'package/backlight/NezhaDsi0.h').read_bytes()).hexdigest(),
              actual_helper_SHA256=hashlib.sha256((D/'package/backlight/NezhaDsiObservation.h').read_bytes()).hexdigest(),
              actual_test_SHA256=hashlib.sha256(model.read_bytes()).hexdigest(),
              generator_SHA256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              reference_generator_SHA256=hashlib.sha256(reference.read_bytes()).hexdigest())
(O/'qualified.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
