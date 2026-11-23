#!/usr/bin/env python3
"""Exercise the actual locationd object sampler with saved thread-state fixtures."""
from pathlib import Path
import argparse
import hashlib
import json
import sys
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--exception-source', type=Path)
parser.add_argument('--event-source', type=Path)
parser.add_argument('--out', type=Path)
args = parser.parse_args()
if bool(args.exception_source) != bool(args.event_source):
    parser.error('both compiled sources are required together')
added = ''.join(line[1:] for line in
                (ROOT / 'patches/xnu-12377/nezha-gnss-pac-capture.patch').read_text().splitlines(True)
                if line.startswith('+') and not line.startswith('+++'))
start = added.index('struct sheng_gnss_object_peer {')
end = added.index('#endif /* __arm64__ */', start)
actual = added[start:end]
helper_start = added.index('void\nsheng_gnss_waiter_snapshot(thread_t thread, uint64_t words[32])')
helper_end = added.index('#endif /* __arm64__ */', helper_start)
helper = added[helper_start:helper_end]
if args.exception_source:
    exception = args.exception_source.read_text()
    event = args.event_source.read_text()
    if actual not in exception or helper not in event:
        raise SystemExit('actual compiled sampler/helper differs from the qualified patch')
    actual_start = exception.index('struct sheng_gnss_object_peer {')
    actual_end = exception.index('#endif /* __arm64__ */', actual_start)
    actual = exception[actual_start:actual_end]
    helper_begin = event.index('void\nsheng_gnss_waiter_snapshot(thread_t thread, uint64_t words[32])')
    helper_finish = event.index('#endif /* __arm64__ */', helper_begin)
    helper = event[helper_begin:helper_finish]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#define uint64_t unsigned long long
