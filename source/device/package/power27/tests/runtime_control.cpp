// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the real runtime/broker against a deterministic mock wire endpoint.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "../platform/NezhaPowerRuntime.cpp"
static uint64_t test_ms=1000,reply_at;
static uint8_t wire[NP_MCA_REQUEST];
static bool wire_pending,linked;
static unsigned writes,log_lines,delivered,pump_reads,test_mode,usb_lines,aux_lines,shutdown_lines;
static bool log_stopped,usb_failure_logged;
extern "C" {uint32_t system_inshutdown;}
static bool fault_used;
static unsigned log_status_calls;
static uint32_t test_icl=500,test_fcc=500;
void IOSleep(unsigned n){test_ms+=n;if((test_mode==8&&test_ms>7000)||(test_mode==15&&test_ms>40000))system_inshutdown=1;}
extern "C" IOService *np_battery_service_create(void){static IOService s;return &s;}
extern "C" int np_power_log_start(void){return 0;}
extern "C" int np_power_log_push(const char *p,size_t n){assert(n<1024&&n>6&&
 (!memcmp(p,"P1 ",3)||!memcmp(p,"CPREG ",6)||!memcmp(p,"U1 ",3)||!memcmp(p,"A1 ",3)||!memcmp(p,"S1 ",3)));
 if(!memcmp(p,"U1 ",3)){++usb_lines;if(strstr(p,"phase=6")&&strstr(p,"failed=-4"))usb_failure_logged=true;}
 if(!memcmp(p,"A1 ",3))++aux_lines;
 if(!memcmp(p,"S1 ",3))++shutdown_lines;
 if(test_mode>=10&&(!memcmp(p,"P1 ",3)||!memcmp(p,"S1 ",3)||
    (!memcmp(p,"A1 ",3)&&strstr(p,"control=1")))) {
  assert(strstr(p,"backend=1")&&strstr(p,"confirmation="));
 }
 ++log_lines;return 0;}
extern "C" void np_power_log_stop(void){log_stopped=true;}
extern "C" int np_power_log_status(NpPowerLogStatus *s){if(test_mode==7&&++log_status_calls%5==0)return 0;memset(s,0,sizeof(*s));s->phase=log_stopped?4:(test_mode==3||test_mode==13)?3:2;return 1;}
extern "C" void qcom_usb_debug_get_status(qcom_usb_debug_status *s){memset(s,0,sizeof(*s));s->phase=QCOM_USB_DEBUG_ACTIVE;
 s->connected=s->configured=s->dtr=1;s->rx_generation=1;s->poll_calls=(uint32_t)test_ms;
 if(test_mode==9&&test_ms>4000){s->phase=QCOM_USB_DEBUG_IO_FAILED;s->error=QCOM_USB_DEBUG_CORE_POLL_FAILED;
  s->core_result=s->core_failed=-4;s->rx_generation=2;s->connected=s->configured=s->dtr=0;}}
extern "C" void np_power_diagnostic_start(void){}
static uint64_t now(void *){return test_ms;}
static uint32_t property(uint32_t p){switch(p){case 0x20001:return 1;case 0x2001:return 3;case 0x20002:return 5000000;case 0x20004:return 490000;
 case 0x2008:return test_icl;case 0x2005:return test_mode==9||test_mode>=10?0:test_fcc;case 0x2007:return 4480;case 0x2003:return 1;case 0x2000c:return 3;default:return 0;}}
static int send(void *,const uint8_t *p,uint32_t n){assert(!wire_pending&&n==sizeof(wire));
 if(np_le32(p+8)==NP_MCA_WRITE){int allowed=np_power27_write_allowed(p,n);if(allowed<=0)return allowed<0?-1:-2;uint32_t prop=np_le32(p+12),val=np_le32(p+20);
  assert(prop==NP_BUCK_ICL||prop==NP_BUCK_FCC);assert(val>=100&&val<=1500);
  if(prop==NP_BUCK_ICL)test_icl=val;else test_fcc=val;++writes;
  if(test_mode==1&&!fault_used){fault_used=true;return 0;}
  if(test_mode==11&&prop==NP_BUCK_FCC&&val==1000&&!fault_used){fault_used=true;return 0;}
 }
 memcpy(wire,p,n);wire_pending=true;reply_at=test_ms+30;return 0;}
static int poll(void *){if(!linked){linked=true;np_power27_link(1,1);}
 if(wire_pending&&test_ms>=reply_at){
  if(test_mode==2&&!fault_used&&np_le32(wire+8)==NP_MCA_WRITE){
   fault_used=true;uint8_t note[NP_MCA_NOTIFICATION]={0};np_put32(note,NP_MCA_OWNER);np_put32(note+4,2);
   np_power27_receive(1,note,sizeof(note));test_icl=100;
  }
  uint8_t answer[NP_MCA_RESPONSE]={0};memcpy(answer,wire,16);np_put32(answer+20,np_le32(wire+16));
  np_put32(answer+24,property(np_le32(wire+12)));
  if(test_mode==5&&np_le32(wire+12)==NP_BUCK_FCC)np_put32(answer+16,1);
  wire_pending=false;++delivered;np_power27_receive(1,answer,sizeof(answer));}
 return 0;}
