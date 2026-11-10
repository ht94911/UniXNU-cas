// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the public boot-argument entry and real worker dispatch boundary.
// Existing runtime peer/stubs supply dependencies; no worker or hardware runs.
#include <cstring>
#include "host_stubs/runtime/pexpert/pexpert.h"
#include "host_stubs/runtime/kern/thread.h"
static int boot_mode,thread_result;
static unsigned dispatches;
static void (*dispatched_worker)(void *,wait_result_t);
static int actual_boot_arg(const char *key,void *out,int bytes){
 if(std::strcmp(key,"nezha-power")||bytes!=(int)sizeof(boot_mode))return 0;
 std::memcpy(out,&boot_mode,sizeof(boot_mode));return 1;
}
static int actual_thread_dispatch(void (*entry)(void *,wait_result_t),void *,thread_t *out){
 ++dispatches;dispatched_worker=entry;*out=nullptr;return thread_result;
}
#define PE_parse_boot_argn actual_boot_arg
#define kernel_thread_start actual_thread_dispatch
#define main existing_runtime_auth_fixture_main
#include "runtime_auth.cpp"
#undef main
#undef PE_parse_boot_argn
#undef kernel_thread_start
int main(int argc,char **argv){
 assert(argc>=2);boot_mode=std::atoi(argv[1]);
 const bool unbound=argc>2&&!std::strcmp(argv[2],"unbound");
 thread_result=argc>2&&!std::strcmp(argv[2],"thread-fail")?5:KERN_SUCCESS;
 if(!unbound){
  NpPowerOps ops={};ops.now_ms=now;ops.poll_link=poll;ops.send_mca=send;ops.read_gauge=gauge_read;
  ops.quiesce=quiet;ops.resume=quiet;ops.set_pump_usb_ovp=ovp;ops.control_mode=(unsigned)boot_mode;
  assert(!np_power27_bind(&ops));
 }
 nezha_power27_start();
 const bool supported=boot_mode>=1&&boot_mode<=5&&!unbound;
 if(dispatches!=(unsigned)supported){
  std::printf("FAIL public power start:boot_mode=%d bound=%d worker_dispatches=%u expected=%u\n",
   boot_mode,!unbound,dispatches,(unsigned)supported);return 2;
 }
 if(supported){
  assert(dispatched_worker==worker);nezha_power27_start();assert(dispatches==1);
  if(thread_result!=KERN_SUCCESS)assert(nezha_power27_lifecycle()==-1&&nezha_power27_worker_exited());
 }
 std::printf("PASS public power start:boot_mode=%d bound=%d dispatches=%u thread_result=%d; worker/hardware not executed\n",
  boot_mode,!unbound,dispatches,thread_result);
}
