#!/usr/bin/env python3
"""Exercise the actual HID callback pacing, contact order and error handling."""
from pathlib import Path
import argparse,hashlib,json,subprocess

PREFIX = '\n#include <cassert>\n#include <cstring>\n#include <vector>\n#include <cstdio>\n#include <stdint.h>\n#include "NezhaHidPacing.h"\n#include "NezhaInputTelemetry.h"\n#define TOUCH_FINGERS 10\nusing IOReturn=int;\nconstexpr int kIOReturnSuccess=0,kIOReturnNoMemory=12,kIOHIDReportTypeInput=1;\nstruct OSObject {virtual ~OSObject()=default;};\n#define OSDynamicCast(T,p) dynamic_cast<T *>(p)\nstruct IOTimerEventSource {\n std::vector<unsigned> arms;unsigned cancels=0;\n void setTimeoutMS(unsigned n){arms.push_back(n);}void cancelTimeout(){++cancels;}\n};\nstruct Buffer {bool broken=false;size_t writeBytes(size_t,const void*,size_t n){return broken?0:n;}};\nvolatile nzi_frame nezha_hid_input_diag={};\nstatic uint64_t clock_ticks=100;\nstatic uint64_t mach_absolute_time(){return ++clock_ticks;}\nstatic unsigned native_touch_hid_reports;\nstatic void native_hid_input_observed(unsigned,int){}\nstruct NativeHidInput:OSObject {\n nezha_hid_pacer touchState={};Buffer storage;Buffer *touchBuffer=&storage;\n bool touchFailed=false,aaAckPending=false,aaReplay=false,ackBusy=false,replay=false,ready=true;\n unsigned touchLogged=99,dispatched=0,acks=0;int result=0;std::vector<nezha_touch_frame> frames;unsigned at=0;\n unsigned armed_when_dispatched=0;IOTimerEventSource *timer=nullptr;\n bool finishAA27Ack(){return !ackBusy;}\n bool hasEventDriverCached(){return ready;}\n static bool read(void *p,nezha_touch_frame *out){auto *s=(NativeHidInput*)p;if(s->at==s->frames.size())return false;*out=s->frames[s->at++];return true;}\n bool pickTouch(){aaReplay=replay;return nezha_hid_pick(&touchState,read,this);}\n void acknowledgeAA27(uint32_t,bool){++acks;}\n int handleReport(Buffer*,int,int){++dispatched;armed_when_dispatched=timer->arms.back();clock_ticks+=5000;return result;}\n void add(unsigned mask,unsigned x){nezha_touch_frame f={};f.sequence=frames.size()+1;f.active=mask;f.x[0]=x;f.y[0]=100;frames.push_back(f);}\n'
TEST = '\n};\nstatic void call(NativeHidInput &n,IOTimerEventSource &t){n.timer=&t;NativeHidInput::touchTick(&n,&t);}\nint main(){\n {NativeHidInput n;IOTimerEventSource t;n.add(1,30);call(n,t);assert(t.arms==std::vector<unsigned>{8});assert(n.armed_when_dispatched==8&&n.dispatched==1&&n.touchState.delivered.active==1);n.add(0,30);n.add(1,40);call(n,t);assert(!n.touchState.delivered.active);call(n,t);assert(n.touchState.delivered.active==1&&n.dispatched==3);for(unsigned x:t.arms)assert(x==8);}\n {NativeHidInput n;IOTimerEventSource t;n.touchState.delivered.active=1;for(unsigned i=0;i<63;i++)n.add(1,i+1);n.add(0,63);call(n,t);assert(n.touchState.delivered.x[0]==63&&n.touchState.coalesced==62&&n.touchState.has_lookahead);call(n,t);assert(!n.touchState.delivered.active&&n.dispatched==2);}\n {NativeHidInput n;IOTimerEventSource t;n.ackBusy=true;n.add(1,50);call(n,t);assert(t.arms==std::vector<unsigned>{16}&&n.dispatched==0&&n.at==0);}\n {NativeHidInput n;IOTimerEventSource t;n.replay=true;n.add(1,60);call(n,t);assert(t.arms==std::vector<unsigned>({8,16})&&n.armed_when_dispatched==16&&n.dispatched==1);}\n {NativeHidInput n;IOTimerEventSource t;n.result=7;n.add(1,70);call(n,t);assert(n.touchFailed&&t.cancels==1&&n.dispatched==1&&n.touchState.ready);auto f=n.touchState.pending;unsigned arms=t.arms.size();call(n,t);assert(n.dispatched==1&&t.arms.size()==arms&&!memcmp(&f,&n.touchState.pending,sizeof(f)));}\n {NativeHidInput n;IOTimerEventSource t;n.storage.broken=true;n.add(1,80);call(n,t);assert(n.touchFailed&&n.dispatched==0&&t.cancels==1);}\n puts("PASS 6 actual HID owner cases: 8 ms physical input, early-only arm, ordered edges/coalescing, 16 ms explicit test steps, busy acknowledgement and terminal errors");\n}\n'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--cc',default='c++')
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=False)
    source=args.device/'package/nezha27-input/NativeHidInput.cpp'
    text=source.read_text()
    start=text.index('    static void touchTick(')
    end=text.index('    bool hasEventDriverCached()',start)
    actual=text[start:end]
    cpp=args.out/'actual.cpp'
    cpp.write_text(PREFIX+actual+TEST)
    command=[args.cc,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
             '-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all',
             '-I',str(args.device/'package/nezha27-input'),str(cpp),'-o',str(args.out/'actual')]
    result=subprocess.run(command,capture_output=True,text=True)
    (args.out/'build.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise SystemExit(result.stderr)
    result=subprocess.run([str(args.out/'actual')],capture_output=True,text=True)
    (args.out/'run.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise SystemExit(result.stdout+result.stderr)
    report=dict(passed=True,cases=6,actual_callback_and_report_executed=True,
                original_coalescer_used=True,physical_interval_ms=8,explicit_test_interval_ms=16,
                ASan_UBSan=True,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                original_IOKit_and_board_dependencies_mocked=True,phone_operations=False,
                hardware_improvement_verified=False)
    (args.out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(result.stdout)

if __name__=='__main__':main()
