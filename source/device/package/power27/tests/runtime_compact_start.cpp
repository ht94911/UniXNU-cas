// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the real startup function with compact and ordinary boot arguments.
#define NP_HOST_BOOT_ARGUMENT 1
#define NP_HOST_THREAD_START 1
#define main prior_runtime_cases_not_executed
#include "runtime_control.cpp"
#undef main

static int boot_mode=10,thread_result;
static unsigned thread_creations;
static void (*pending_worker)(void *,wait_result_t);
static void *pending_context;
int PE_parse_boot_argn(const char *key,void *out,int size)
{
 if(!strcmp(key,"nezha-power")){assert(size==(int)sizeof(int));*(int *)out=boot_mode;return 1;}
 if(!strcmp(key,"nezha-power-log")){assert(size==(int)sizeof(uint32_t));*(uint32_t *)out=1;return 1;}
 return 0;
}
int kernel_thread_start(void (*entry)(void *,wait_result_t),void *context,thread_t *out)
{
 ++thread_creations;pending_worker=entry;pending_context=context;*out=nullptr;
 return thread_result;
}
static void reset_start(void)
{
 started=stopRequested=workerExited=system_inshutdown=0;lifecycle=0;
 pending_worker=nullptr;thread_result=0;
}
int main(void)
{
 bound=1;io.control_mode=9;io.ffc_enabled=1;
 snapshotLock=IOLockAlloc();controlLock=IOLockAlloc();
 reset_start();boot_mode=10;nezha_power27_start();
 assert(started==1&&thread_creations==1&&pending_worker==worker);
 nezha_power27_start();assert(thread_creations==1);
 // Execute the callback recorded by the thread API. The worker must publish
 // diagnostics even when a stop was requested immediately after creation.
 stopRequested=1;NpPowerOps saved=io;
 io.now_ms=now;io.quiesce=quiet;io.control_mode=1;io.ffc_enabled=0;
 pending_worker(pending_context,0);assert(workerExited==1&&lifecycle==5);
 io=saved;
 reset_start();io.ffc_enabled=0;boot_mode=10;nezha_power27_start();
 assert(!started&&thread_creations==1);
 reset_start();io.ffc_enabled=1;io.control_mode=8;nezha_power27_start();
 assert(!started&&thread_creations==1);
 reset_start();bound=0;io.control_mode=9;nezha_power27_start();
 assert(!started&&thread_creations==1);
 reset_start();bound=1;boot_mode=9;io.ffc_enabled=0;nezha_power27_start();
 assert(started&&thread_creations==2);
 reset_start();boot_mode=0;nezha_power27_start();assert(!started&&thread_creations==2);
 reset_start();boot_mode=11;nezha_power27_start();assert(!started&&thread_creations==2);
 reset_start();boot_mode=11;io.ffc_enabled=1;io.stock_current_enabled=1;nezha_power27_start();
 assert(started&&thread_creations==3&&pending_worker==worker);
 nezha_power27_start();assert(thread_creations==3);io.stock_current_enabled=0;
 reset_start();boot_mode=10;io.ffc_enabled=1;thread_result=-1;nezha_power27_start();
 assert(started&&thread_creations==4&&workerExited&&lifecycle==-1);
 puts("PASS real Runtime compact10 start: one worker, normal mode retained, invalid bindings and thread failure handled");
 return 0;
}