#define FALSE 0
#define TH_WAIT 1
#define TH_RUN 4
#define TH_TERMINATE 16
#define TH_TERMINATE2 32
#define MACH_BSD 1
#define SYS_kevent_id 375
#define kThreadWaitWorkloopSyncWait 0x10
struct kevent_qos_s {
 uint64_t ident; int16_t filter; uint16_t flags; uint32_t qos;
 uint64_t udata; uint32_t fflags,xflags; uint64_t data,ext[4];
};
struct uthread {
 uint64_t uu_arg[8]; int uu_rval[2]; unsigned syscall_code,uu_workq_flags;
 uint64_t uu_workq_stackaddr,uu_workq_thport;
 union {struct _kevent_register {
  struct kevent_qos_s kev; void *handoff_thread,*kqwl;
  int eventout; uint64_t ueventlist;
 } uus_kevent_register; uint64_t alternate[32];} uu_save;
};
typedef int boolean_t;
typedef uint64_t user_addr_t;
typedef struct {uint64_t x[29],pc,lr,sp,fp;int valid;} arm_saved_state_t;
struct thread {
 struct thread *next;
 struct {arm_saved_state_t *upcb;uint64_t cthread_self;} machine;
 uint64_t wait_event; void *waitq;
 int wait_result; unsigned block_hint;
 void (*continuation)(void *,int);void *parameter;
 struct uthread *uth;
 uint64_t thread_id;
 unsigned state;
 int sched_lock;
};
typedef struct thread *thread_t;
struct task {struct {thread_t first;} threads;unsigned task_shared_region_slide;uint64_t all_image_info_addr;};
typedef struct task *task_t;
static unsigned thread_lck_grp;
static unsigned char memory[65536];
static unsigned reads,locked,scheduler_locked,irq=1,task_busy,sched_busy,recursive;
static uint64_t rejected;
static char output[32768];
static size_t output_length;
static task_t active_task;
static arm_saved_state_t *active_fault;
static void sheng_gnss_object_context(task_t,arm_saved_state_t *);
static uint64_t get_saved_state_reg(arm_saved_state_t *s,unsigned n){assert(n<29);return s->x[n];}
static uint64_t get_saved_state_pc(arm_saved_state_t *s){return s->pc;}
static uint64_t get_saved_state_lr(arm_saved_state_t *s){return s->lr;}
static uint64_t get_saved_state_sp(arm_saved_state_t *s){return s->sp;}
static uint64_t get_saved_state_fp(arm_saved_state_t *s){return s->fp;}
static int is_saved_state64(arm_saved_state_t *s){return s->valid;}
static int ml_get_interrupts_enabled(void){return irq;}
static int ml_set_interrupts_enabled(int value){int old=irq;irq=value;return old;}
static int task_lock_try(task_t t){(void)t;assert(!locked);if(task_busy)return 0;locked=1;return 1;}
static void task_unlock(task_t t){(void)t;assert(locked&&!scheduler_locked);locked=0;}
static int simple_lock_try(int *lock,unsigned *group){(void)lock;(void)group;assert(locked&&!irq&&!scheduler_locked);if(sched_busy)return 0;scheduler_locked=1;return 1;}
static void thread_unlock(thread_t t){(void)t;assert(locked&&scheduler_locked&&!irq);scheduler_locked=0;}
static void filt_wlwait_continue(void *parameter,int result){(void)parameter;(void)result;}
static struct uthread *get_bsdthread_info(thread_t t){assert(locked&&scheduler_locked&&!irq);return t->uth;}
#define queue_iterate(head,entry,type,field) for((entry)=(head)->first;(entry);(entry)=(entry)->next)
static int copyin(user_addr_t at,void *bytes,size_t size){
 assert(!locked&&!scheduler_locked&&irq);++reads;
 if(recursive){recursive=0;sheng_gnss_object_context(active_task,active_fault);}
 if(at==rejected||at<0x10000||at-0x10000>sizeof(memory)||size>sizeof(memory)-(at-0x10000))return 1;
 memcpy(bytes,memory+(at-0x10000),size);return 0;
}
static int captured_printf(const char *format,...){
 assert(!locked&&!scheduler_locked);va_list args;va_start(args,format);
 int length=vsnprintf(output+output_length,sizeof(output)-output_length,format,args);va_end(args);
 assert(length>=0&&(size_t)length<sizeof(output)-output_length);output_length+=(size_t)length;return length;
}
#define printf captured_printf
'''
cases = r'''
#undef printf
static void clear_output(void){output[0]=0;output_length=reads=0;assert(!locked&&!scheduler_locked);}
int main(void){
 arm_saved_state_t fault={.valid=1},saved={.valid=1,.pc=0x20000,.lr=0x20004,.sp=0x10000,.fp=0x12000};
 fault.x[25]=0x10200;fault.x[20]=0x11000;
 saved.x[19]=fault.x[25];saved.x[20]=fault.x[20];
 struct thread peer={.machine.upcb=&saved,.thread_id=19,.state=TH_WAIT};
 struct task task={.threads.first=&peer,.task_shared_region_slide=0x1ac4000};
 active_task=&task;active_fault=&fault;
 uint64_t pair[2]={0x12010,0xabcd000180012340ULL};memcpy(memory+0x2000,pair,sizeof(pair));
 pair[0]=0;pair[1]=0x180056780ULL;memcpy(memory+0x2010,pair,sizeof(pair));
 sheng_gnss_object_context(&task,&fault);
 assert(reads==6&&strstr(output,"peers=1 visited=1 skipped=0"));
 assert(strstr(output,"ret=180012340")&&strstr(output,"ret=180056780"));
 clear_output();irq=0;sheng_gnss_object_context(&task,&fault);assert(!reads);irq=1;
 clear_output();task_busy=1;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"task snapshot busy"));task_busy=0;
 clear_output();sched_busy=1;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"peers=0 visited=1 skipped=1"));sched_busy=0;
 clear_output();peer.state=TH_WAIT|TH_RUN;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"peers=0"));peer.state=TH_WAIT;
 clear_output();saved.sp=0x100000;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"peers=0"));saved.sp=0x10000;
 clear_output();rejected=0x12000;sheng_gnss_object_context(&task,&fault);assert(reads==4&&strstr(output,"frame=0 unavailable"));rejected=0;
 clear_output();recursive=1;sheng_gnss_object_context(&task,&fault);assert(reads==6&&strstr(output,"peer snapshot unavailable"));
 struct thread threads[70];memset(threads,0,sizeof(threads));
 for(unsigned i=0;i<70;i++){threads[i].next=i+1<70?&threads[i+1]:0;threads[i].state=TH_RUN;}
 clear_output();task.threads.first=threads;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"visited=64"));

 clear_output();task.threads.first=&peer;struct uthread uth={.syscall_code=SYS_kevent_id,.uu_workq_flags=0x21,.uu_workq_stackaddr=0x40000,.uu_workq_thport=0x501};
 peer.uth=&uth;peer.block_hint=kThreadWaitWorkloopSyncWait;peer.continuation=filt_wlwait_continue;
 peer.parameter=&uth.uu_save.uus_kevent_register;peer.wait_result=7;peer.wait_event=0xabcdef;peer.waitq=(void*)0x55660;
 peer.machine.cthread_self=0x13000;task.all_image_info_addr=0x14000;
 uth.uu_rval[0]=-1;uth.uu_rval[1]=2;for(unsigned i=0;i<8;i++)uth.uu_arg[i]=0x50000+i;
 uth.uu_save.uus_kevent_register.kev=(struct kevent_qos_s){.ident=19,.filter=-17,.flags=9,.fflags=4,.udata=0x30000};
 uth.uu_save.uus_kevent_register.kqwl=(void*)0x556600;uth.uu_save.uus_kevent_register.ueventlist=0x15000;
 saved.sp=0x10000;saved.fp=0;uint64_t dsc=0x16000;memcpy(memory+0x33c8,&dsc,8);
 sheng_gnss_object_context(&task,&fault);
 assert(strstr(output,"hint=16")&&strstr(output,"event=abcdef")&&strstr(output,"parameter="));
 assert(strstr(output,"current-dsc address=0x16000")&&strstr(output,"dyld-images address=0x14000"));
 assert(strstr(output,"pointer=16000 after=16000 error=0 repeat=0"));
 uint64_t words[32];locked=scheduler_locked=1;irq=0;sheng_gnss_waiter_snapshot(&peer,words);locked=scheduler_locked=0;irq=1;
 assert(words[0]==375&&words[13]==1&&words[14]==19&&words[24]==0x556600&&words[26]==0x15000);
 assert(words[4]==0x2ffffffffULL&&words[5]==0x50000&&words[12]==0x50007);
 for(unsigned reason=0;reason<4;reason++){
  uth.syscall_code=reason==0?0:375;peer.block_hint=reason==1?0:16;
  peer.continuation=reason==2?NULL:filt_wlwait_continue;peer.parameter=reason==3?NULL:&uth.uu_save.uus_kevent_register;
  memset(words,0xff,sizeof(words));locked=scheduler_locked=1;irq=0;sheng_gnss_waiter_snapshot(&peer,words);locked=scheduler_locked=0;irq=1;
  for(unsigned i=13;i<32;i++)assert(!words[i]);
 }
 peer.parameter=&uth.uu_save.uus_kevent_register;uth.syscall_code=375;peer.block_hint=16;
 peer.continuation=filt_wlwait_continue;
 for(unsigned state=0;state<256;state++){
  if((state&(TH_WAIT|TH_RUN|TH_TERMINATE|TH_TERMINATE2))==TH_WAIT)continue;
  peer.state=state;peer.uth=(void*)1;clear_output();sheng_gnss_object_context(&task,&fault);assert(strstr(output,"peers=0"));
 }
 peer.state=TH_WAIT;peer.uth=&uth;
 uint64_t invalid_dsc[]={0,0x16008,0x1000,0x20001,0x20010,(1ULL<<47)-64};
 for(unsigned i=0;i<sizeof(invalid_dsc)/sizeof(*invalid_dsc);i++){
  memcpy(memory+0x33c8,&invalid_dsc[i],8);clear_output();sheng_gnss_object_context(&task,&fault);
  assert(!strstr(output,"current-dsc address"));
 }
 memcpy(memory+0x33c8,&dsc,8);clear_output();rejected=0x133c8;sheng_gnss_object_context(&task,&fault);
 assert(!strstr(output,"current-dsc address")&&strstr(output,"error=1 repeat=1"));rejected=0;
 assert(!sheng_gnss_user_range(0,128)&&!sheng_gnss_user_range((1ULL<<47)-64,128));
 assert(sheng_gnss_user_range((1ULL<<47)-128,128));
 puts("PASS: actual fatal/waiter sampler;256 thread states, typed375 continuation gate, TSD context bounds, unlocked reads and64-thread bound");
}
'''
with tempfile.TemporaryDirectory(prefix='gnss-object-context-') as temp:
    work = Path(temp)
    source, binary = work / 'test.c', work / 'test'
    source.write_text(prefix + actual + helper + cases)
    command = ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               *([] if sys.platform == 'darwin' else ['-fno-pie', '-no-pie']),
               str(source), '-o', str(binary)]
    subprocess.run(command, check=True)
    execution = subprocess.check_output([str(binary)], text=True)
    print(execution, end='')
if args.out:
    result = dict(passed=True, actual_compiled_sources=bool(args.exception_source),
                  sampler_sha256=hashlib.sha256(actual.encode()).hexdigest(),
                  helper_sha256=hashlib.sha256(helper.encode()).hexdigest(),
                  thread_states=256, ASan_UBSan=True,
                  waiting_stable_only=True, typed_syscall_and_continuation_gate=True,
                  no_user_read_or_printf_under_locks=True, reentrancy_guard=True,
                  physical_runtime_verified=False, phone_operations=False,
                  output=execution)
    if args.exception_source:
        result.update(exception_source_sha256=hashlib.sha256(args.exception_source.read_bytes()).hexdigest(),
                      event_source_sha256=hashlib.sha256(args.event_source.read_bytes()).hexdigest())
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
