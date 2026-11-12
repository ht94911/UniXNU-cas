// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute only the new connector-probe caller paths with the existing wire peer.
#define main previous_control_main
#include "runtime_control.cpp"
#undef main

static unsigned adc_case,adc_quiet_calls,adc_boot_lines,adc_quiet_lines;
static bool adc_owner=true,adc_suspend_requested;
static void adc_log(const char *line,size_t) {
 if(!strstr(line,"adc_boot=1")&&!strstr(line,"adc_quiesce=1"))return;
 assert(!log_stopped&&strstr(line,"gen=0 epoch=0 valid_raw=0"));
 if(strstr(line,"adc_boot=1")) {
  ++adc_boot_lines;
  if(!adc_case)assert(strstr(line,"adc_attempted=3")&&strstr(line,"adc0_valid=1")&&
      strstr(line,"adc0_raw=32000")&&strstr(line,"adc1_valid=1")&&strstr(line,"adc1_raw=32100"));
  else assert(strstr(line,"adc_attempted=1")&&strstr(line,"adc0_valid=0")&&strstr(line,"adc1_valid=0"));
 } else {
  ++adc_quiet_lines;
  assert(strstr(line,"adc_cleanup_error="));
 }
}
static int adc_quiet(void *) {
 ++adc_quiet_calls;
 assert(adc_owner&&quiet_calls==0&&!log_stopped);
 if(adc_case)return EIO;
 adc_owner=false;return 0;
}
static void adc_sleep(unsigned) {
 if(test_ms<3500||!control.sample.generation||adc_suspend_requested)return;
 if(adc_case) {
  assert(np_power_lifecycle_request(0)==1);adc_suspend_requested=true;
 } else system_inshutdown=1;
}
int main(int argc,char **argv) {
 assert(argc==2);adc_case=(unsigned)strtoul(argv[1],nullptr,10);assert(adc_case<2);
 test_mode=100;NpAdcNativeBootProbe probe={};probe.attempted=adc_case?1:3;
 if(adc_case){probe.reading[0].error=EIO;probe.stop_error=ETIMEDOUT;}
 else {
  probe.reading[0]={32000,0,30720,3000,1,1};
  probe.reading[1]={32100,0,30822,3010,1,1};
 }
 NpPowerOps ops={};ops.now_ms=now;ops.poll_link=poll;ops.send_mca=send;
 ops.read_gauge=gauge_read;ops.read_pump=pump_read;ops.quiesce=quiet;ops.resume=reactivate;
 ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;ops.property_online=0x20001;
 ops.property_status=0x2001;ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
 ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
 ops.control_mode=5;ops.country_cn=1;ops.adc_boot_probe=&probe;ops.quiesce_adc=adc_quiet;
 assert(np_power27_bind(&ops)==0);started=1;persistentLog=1;
 additional_sleep=adc_sleep;additional_log=adc_log;
 worker(nullptr,0);
 assert(workerExited&&log_stopped&&!control.sample.valid&&!writes);
 assert(adc_boot_lines==1&&adc_quiet_lines==1&&adc_quiet_calls==1);
 // ADC mappings remain owned on failure. The unchanged runtime may still
 // quiesce the independent idle board; PM must retain the ADC failure.
 if(adc_case)assert(lifecycle==-1&&adc_owner&&quiet_calls==1&&adc_suspend_requested);
 else assert(lifecycle==5&&!adc_owner&&quiet_calls==1&&system_inshutdown);
 printf("PASS new ADC Runtime caller%u: startup raw record, cleanup before logger/board release, retained failure without cleanup replay; no charging baseline rerun\n",adc_case);
 return 0;
}
