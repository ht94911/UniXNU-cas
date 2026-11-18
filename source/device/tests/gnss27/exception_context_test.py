#!/usr/bin/env python3
"""Exercise the actual locationd object sampler with saved thread-state fixtures."""
from pathlib import Path
import sys
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
added = ''.join(line[1:] for line in
                (ROOT / 'patches/xnu-12377/nezha-gnss-pac-capture.patch').read_text().splitlines(True)
                if line.startswith('+') and not line.startswith('+++'))
start = added.index('struct sheng_gnss_object_peer {')
end = added.index('#endif /* __arm64__ */', start)
actual = added[start:end]
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
typedef int boolean_t;
typedef uint64_t user_addr_t;
typedef struct {uint64_t x[29],pc,lr,sp,fp;int valid;} arm_saved_state_t;
struct thread {
 struct thread *next;
 struct {arm_saved_state_t *upcb;} machine;
 uint64_t thread_id;
 unsigned state;
 int sched_lock;
};
typedef struct thread *thread_t;
struct task {struct {thread_t first;} threads;unsigned task_shared_region_slide;};
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
 assert(reads==4&&strstr(output,"peers=1 visited=1 skipped=0"));
 assert(strstr(output,"ret=180012340")&&strstr(output,"ret=180056780"));
 clear_output();irq=0;sheng_gnss_object_context(&task,&fault);assert(!reads);irq=1;
 clear_output();task_busy=1;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"task snapshot busy"));task_busy=0;
 clear_output();sched_busy=1;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"peers=0 visited=1 skipped=1"));sched_busy=0;
 clear_output();peer.state=TH_WAIT|TH_RUN;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"peers=0"));peer.state=TH_WAIT;
 clear_output();saved.sp=0x100000;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"peers=0"));saved.sp=0x10000;
 clear_output();rejected=0x12000;sheng_gnss_object_context(&task,&fault);assert(reads==3&&strstr(output,"frame=0 unavailable"));rejected=0;
 clear_output();recursive=1;sheng_gnss_object_context(&task,&fault);assert(reads==4&&strstr(output,"peer snapshot unavailable"));
 struct thread threads[70];memset(threads,0,sizeof(threads));
 for(unsigned i=0;i<70;i++){threads[i].next=i+1<70?&threads[i+1]:0;threads[i].state=TH_RUN;}
 clear_output();task.threads.first=threads;sheng_gnss_object_context(&task,&fault);assert(reads==2&&strstr(output,"visited=64"));
 assert(!sheng_gnss_user_range(0,128)&&!sheng_gnss_user_range((1ULL<<47)-64,128));
 assert(sheng_gnss_user_range((1ULL<<47)-128,128));
 puts("PASS: actual object sampler; unlocked reads, waiting-state selection, busy/reentrant paths, frame failure and64-thread bound");
}
'''
with tempfile.TemporaryDirectory(prefix='gnss-object-context-') as temp:
    work = Path(temp)
    source, binary = work / 'test.c', work / 'test'
    source.write_text(prefix + actual + cases)
    command = ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               *([] if sys.platform == 'darwin' else ['-fno-pie', '-no-pie']),
               str(source), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
