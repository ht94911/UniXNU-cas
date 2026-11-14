// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual single worker and wire authorization, with local pump adapters.
#define main prior_runtime_cases_not_executed
#include "runtime_control.cpp"
#undef main
static unsigned variant,mode_calls,wd_calls,enable_calls,disable_calls,exit_calls,restore_calls,feeds,cp_reads,expiry_rows;
static uint32_t cp_mode,cp_on,cp_wd,cp_fault;
static uint64_t last_cp_access,quiet_start,first_enable,expiry_access;
static unsigned reads_at_fence,feeds_at_fence;
static bool cccv_test;static unsigned pps_updates;static uint32_t simulated_vbat=4250,simulated_ibat=3000,simulated_temp=3030;
static int connector_begin(void *,unsigned,NpAdcNativeReading *){return 1;}
static int connector_poll(void *,NpAdcNativeReading *){return 1;}
static int connector_quiet(void *){return 0;}
static int cp_read(void *,NpPump *p){
 ++cp_reads;last_cp_access=test_ms;
 *p=NpPump{};p->chip=0x85;p->controls_valid=p->switching_valid=1;
 p->mode=(uint8_t)cp_mode;p->enable=p->switching=(uint8_t)cp_on;
 p->valid=23;p->bus_mv=(int32_t)test_vbus;p->battery_mv=(int32_t)simulated_vbat;p->bus_ma=300;p->die_centi_c=3000;
 p->control[2]=10;p->control[4]=0x1d;p->control[5]=19;p->control[13]=(uint8_t)cp_mode;p->control[12]=(uint8_t)cp_wd;
 p->control[10]=cp_on?0x80:0;p->control[9]=cp_on?2:0;p->control[20]=cp_fault?8:0x88;
 p->control[18]=cp_fault?0x10:0;
 (void)np_sc8585_protection_decode(&p->protection,0x85,p->control,0);
 if(quiet_start){expiry_access=test_ms;cp_fault=0;return EIO;}
 if(((variant==3&&cp_on)||variant==7)&&enable_calls){
  p->valid=0;reads_at_fence=cp_reads;feeds_at_fence=feeds;return EIO;}
 return 0;
}
static int cp_mode1(void *,NpPumpModeReading *p){
 ++mode_calls;last_cp_access=test_ms;*p=NpPumpModeReading{};
 p->valid_before=p->valid_after=0x7ff;p->owned=0x1ff;cp_mode=1;return 0;
}
static int cp_forward(void *,unsigned mode,NpPumpModeReading *p){
 assert(mode==0);++mode_calls;last_cp_access=test_ms;*p=NpPumpModeReading{};
 p->valid_before=p->valid_after=0x7ff;p->owned=0x1ff;cp_mode=0;return 0;
}
static int cp_enable_forward(void *context,unsigned mode,uint32_t mv,NpPump *p);
static int cp_watchdog(void *,unsigned timeout,uint8_t *observed,uint8_t *attempted){
 assert(timeout==5000&&!cp_on&&mode_calls==1);++wd_calls;last_cp_access=test_ms;
 cp_wd=0x2c;*observed=0x2c;*attempted=1;return 0;
}
static int cp_enable(void *,uint32_t mv,NpPump *p){
 assert(mv==17600&&test_vbus==17600&&cp_mode==0&&wd_calls==1);
 ++enable_calls;last_cp_access=first_enable=test_ms;cp_on=1;
 *p=NpPump{};p->enable=p->enable_write_attempted=1;
 p->enable_requested_raw=0x80;p->enable_immediate_raw=0x88;p->enable_settled_raw=0x88;
 p->enable_immediate_valid=p->enable_settled_valid=1;p->enable_check_phase=5;p->enable_settle_ms=300;
 return variant==2?EIO:0;
}
static int cp_enable_forward(void *context,unsigned mode,uint32_t mv,NpPump *p){
 assert(mode==0&&mv>=12600&&mv<=20000);return cp_enable(context,mv,p);
}
static int cp_disable(void *){++disable_calls;last_cp_access=test_ms;
 if(reads_at_fence)assert(feeds==feeds_at_fence&&cp_reads==reads_at_fence);
 if(variant==4)return EIO;
 cp_on=0;return 0;
}
static int cp_exit(void *){assert(!cp_on);++exit_calls;last_cp_access=test_ms;cp_wd=0x28;cp_fault=0;quiet_start=0;return 0;}
static int cp_restore(void *){assert(!cp_on&&test_vbus==5000);++restore_calls;last_cp_access=test_ms;cp_mode=0;return 0;}
static int cp_feed(void *,uint8_t *observed,uint8_t *attempted){
 assert(!quiet_start);++feeds;last_cp_access=test_ms;*observed=0x2c;*attempted=1;
 return variant==1&&enable_calls?EIO:0;
}
static int wire_send(void *,const uint8_t *p,uint32_t n){
 assert(!wire_pending&&n==sizeof(wire));
 if(np_le32(p+8)==NP_MCA_WRITE){
  uint32_t prop=np_le32(p+12),value=np_le32(p+20);
  if(cccv_test&&variant==18&&prop==NP_BUCK_PPS_PD&&cp_on){
   control.buck.cccv.valid=0;
   assert(np_power27_write_allowed(p,n)==0);
   control.buck.cccv.valid=1;++pps_updates;return -2;
  }
  assert(np_power27_write_allowed(p,n)==1);
  uint8_t altered[NP_MCA_REQUEST];memcpy(altered,p,n);altered[20]^=1;
  assert(!np_power27_write_allowed(altered,n));
  if(prop==NP_BUCK_PPS_PD){
   assert((value&65535)==20);
   const uint32_t requested=(value>>16)*20;
   if(!cp_on){assert(requested==17600);test_vbus=17600;}
   else{assert(cccv_test&&requested>=12600&&requested<=20000);++pps_updates;
    if(variant==14)return 0; /* accepted update, missing completion */
    test_vbus=requested;
   }
  }
  else if(prop==NP_BUCK_FIXED_PD){assert(value==5000&&!cp_on);test_vbus=5000;}
  else{assert(prop==NP_BUCK_FCC&&(value==100||value==500));test_fcc=value;}
  ++writes;
 }
 memcpy(wire,p,n);wire_pending=true;reply_at=test_ms+20;return 0;
}
static int wire_poll(void *){
 if(wire_pending&&test_ms>=reply_at){
  uint8_t answer[NP_MCA_RESPONSE]={0};memcpy(answer,wire,16);np_put32(answer+20,np_le32(wire+16));
  uint32_t prop=np_le32(wire+12),v=0;
  switch(prop){
   case 0x20001:v=1;break;case 0x2001:v=cp_on?7:3;break;
   case 0x20002:v=(variant==11||variant==12)&&cp_on?test_vbus*1000-(variant==11?224000u:501000u):test_vbus*1000;break;
   case 0x20004:v=cccv_test?300000:1000000;break;case NP_BUCK_ICL:v=5000;break;case NP_BUCK_TERM_VOLT:v=4400;break;
   case NP_BUCK_ENABLE:v=1;break;case 0x2000c:v=11;break;case 0x2100d:v=1;break;case 0x2100e:v=2;break;
   case 0x21014:
    np_put32(answer+24,5000);np_put32(answer+28,5000);np_put32(answer+32,3000);
    np_put32(answer+40,3600);np_put32(answer+44,11000);np_put32(answer+48,6100);np_put32(answer+52,3);
    np_put32(answer+56,3600);np_put32(answer+60,20000);np_put32(answer+64,5000);np_put32(answer+68,3);break;
   case 0x2101c:assert(0&&"Unimplemented firmware getter must not be used");break;
   default:break;
  }
  if(prop!=0x21014)np_put32(answer+24,v);
  wire_pending=false;++delivered;np_power27_receive(1,answer,sizeof(answer));
 }
 return 0;
}
static int gauge63(void *,uint8_t (*out)[2],uint32_t *mask){
 uint16_t w[10]={(uint16_t)simulated_vbat,(uint16_t)(cp_on?simulated_ibat:test_fcc),(uint16_t)simulated_temp,60,4500,5700,6800,331,80,0};
 for(unsigned i=0;i<10;++i){out[i][0]=(uint8_t)w[i];out[i][1]=(uint8_t)(w[i]>>8);}*mask=0x3ff;test_ms+=4;return 0;
}
static void fresh_policy(){
 NpConnectorThermal c={};c.enabled=1;c.generation=test_ms;
 c.begin_ms=test_ms-2;c.end_ms=test_ms;c.attempted=c.valid_mask=3;
 for(auto &r:c.reading){r.converted=r.quiescent=1;r.centi_c=3000;}
 c.sampled_ms[0]=c.sampled_ms[1]=test_ms;np_control_connector_sample(&control,&c,test_ms);
}
static void advance(unsigned){
 if(cccv_test&&control.buck.cccv_updates>=2){
  if(variant==13){simulated_vbat=4454;simulated_ibat=1000;simulated_temp=3140;}
  if(variant==15)auth.verified=0;
  if(variant==16)np_control_stop(&control);
  if(variant==17)runtime_log_capped=1;
 }
 if(cccv_test&&control.buck.cccv_updates>=4)np_control_stop(&control);

 if(control.buck.state==NP_BUCK_PUMP_QUIET&&!quiet_start)quiet_start=test_ms;
 if(quiet_start&&!expiry_access){
  assert(last_cp_access<=quiet_start);
  if(test_ms-quiet_start>=5000){cp_on=0;cp_fault=NP_PUMP_WD_TIMEOUT;}
 }
 fresh_policy();
 if(variant==4&&enable_calls&&control.buck.state==NP_BUCK_PUMP_RUN)np_control_stop(&control);
 if(variant==8&&enable_calls&&control.buck.state==NP_BUCK_PUMP_RUN){
  feeds_at_fence=feeds;reads_at_fence=cp_reads;auth.verified=0;}
 if(control.buck.state==NP_BUCK_FINISHED||control.buck.state==NP_BUCK_UNCERTAIN){system_inshutdown=1;return;}
 assert(test_ms<130000);
}
static void logging(const char *p,size_t){
 if(strstr(p,"pump_command=ff000013"))++expiry_rows;
 if(strstr(p,"pump_enable_check=1")&&enable_calls){
  assert(strstr(p,"phase=5")&&strstr(p,"requested=128")&&strstr(p,"immediate_valid=1 immediate=136")&&
    strstr(p,"settled_valid=1 settled=136 wait_ms=300"));
 }
}
#ifndef NP_BUCK_CV_CASE_MAIN
#define NP_BUCK_CV_CASE_MAIN main
#endif
int NP_BUCK_CV_CASE_MAIN(int argc,char **argv){
 assert(argc==2);variant=(unsigned)strtoul(argv[1],nullptr,10);assert(variant>=13&&variant<19);cccv_test=variant>=13;
 test_mode=100;test_icl=3000;test_type=11;NpAuthMaterial material={};
 NpPowerOps ops={};ops.now_ms=now;ops.poll_link=wire_poll;ops.send_mca=wire_send;
 ops.read_gauge=gauge63;ops.read_pump=cp_read;ops.quiesce=quiet;ops.resume=quiet;
 ops.control_mode=cccv_test?9:7;ops.stock_support100w_valid=1;ops.stock_support100w=1;ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
 ops.property_online=0x20001;ops.property_status=0x2001;ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
 ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
 ops.auth_enabled=1;ops.auth_material=&material;ops.begin_connector=connector_begin;ops.poll_connector=connector_poll;ops.quiesce_adc=connector_quiet;
 ops.set_pump_mode1=cp_mode1;ops.set_pump_watchdog=cp_watchdog;ops.feed_pump_watchdog=cp_feed;
 ops.set_pump_forward_mode=cp_forward;ops.enable_pump_forward_mode=cp_enable_forward;ops.enable_pump=cp_enable;ops.disable_pump=cp_disable;ops.prepare_pump_exit=cp_exit;ops.restore_pump_mode=cp_restore;
 assert(!np_power27_bind(&ops));persistentLog=1;np_control_link(&control,1,1);np_mca_link(&mca,1);transportEpoch=1;
 auth.enabled=auth.battery_ok=auth.battery_checked=auth.verified=1;auth.epoch=auth.attachment=1;auth.state=NP_AUTH_DONE;
 NpGaugeIdentity id={};id.valid=3;id.cell_code='F';memcpy(id.device_name,"BM6N",5);np_control_profile(&control,&id,1);gaugeIdentity=id;io.country_cn=1;
 for(auto &m:control.meta){m.valid=1;m.at_ms=test_ms;}
 control.meta[NP_META_ICL].value=5000;control.meta[NP_META_TERM].value=4400;control.meta[NP_META_ENABLED].value=1;
 control.meta[NP_META_TYPE].value=11;control.meta[NP_META_DATA_ROLE].value=1;control.meta[NP_META_PD_STATE].value=2;
 control.caps.valid=1;control.caps.epoch=1;control.caps.present_mask=7;control.caps.at_ms=test_ms;
 control.caps.entry[0]={5000,5000,3000,NP_PD_FIXED};control.caps.entry[1]={3600,11000,6100,NP_PD_APDO};control.caps.entry[2]={3600,20000,5000,NP_PD_APDO};
 fresh_policy();additional_log=logging;
 if(variant==5||variant==6){
  // An already-qualified local operation still rechecks authentication.
  begin(test_ms);cycle=outgoing=false;mca.pending=0;test_vbus=8800;cp_mode=1;
  control.sample.charger.usb_uv=8800000;control.pump.bus_mv=8800;control.pump.mode=1;
  control.buck.armed=1;control.buck.stop=0;control.buck.epoch=control.buck.attachment=1;
  control.buck.state=NP_BUCK_PUMP_START;control.buck.pump_target_mv=8800;control.buck.owned=58;control.buck.lease_until=100000;
  if(variant==5)auth.verified=0;else ++auth.attachment;
  auxiliary_begin(test_ms);assert(!enable_calls);puts("PASS runtimePump063 stale auth blocks enable");return 0;
 }
 if(variant==9||variant==10){
  additional_sleep=advance;
  while(control.buck.state!=NP_BUCK_PUMP_RUN){step();IOSleep(50);}
  draining=false;started=1;lifecycle=1;
  if(variant==9)system_inshutdown=1;else suspendRequested=1;
  additional_sleep=nullptr;
  assert(!drain_control()&&control.buck.state==NP_BUCK_FINISHED&&!control.buck.owned&&
    !cp_on&&disable_calls==1&&exit_calls==1&&restore_calls==1&&!expiry_rows);
  puts("PASS runtimePump063 normal shutdown/suspend drains running pump before5V/mode release");return 0;
 }
 additional_sleep=advance;
 for(unsigned i=0;i<2700&&!system_inshutdown;++i){step();IOSleep(50);}
 assert(system_inshutdown&&mode_calls==1&&wd_calls==1&&enable_calls==1);
 if(cccv_test){
  assert(!expiry_rows&&disable_calls==1&&!cp_on&&test_vbus==5000&&restore_calls==1&&
   control.buck.state==NP_BUCK_FINISHED&&!control.buck.owned);
  assert(pps_updates>=1);
  if(variant==13)assert(control.buck.cccv.stage==3&&control.buck.cccv_updates>=4);
  if(variant==14||variant==18)assert(pps_updates==1&&control.buck.failures);
  if(variant==17)assert(runtime_log_capped&&control.buck.cccv_updates==2);
  assert(!drain_control()&&shutdown_lines==1);
  printf("PASS actual worker CCCV%u updates%u PPS%u feed%u; oneenable, measured5V and released owners\n",variant,control.buck.cccv_updates,pps_updates,feeds);return 0;
 }
 if(variant==4)assert(control.buck.state==NP_BUCK_UNCERTAIN&&disable_calls==1&&cp_on);
 else if(variant==7)assert(control.buck.state==NP_BUCK_UNCERTAIN&&disable_calls==1&&!cp_on&&
  (control.buck.owned&24)==24&&!restore_calls&&exit_calls==1);
 else{assert(control.buck.state==NP_BUCK_FINISHED&&!control.buck.owned&&!cp_on&&test_vbus==5000&&restore_calls==1&&exit_calls==1);
  if(!variant||variant==11)assert(expiry_rows==1&&expiry_access&&control.buck.pump_shutdown_verified&&feeds>=50);
  else assert(!expiry_rows&&!control.buck.pump_shutdown_verified&&disable_calls==1);
 }
 printf("PASS runtimePump063 case%u feeds%u CPreads%u enable%u disable%u state%u\n",variant,feeds,cp_reads,enable_calls,disable_calls,control.buck.state);
 return 0;
}