static int gauge_read(void *,uint8_t (*out)[2],uint32_t *mask){uint16_t words[10]={4250,0xff9c,3032,79,4500,5729,6800,327,90,0};
 if(test_mode>=10)words[1]=(uint16_t)(test_mode==12?500:test_fcc);
 for(unsigned i=0;i<10;++i){out[i][0]=(uint8_t)words[i];out[i][1]=(uint8_t)(words[i]>>8);}*mask=0x3ff;test_ms+=4;return 0;}
static int pump_read(void *,NpPump *p){++pump_reads;memset(p,0,sizeof(*p));p->enable=p->switching=test_mode==4||test_mode==14;p->chip=0x85;p->controls_valid=p->switching_valid=1;return 0;}
static int quiet(void *){return 0;}
int main(int argc,char **argv){if(argc>1)test_mode=(unsigned)strtoul(argv[1],nullptr,10);if(test_mode>=10)test_icl=3000;NpPowerOps ops={};ops.now_ms=now;ops.poll_link=poll;ops.send_mca=send;ops.read_gauge=gauge_read;ops.read_pump=pump_read;
 ops.quiesce=quiet;ops.resume=quiet;ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
 ops.property_online=0x20001;ops.property_status=0x2001;ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
 ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;ops.control_mode=test_mode==6?1:test_mode>=10?3:2;
 assert(!np_power27_bind(&ops));persistentLog=1;
 if(test_mode==15){worker(nullptr,0);assert(system_inshutdown&&workerExited&&lifecycle==5&&log_stopped&&shutdown_lines==1);
  assert(control.buck.state==NP_BUCK_FINISHED&&!control.buck.owned&&writes==3&&test_icl==3000&&test_fcc==500);
  puts("PASS FCC setter normal XNU shutdown: one reduction ACK, no getter/readback fabrication");return 0;}
 if(test_mode==8){worker(nullptr,0);assert(system_inshutdown&&workerExited&&lifecycle==5&&log_stopped&&shutdown_lines==1);
  assert(!control.buck.owned&&test_icl==500&&test_fcc==500);puts("PASS normal XNU shutdown drains control and closes its existing Data logger");return 0;}
 for(unsigned i=0;i<1500;++i){step();test_ms+=20;
  if((!test_mode||test_mode==7||test_mode==10)&&control.buck.state==NP_BUCK_RUNNING)break;
  if(test_mode==1&&control.buck.state==NP_BUCK_FINISHED)break;
  if(test_mode==2&&control.buck.state==NP_BUCK_UNCERTAIN)break;
  if((test_mode==11||test_mode==12)&&control.buck.state==NP_BUCK_FINISHED)break;
 }
 if(test_mode>=10){
  assert(!control.meta[NP_META_FCC].valid&&test_icl==3000);
  if(test_mode==10){assert(control.buck.state==NP_BUCK_RUNNING&&writes==2&&test_fcc==1000&&control.buck.confirmation==NP_BUCK_CONFIRM_CURRENT);
   assert(control.buck.feedback_baseline_ma==500&&control.buck.feedback_target_ma==1000);
   drain_control();assert(control.buck.state==NP_BUCK_FINISHED&&!control.buck.owned&&writes==3&&test_fcc==500);}
  if(test_mode==11||test_mode==12)assert(control.buck.state==NP_BUCK_FINISHED&&writes==3&&test_fcc==100&&control.buck.failures);
  if(test_mode==13||test_mode==14)assert(!writes&&test_fcc==500);
  printf("PASS real runtime FCC setter mode%u: actual ACK/feedback, no unsupported getter or ICL write\n",test_mode);return 0;
 }
 if(test_mode&&test_mode!=7){
  if(test_mode==1)assert(writes==2&&test_icl==500&&test_fcc==500&&!control.buck.owned&&control.buck.state==NP_BUCK_FINISHED);
  if(test_mode==2)assert(writes==1&&test_icl==100&&control.buck.state==NP_BUCK_UNCERTAIN);
  if(test_mode>=3)assert(!writes&&test_icl==500&&test_fcc==500);
  if(test_mode==6)assert(!pump_reads&&!log_lines&&!control.queries);
  if(test_mode==9)assert(usb_lines==2&&usb_failure_logged&&aux_lines&&aux_lines<delivered&&log_lines>usb_lines+aux_lines);
  printf("PASS runtime fault mode %u: no replay or unqualified increase\n",test_mode);return 0;
 }
 assert(control.buck.state==NP_BUCK_RUNNING&&writes==2&&test_icl==1500&&test_fcc==1500);
 assert(log_lines&&delivered>10&&control.buck.owned==3);
 assert(usb_lines==1); /* Changing poll counters do not flood the Data log. */
 drain_control();assert(control.buck.state==NP_BUCK_FINISHED&&control.buck.owned==0&&writes==4&&test_icl==500&&test_fcc==500);
 NpPowerHistoryRecord rows[8];unsigned count;uint64_t next,oldest;
 assert(np_power_history_copy(0,8,rows,&count,&next,&oldest)==1&&count&&rows[0].id==oldest);
 puts("PASS real runtime: one MCA request in flight, readback-qualified limits, two restores, bounded history/log enqueue");}
