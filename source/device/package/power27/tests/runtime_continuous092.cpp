// SPDX-License-Identifier: BSD-2-Clause-Patent
// One real Runtime worker, MCA broker and normal-policy continuation. The
// simulated wire/hardware supplies only original OS4 replies and callbacks.
#define NP_HOST_BOOT_ARGUMENT 1
#define NP_HOST_THREAD_START 1
#define main retained_runtime_main
#include "runtime_control.cpp"
#undef main
#include <openssl/hmac.h>
#include <openssl/evp.h>
static unsigned scenario,mode_calls,enables,disables,adc_exits,restores,feeds,pauses,resumes;
static bool delayed_wire(){return scenario>=18;}
static unsigned pps_requests,current_updates,normal_fcc_writes,ffc_on_calls,ffc_off_calls;
static uint32_t apdo_ma=5000,requested_ma,term_mv=4470;
static uint32_t normal_peak_fcc;
static uint8_t cp_mode,cp_on,charge_on=1;
static bool input_paused,ffc_on,thermal_low,normal_seen;
static bool finish_queued,failed_pps,thermal_returned,full_observed,recharge_ready;
static bool source_connected=true,source_detached=false,source_reconnected=false;
static bool quick_sdp(){return scenario==54;}
static bool async_scene(){return scenario>=60&&scenario<=62;}
static bool scene_test(){return scenario>=55&&scenario<=57;}
static int scene_screen(void *,uint32_t *out){
 if(scenario==57||scenario==62)return -1;
 *out=((scenario==56||scenario==61)&&test_ms>=300000)?1u:0u;return 0;
}
static bool original_type12(){return scenario==32;}
static bool retained_source(){return (scenario>=45&&scenario<=49)||scenario==53;}
static bool ordinary_source(){return scenario==23||scenario==24||scenario==29||scenario==30||scenario==34||scenario==36||(scenario>=37&&scenario<=42)||scenario==44||
 (retained_source()&&!source_reconnected)||scenario==47||(scenario>=50&&scenario<=52)||
 (quick_sdp()&&source_reconnected);}
static bool physical_detach(){return scenario==31||scenario==33||scenario==35||scenario==36||retained_source()||quick_sdp();}
static unsigned ordinary_peak_fcc(){return scenario==23||scenario==29||scenario==36||scenario==51?2000u:scenario==34||scenario==44?900u:scenario==37||scenario==39?500u:scenario==47?3000u:scenario==50||scenario==52?1000u:800u;}
static unsigned ordinary_icl(){return scenario==23||scenario==29||scenario==36||scenario==47||scenario==51?1600u:scenario==34||scenario==44?900u:scenario==50||scenario==52?1000u:500u;}
static NpAuthMaterial test_material;
static unsigned crypto_sequence,process_flag,verified_flag,vdm_state,seed_index,actual_data_role=1;
static uint8_t response_bytes[16];
static int test_hmac(void *,const uint8_t *key,size_t nk,const uint8_t *data,size_t nd,uint8_t out[32]){
 unsigned n=0;assert(HMAC(EVP_sha256(),key,(int)nk,data,nd,out,&n)&&n==32);return 0;
}
static int test_random(void *,uint8_t *out,size_t n){for(size_t i=0;i<n;++i)out[i]=(uint8_t)++crypto_sequence;return 0;}
static int test_digest(void *,const uint8_t nonce[32],uint8_t out[32]){test_ms+=400;return test_hmac(nullptr,test_material.battery_key,32,nonce,32,out);}
static void reverse_words(uint8_t out[16],const uint8_t in[16]){for(unsigned i=0;i<16;i+=4)for(unsigned j=0;j<4;++j)out[i+j]=in[i+3-j];}
static unsigned auth_opcode(uint32_t p){switch(p){case 0x21003:return 1;case 0x21004:return 2;case 0x21005:return 3;case 0x21006:return 4;case 0x21007:return 5;case 0x21009:return 7;case 0x21008:return 6;default:return 0;}}
static uint64_t pps_ack_ms;
static uint64_t catalog_refresh_after;
static unsigned catalog_reads;
static uint64_t last_run_gauge_ms,last_run_PPS_ms,last_run_PPS_generation;
static uint64_t max_run_gauge_gap_ms,max_run_PPS_gap_ms;
static uint64_t max_steady_first_cohort_gap_ms;
static uint64_t ack_sample_generation;
static bool awaiting_first_ack_cohort;
static unsigned first_ack_cohorts;
static unsigned regular_run_samples,confirmed_run_requests;
static unsigned source_current_grants,pump_write_attempts;
static uint64_t initial_enable_ms;
static bool foreign_clamp;
static void record(const char *p,size_t){
 if(!memcmp(p,"P1 ",3)&&io.stock_current_enabled)assert(strstr(p,"stock_current=1")&&strstr(p,"bus_ocp=4950"));
}
static void (*created_worker)(void *,wait_result_t);static void *created_context;static unsigned threads;
static int read_cp(void *,NpPump *p){
 *p=NpPump{};p->chip=0x85;p->mode=cp_mode;p->enable=p->switching=cp_on;
 p->controls_valid=p->switching_valid=1;p->valid=23;p->bus_mv=(int32_t)test_vbus;
 p->battery_mv=3600;p->bus_ma=cp_on?650:0;p->die_centi_c=3500;
 p->protection.valid=1;p->protection.bus_ocp_ma=4950;p->protection.bus_ocp_enabled=1;
 p->control[12]=4;if(delayed_wire())test_ms+=12;return 0;
}
static int forward(void *,unsigned mode,NpPumpModeReading *r){
 assert(!cp_on&&input_paused&&mode<=1);++mode_calls;cp_mode=(uint8_t)mode;
 *r=NpPumpModeReading{};r->owned=0x1ff;return 0;
}
static int mode1(void *c,NpPumpModeReading *r){return forward(c,1,r);}
static int watchdog(void *,unsigned ms,uint8_t *read,uint8_t *attempt){
 assert(!cp_on&&ms==5000);*read=4;*attempt=1;return 0;
}
static int feed(void *,uint8_t *read,uint8_t *attempt){++feeds;*read=4;*attempt=1;return 0;}
static int enable_forward(void *,unsigned mode,uint32_t mv,NpPump *r){
 assert(input_paused&&control.buck.input_suspend_confirmed&&mv==test_vbus&&mode==cp_mode);
 assert(control.buck.voltage_samples>=2&&auth.verified&&auth.battery_ok);
 ++enables;if(!initial_enable_ms)initial_enable_ms=test_ms;
 if(delayed_wire())test_ms+=300;
 cp_on=1;read_cp(nullptr,r);r->enable_write_attempted=1;return 0;
}
static int enable2(void *c,uint32_t mv,NpPump *r){return enable_forward(c,1,mv,r);}
static int disable(void *){++disables;cp_on=0;return 0;}
static int adc_exit(void *){assert(!cp_on);++adc_exits;return 0;}
static int restore(void *){assert(!cp_on&&(source_connected?test_vbus==5000:test_vbus==0));++restores;cp_mode=0;return 0;}
static bool scene_adc_pending;
static unsigned scene_adc_channel,scene_samples,scene_low_samples,scene_recovered_samples;
static uint64_t scene_adc_ready,scene_previous_begin,scene_max_period;
static uint64_t scene_seen_generation;
static int32_t scene_temperature(){
 return test_ms<120000?39500:test_ms<250000?42500:41900;
}
static int32_t scene_input(){return ((scene_temperature()-1307)*1000+964)/965;}
static int connector_start(void *,unsigned channel,NpAdcNativeReading *){
 if(async_scene()){
  assert(!scene_adc_pending&&channel<8);scene_adc_pending=true;
  scene_adc_channel=channel;scene_adc_ready=test_ms+100;
 }
 return 1;
}
static int connector_poll(void *,NpAdcNativeReading *r){
 if(!async_scene()||test_ms<scene_adc_ready)return 1;
 assert(scene_adc_pending);scene_adc_pending=false;*r=NpAdcNativeReading{};
 r->raw=(uint16_t)(25000+scene_adc_channel);r->c1024=scene_input();
 r->centi_c=(int32_t)((int64_t)r->c1024*100/1024);
 r->converted=r->quiescent=1;return 0;
}
static int connector_quiet(void *){return 0;}
static int keep_connected_quiet(void *){++quiet_calls;assert(!np_buck_all_owned(&control.buck));return 0;}
static int restore_connected(void *){++resume_calls;np_power27_link(1,transportEpoch);return 0;}
static int original_temperature(void *,NpBoardTemperatureReading *r){
 *r=NpBoardTemperatureReading{};r->valid=1;r->milli_c=async_scene()?scene_input():36000;
 r->begin_ms=r->end_ms=test_ms;return 0;
}
static int identity(void *,NpGaugeIdentity *id){
 *id=NpGaugeIdentity{};id->valid=3;id->cell_code='F';memcpy(id->device_name,"BM6N",5);return 0;
}
static int fast(void *,unsigned enabled,unsigned auth_pending,NpFastModeReading *r){
 assert(!cycle&&!auxiliaryActive&&!mca.pending&&!auth_pending);
 if(enabled){assert(cp_on&&enables>=1);++ffc_on_calls;}else ++ffc_off_calls;
 if(delayed_wire()){test_ms+=400;}
 ffc_on=enabled;*r=NpFastModeReading{};r->valid=1;r->enabled=r->owned=enabled;return 0;
}
static int current_gauge(void *,uint8_t (*out)[2],uint32_t *mask){
 if((scenario==12||delayed_wire())&&awaiting_first_ack_cohort){
  assert(control.buck.state==NP_BUCK_PUMP_OBSERVE_REGULATION);
  assert(!control.buck.voltage_samples&&control.caps.at_ms>pps_ack_ms);
  const uint64_t gap=test_ms-pps_ack_ms;
  if(first_ack_cohorts&&gap>max_steady_first_cohort_gap_ms)max_steady_first_cohort_gap_ms=gap;
  ++first_ack_cohorts;awaiting_first_ack_cohort=false;
 }
 if((scenario==12||delayed_wire())&&cp_on&&ffc_on&&
    control.buck.cccv_updates>=1){
  if(last_run_gauge_ms){
   const uint64_t gap=test_ms-last_run_gauge_ms;
   if(gap>max_run_gauge_gap_ms)max_run_gauge_gap_ms=gap;
  }
  last_run_gauge_ms=test_ms;++regular_run_samples;
 }
 const bool final_charge=scenario==21&&test_ms>=450000&&!recharge_ready;
 const uint16_t soc=final_charge?99:24;
 const int32_t current=cp_on?(final_charge?1000:thermal_low?700:delayed_wire()?1301:2000):input_paused?-300:source_connected&&charge_on?(int32_t)test_fcc-100:-100;
 uint16_t words[10]={(uint16_t)(final_charge?4524:3600),(uint16_t)current,(uint16_t)(scenario==37||scenario==39?3240:3090),soc,(uint16_t)(soc*57),5700,6800,333,80,0};
 for(unsigned i=0;i<10;++i){out[i][0]=(uint8_t)words[i];out[i][1]=(uint8_t)(words[i]>>8);}
 *mask=0x3ff;test_ms+=delayed_wire()?8:4;return 0;
}
static int current_send(void *,const uint8_t *p,uint32_t n){
 assert(!wire_pending&&n==sizeof(wire));
 const uint32_t prop=np_le32(p+12),value=np_le32(p+20);
 if(np_le32(p+8)==NP_MCA_WRITE){
  int allowed=np_power27_write_allowed(p,n);if(allowed<=0)return allowed<0?-1:-2;
  if(authAuxiliary){
   if(prop==0x21002)process_flag=value;
   else if(prop==0x21013)verified_flag=value;
   else if(prop==0x2100d){assert(value==2);actual_data_role=value;}
   else if(prop==0x2100b)vdm_state=254;
   else{
    vdm_state=auth_opcode(prop);assert(vdm_state);
    if(vdm_state==4){
     uint8_t natural[16];reverse_words(natural,p+20);seed_index=10;
     for(unsigned i=0;i<10;++i)if(!memcmp(natural,test_material.seeds[i],16))seed_index=i;
     assert(seed_index<10);
    }
    if(vdm_state==5){
     uint8_t input[20],mac[32];reverse_words(input,p+20);
     input[16]=0xa1;input[17]=0xb2;input[18]=0xc3;input[19]=0xd4;
     test_hmac(nullptr,test_material.adapter_keys[seed_index],32,input,20,mac);reverse_words(response_bytes,mac);
    }
   }
  }else if(prop==NP_BUCK_PPS_PD){
   ++pump_write_attempts;
   const uint32_t mv=(value>>16)*20,ma=(value&65535)*50;
   assert((uint64_t)mv*ma<=90000000&&ma<=apdo_ma&&ma<=4950);
   if(cp_on){++current_updates;assert(ffc_on&&ma>=100);}
   else assert(ma<=2000);
   if((scenario==12||delayed_wire())&&cp_on){
    if(last_run_PPS_ms){
     const uint64_t gap=test_ms-last_run_PPS_ms;
     if(gap>max_run_PPS_gap_ms)max_run_PPS_gap_ms=gap;
     assert(control.sample.generation>=last_run_PPS_generation+2);
     assert(control.sample.generation>=ack_sample_generation+2);
    }
    last_run_PPS_ms=test_ms;last_run_PPS_generation=control.sample.generation;
    ++confirmed_run_requests;
   }
   ++pps_requests;requested_ma=ma;test_vbus=mv;
  }else if(prop==NP_BUCK_FIXED_PD){assert(value==5000&&!cp_on);
   assert(!(quick_sdp()&&source_reconnected));test_vbus=value;}
  else if(prop==NP_BUCK_INPUT_SUSPEND){
   for(unsigned i=21;i<n;++i)assert(!p[i]);
   if(value){++pauses;input_paused=true;}
   else{assert(!cp_on&&(source_connected?test_vbus==5000:test_vbus==0)&&restores>=resumes+1);++resumes;input_paused=false;}
  }else if(prop==NP_BUCK_FCC){
   if(control.buck.backend==NP_BUCK_OS4_STOCK_POLICY){
    assert(!cp_on&&!input_paused&&!ffc_on&&value<=3000);++normal_fcc_writes;
    if(value>normal_peak_fcc)normal_peak_fcc=value;
    if(thermal_low)assert(value<=1000);
   }else assert(value==100||value==500);
   test_fcc=value;
  }else if(prop==NP_BUCK_ICL){
   assert(!cp_on&&!input_paused&&value<=3000);
   if(value>1600){
    // Any authenticated source can use its completed low-FCC handoff. Check
    // the real state instead of restricting this route to scenario numbers.
    const bool direct_new_source=charge_on&&
       (control.buck.confirmation==NP_BUCK_CONFIRM_REDUCTION_ACK||
        control.buck.confirmation==NP_BUCK_CONFIRM_CURRENT)&&
       control.buck.target_fcc==500&&control.buck.stock.actual[1]==4470&&
       control.buck.stock.actual[3]==1&&control.buck.epoch==control.epoch&&
       control.buck.attachment==control.attachment;
    assert((normal_seen||direct_new_source)&&!thermal_low&&value==3000);
    assert(control.buck.stock.reentry==1);
    ++source_current_grants;
   }
   test_icl=value;
  }
  else if(prop==NP_BUCK_TERM_VOLT){assert(!cp_on&&!ffc_on&&value<=4470);term_mv=value;}
  else if(prop==NP_BUCK_ENABLE){assert(!cp_on&&!ffc_on&&value<=1);charge_on=(uint8_t)value;}
  else assert(0&&"Only original charging properties enter the wire");
  ++writes;
 }
 memcpy(wire,p,n);wire_pending=true;reply_at=test_ms+(delayed_wire()?50:20);return 0;
}
static int current_poll(void *){
 if(!wire_pending||test_ms<reply_at)return 0;
 uint8_t answer[NP_MCA_RESPONSE]={};memcpy(answer,wire,16);np_put32(answer+20,np_le32(wire+16));
 const uint32_t prop=np_le32(wire+12);uint32_t value=0;
 switch(prop){
 case 0x21012:assert(process_flag==1);value=0x2717;break;
 case 0x2100f:value=0xa1b2c3d4;break;
 case 0x21002:value=process_flag;break;
 case 0x2100b:value=verified_flag;break;
 case 0x21015:value=vdm_state;break;
 case 0x21007:memcpy(answer+24,response_bytes,16);break;
 case 0x20001:value=source_connected;break;case 0x2001:value=!source_connected||input_paused||!charge_on?7:3;break;
 case 0x20002:value=test_vbus*1000;break;case 0x20004:value=!source_connected||input_paused||!charge_on?0:500000;break;
 case NP_PACK_TBAT_PROPERTY:value=40;break;
 case NP_BUCK_ICL:value=scenario==42?UINT32_MAX:foreign_clamp&&test_icl>1200?1200:test_icl;break;case NP_BUCK_TERM_VOLT:value=term_mv;break;
 case NP_BUCK_ENABLE:value=charge_on;break;case 0x2000c:value=test_type;break;
 case 0x2100d:value=((scenario>=37&&scenario<=42)||scenario==44||(scenario>=50&&scenario<=52)||(quick_sdp()&&source_reconnected))?0:actual_data_role;break;
 case 0x2100e:value=((scenario>=37&&scenario<=42)||scenario==44||(scenario>=50&&scenario<=52)||(quick_sdp()&&source_reconnected))?1:2;break;
 case 0x21014:
  if((scenario>=37&&scenario<=42)||scenario==44||(scenario>=50&&scenario<=52)||(quick_sdp()&&source_reconnected))break;
  np_put32(answer+24,5000);np_put32(answer+28,5000);np_put32(answer+32,3000);
  if(scenario!=11||test_ms>=catalog_refresh_after){
   np_put32(answer+40,3600);np_put32(answer+44,20000);np_put32(answer+48,apdo_ma);np_put32(answer+52,3);
  }
  ++catalog_reads;break;
 case 0x21022:value=1;break;case 0x21023:value=0;break;
 case 0x2101c:case NP_BUCK_INPUT_SUSPEND:assert(np_le32(wire+8)!=NP_MCA_READ);break;
 default:break;
 }
 if(prop!=0x21014&&prop!=0x21007)np_put32(answer+24,value);
 if(scenario==5&&prop==NP_BUCK_PPS_PD&&cp_on&&!failed_pps){failed_pps=true;np_put32(answer+16,1);}
 const bool regulation=prop==NP_BUCK_PPS_PD&&np_le32(wire+8)==NP_MCA_WRITE&&
  control.buck.state==NP_BUCK_PUMP_REGULATE;
 wire_pending=false;++delivered;np_power27_receive(1,answer,sizeof(answer));
 if((scenario==12||delayed_wire())&&regulation&&!failed_pps&&
     auxiliaryCommand.attachment==control.attachment){
  assert(control.buck.state==NP_BUCK_PUMP_OBSERVE_REGULATION);
  assert(control.buck.voltage_after_ms==test_ms&&control.buck.voltage_samples==0);
  assert(!refreshAt);awaiting_first_ack_cohort=true;
  ack_sample_generation=control.sample.generation;
 }
 if(prop==NP_BUCK_PPS_PD&&np_le32(wire+8)==NP_MCA_WRITE&&!failed_pps)pps_ack_ms=test_ms;
 return 0;
}
static void tick(unsigned){
 if(physical_detach()&&test_ms>=200000&&!source_detached){source_detached=true;source_connected=false;awaiting_first_ack_cohort=false;test_vbus=0;test_type=0;np_control_usb_type(&control,0,0,test_ms);}
 if(physical_detach()&&source_detached&&!source_reconnected&&test_ms>=((scenario==33||quick_sdp())?200200u:300000u)){
  if(scenario!=33&&!quick_sdp())assert(!cp_on&&!input_paused&&!ffc_on&&!np_buck_all_owned(&control.buck));
  source_connected=true;source_reconnected=true;test_vbus=5000;
  if(quick_sdp())normal_peak_fcc=0;
  if(retained_source()){
   /* The real094 connection retains the ACKed HLOS100/FCC100/enable0.
    * No unrelated actor configures a fictional fresh3000mA baseline. */
   assert(test_icl==100&&test_fcc==100&&charge_on==0&&!enables);
   if(scenario==47||scenario==53)foreign_clamp=true;
   if(scenario==49)thermal_low=true;
  }else if(!quick_sdp()){test_icl=3000;charge_on=1;}
  test_type=quick_sdp()?1:scenario==36?3:scenario==46?12:11;np_control_usb_type(&control,1,test_type,test_ms);
  last_run_PPS_ms=last_run_PPS_generation=ack_sample_generation=0;awaiting_first_ack_cohort=false;
 }

 if(!async_scene()){
 NpBoardThermal b={};b.enabled=1;b.epoch=control.epoch;b.attachment=control.attachment;
 b.generation=stagingBoard.generation+1;b.begin_ms=test_ms-50;b.valid_mask=b.attempted_mask=255;
 const int32_t mc=scene_test()?41000:scenario==37||scenario==39?46000:thermal_low?(scenario==8?46000:44700):39500;
 const int32_t input=((mc-1307)*1000+964)/965;
 for(unsigned i=0;i<8;++i){b.input_milli_c[i]=input;b.sampled_ms[i]=test_ms-2;}
 stagingBoard=b;publish_board();
 if(scene_test()&&stagingBoard.derived_valid&&control.charger_thermal.valid&&
    control.sample.valid&&control.sample.external&&!finish_queued&&lifecycle==1){
  const auto &t=control.charger_thermal;
  if(scenario==57||(scenario==56&&test_ms>=300000))assert(t.current_ua<=5000000);
  else if(test_ms>=120000)assert(t.current_ua>5000000&&t.current_ua<=12400000);
 }
 NpConnectorThermal t={};t.enabled=1;t.generation=test_ms;t.begin_ms=test_ms-2;t.end_ms=test_ms;
 t.attempted=t.valid_mask=3;t.sampled_ms[0]=t.sampled_ms[1]=test_ms;
 for(auto &r:t.reading){r.converted=r.quiescent=1;r.centi_c=3000;}
 np_control_connector_sample(&control,&t,test_ms);
 }else if(boardThermal.derived_valid&&boardThermal.generation!=scene_seen_generation){
  scene_seen_generation=boardThermal.generation;++scene_samples;
  assert(boardThermal.valid_mask==255&&!boardThermal.retained);
  assert(boardThermal.end_ms>=boardThermal.begin_ms&&
         boardThermal.end_ms-boardThermal.begin_ms<=2000);
  const uint64_t expected=boardThermal.begin_ms+1000;
  assert(connectorAt==expected);
  if(scene_previous_begin){
   const uint64_t period=boardThermal.begin_ms-scene_previous_begin;
   if(period>scene_max_period)scene_max_period=period;
   assert(period>=1000&&period<1700);
  }
  scene_previous_begin=boardThermal.begin_ms;
  const auto &vote=control.charger_thermal;
  assert(vote.valid&&vote.generation==boardThermal.generation);
  if(scenario==60){
   if(test_ms>=220000&&test_ms<249000){assert(vote.current_ua==3500000);++scene_low_samples;}
   if(test_ms>=400000){assert(vote.current_ua>6000000);++scene_recovered_samples;}
   if(test_ms>=500000)assert(vote.current_ua>8000000);
  }else if(test_ms>=400000)assert(vote.current_ua<=5000000);
 }
 if(scenario==20&&test_ms>350000)runtime_log_capped=1;
 if(scenario==25&&test_ms>500000){persistentLog=0;}
 if((scenario==26||scenario==27||scenario==29||scenario==30||scenario==34||scenario==39)&&test_ms>450000&&lifecycle==1&&pm_stage==0){pm_stage=1;assert(np_power_lifecycle_request(0)==1);}
 if(scenario==26&&lifecycle==3&&pm_stage==1){assert(!cp_on&&!input_paused&&!np_buck_all_owned(&control.buck));pm_stage=2;system_inshutdown=1;stopRequested=1;}
 if((scenario==27||scenario==29||scenario==30||scenario==34||scenario==39)&&lifecycle==3&&pm_stage==1){assert(!cp_on&&!input_paused&&!np_buck_all_owned(&control.buck));pm_stage=2;assert(np_power_lifecycle_request(1)==1);}

 if((scenario==19||scenario==35)&&test_ms>=60000)thermal_low=false;
 if(scenario==49&&test_ms>=400000)thermal_low=false;
 if(scenario==53&&test_ms>=450000)foreign_clamp=false;
 if(scenario==48&&source_reconnected&&enables&&test_ms>=450000&&
    lifecycle==1&&pm_stage==0){pm_stage=1;assert(np_power_lifecycle_request(0)==1);}
 if(scenario==48&&lifecycle==3&&pm_stage==1){
  assert(!cp_on&&!input_paused&&!np_buck_all_owned(&control.buck));
  pm_stage=2;assert(np_power_lifecycle_request(1)==1);
 }
 if(scenario==22&&enables==1&&test_ms>=150000&&!thermal_returned)thermal_low=true;
 if(control.buck.backend==NP_BUCK_OS4_STOCK_POLICY&&control.buck.state==NP_BUCK_POLICY_MONITOR){
  normal_seen=true;
  if(scenario==22&&disables){thermal_returned=true;thermal_low=false;}
  if(scenario==21&&control.jeita.vote.full){
   assert(!cp_on&&!input_paused&&!ffc_on&&!charge_on);full_observed=true;
   if(test_ms>=500000)recharge_ready=true;
  }
 }

 if(scenario>=40&&scenario<=42&&test_ms>=200000&&!finish_queued){
  assert(!cp_on&&!enables);
  if(scenario==40||scenario==41)assert(!writes);
  else assert(control.buck.state==NP_BUCK_UNCERTAIN&&normal_fcc_writes==0);
  finish_queued=true;system_inshutdown=stopRequested=1;return;
 }
 if(test_ms>=660000&&!finish_queued){
  if(async_scene()){
   assert(scene_samples>300&&scene_max_period<1700);
   if(scenario==60)assert(scene_low_samples>10&&scene_recovered_samples>10);
  }
  if(ordinary_source()){
   assert(!cp_on&&!input_paused&&enables==(quick_sdp()?1u:0u)&&normal_seen);
   assert(normal_peak_fcc==ordinary_peak_fcc());
   assert(test_icl==ordinary_icl());
  }else{
   assert(cp_on&&input_paused&&ffc_on&&control.buck.armed);
   assert(control.buck.cccv_updates>=20);
  }
  if(scenario==19)assert(normal_seen&&enables==1);
  if(scenario==21)assert(full_observed&&recharge_ready&&normal_seen&&enables==2);
  if(scenario==22)assert(thermal_returned&&normal_seen&&enables==2);
  if(scenario==27)assert(pm_stage==2&&enables==2);
  if(scenario==29||scenario==30||scenario==34||scenario==39)assert(pm_stage==2&&auth.battery_checks==1&&normal_seen);
  if(scenario==28||scenario==32)assert(auth.attempts==1&&auth.verified&&auth.battery_checks==1&&process_flag==0);
  if(scenario==31||scenario==33||scenario==35)assert(source_detached&&source_reconnected&&enables==2&&auth.verified);
  if(scenario==43)assert(normal_seen&&enables==1&&control.buck.stock.reentry==3);
  if(scenario==45||scenario==46||scenario==49){
   assert(source_detached&&source_reconnected&&normal_seen&&enables==1&&
          source_current_grants==1&&auth.verified);
  }
  if(scenario==47){
   assert(source_detached&&source_reconnected&&normal_seen&&auth.verified&&
          source_current_grants==1&&!enables&&!pump_write_attempts&&foreign_clamp);
  }
  if(scenario==48)assert(pm_stage==2&&enables==2&&auth.battery_checks==1);
  if(scenario==53){
   assert(source_reconnected&&source_current_grants==2&&!foreign_clamp&&
          normal_seen&&enables==1&&auth.verified);
  }
  if(scenario==36)assert(source_detached&&source_reconnected&&!enables&&normal_seen);
  if(quick_sdp())assert(source_detached&&source_reconnected&&enables==1&&disables==1&&
    test_type==1&&test_icl==500&&normal_peak_fcc==800&&restores==1&&resumes==1);
  assert(control.buck.lease_until==UINT64_MAX&&control.buck.stock_continuous);
  finish_queued=true;assert(np_power_finish_request()==1);
 }
 if(control.buck.state==NP_BUCK_FINISHED&&!control.buck.owned&&finish_queued){system_inshutdown=1;stopRequested=1;}
 assert(test_ms<720000);
}
int PE_parse_boot_argn(const char *key,void *out,int size){
 if(!strcmp(key,"nezha-power")){assert(size==4);*(int *)out=12;return 1;}
 if(!strcmp(key,"nezha-power-pack-tbat")){assert(size==4);*(int *)out=delayed_wire();return 1;}
 if(!strcmp(key,"nezha-power-log")){assert(size==4);*(uint32_t *)out=1;return 1;}
 return 0;
}
int kernel_thread_start(void (*entry)(void *,wait_result_t),void *context,thread_t *out){
 created_worker=entry;created_context=context;++threads;*out=nullptr;return KERN_SUCCESS;
}
int main(int argc,char **argv){
 assert(argc==2);scenario=(unsigned)strtoul(argv[1],nullptr,10);assert(scenario>=18&&scenario<=62);
 thermal_low=scenario==19||scenario==35||scenario==37||scenario==39;
 test_mode=100;test_icl=scenario>=37?(scenario==40?0:scenario==41?6000:UINT32_MAX):3000;
 test_type=(scenario==23||scenario==29||scenario==36)?3:
  (scenario==24||scenario==30||(scenario>=37&&scenario<=42)||retained_source())?1:
  scenario==34||scenario==44?2:scenario==32?12:scenario==50?4:scenario==51?9:scenario==52?15:11;
 if(scenario==58){
  // Actual098 attachment begins with acknowledged HLOS100/enable0 and
  // previousFV4400. No fixture actor establishes a fictitious enabled buck.
  test_icl=100;charge_on=0;term_mv=4400;
 }
 if(scenario==59){test_icl=100;charge_on=0;term_mv=4470;}
 NpAuthMaterial &material=test_material;
 memcpy(material.magic,"NP18AUTH",8);np_put32(material.version,1);np_put32(material.fg_selector,2);
 for(unsigned i=0;i<10;++i){for(unsigned j=0;j<16;++j)material.seeds[i][j]=(uint8_t)(i*19+j);for(unsigned j=0;j<32;++j)material.adapter_keys[i][j]=(uint8_t)(i*17+j);}
 for(unsigned j=0;j<32;++j)material.battery_key[j]=(uint8_t)(j+31);
 NpPowerOps ops={};ops.control_mode=9;ops.stock_support100w_valid=ops.stock_support100w=1;
 ops.stock_current_enabled=ops.input_suspend_enabled=ops.ffc_enabled=ops.stock_continuous=1;
 if(scene_test()||async_scene())ops.read_screen_state=scene_screen;
 ops.context=nullptr;ops.now_ms=now;ops.poll_link=current_poll;ops.send_mca=current_send;
 ops.read_gauge=current_gauge;ops.read_pump=read_cp;ops.quiesce=keep_connected_quiet;ops.resume=restore_connected;
 ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;ops.country_cn=1;
 ops.property_online=0x20001;ops.property_status=0x2001;ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
 ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
 ops.auth_enabled=1;ops.auth_material=&material;ops.auth={nullptr,test_random,test_hmac,test_digest};ops.read_identity=identity;ops.set_gauge_fast_mode=fast;
 ops.set_pump_mode1=mode1;ops.set_pump_forward_mode=forward;ops.set_pump_watchdog=watchdog;
 ops.feed_pump_watchdog=feed;ops.enable_pump=enable2;ops.enable_pump_forward_mode=enable_forward;
 ops.disable_pump=disable;ops.prepare_pump_exit=adc_exit;ops.restore_pump_mode=restore;
 ops.begin_connector=connector_start;ops.poll_connector=connector_poll;ops.quiesce_adc=connector_quiet;
 ops.read_original_temperature=original_temperature;
 if(scenario==999){
  ops.read_original_temperature=nullptr;assert(np_power27_bind(&ops)==-1);
  ops.read_original_temperature=original_temperature;ops.input_suspend_enabled=0;assert(np_power27_bind(&ops)==-1);
  ops.input_suspend_enabled=1;ops.ffc_enabled=0;assert(np_power27_bind(&ops)==-1);
  assert(!bound&&!started&&!threads&&!writes);puts("PASS stockCurrent083 binding requires original current/input/FFC callbacks");return 0;
 }
 assert(!np_power27_bind(&ops));persistentLog=1;np_control_link(&control,1,1);np_mca_link(&mca,1);transportEpoch=1;
 auth.enabled=auth.battery_ok=auth.battery_checked=1;auth.verified=!ordinary_source()&&scenario!=28&&!original_type12();
 auth.epoch=auth.attachment=1;auth.state=(scenario==28||scenario==32)?NP_AUTH_IDLE:NP_AUTH_DONE;
 if(scenario==28||scenario==32)auth.battery_ok=auth.battery_checked=0;
 identity(nullptr,&gaugeIdentity);np_control_profile(&control,&gaugeIdentity,1);
 for(auto &m:control.meta){m.valid=1;m.at_ms=test_ms;}
 control.meta[NP_META_ICL].value=test_icl;control.meta[NP_META_TERM].value=term_mv;control.meta[NP_META_ENABLED].value=charge_on;
 control.meta[NP_META_TYPE].value=test_type;control.meta[NP_META_PD_STATE].value=2;control.meta[NP_META_DATA_ROLE].value=1;
 control.caps.valid=1;control.caps.epoch=1;control.caps.at_ms=test_ms;control.caps.present_mask=3;
 if((scenario>=37&&scenario<=42)||scenario==44||(scenario>=50&&scenario<=52)){control.meta[NP_META_DATA_ROLE].valid=control.meta[NP_META_PD_STATE].valid=0;}
 control.caps.entry[0]={5000,5000,3000,NP_PD_FIXED};control.caps.entry[1]={3600,20000,5000,NP_PD_APDO};
 if(scenario==11){
  /* Same post-auth invalidation as the real auxiliary_done path. */
  memset(&control.caps,0,sizeof(control.caps));
  memset(&control.meta[NP_META_PDOS],0,sizeof(control.meta[NP_META_PDOS]));
  control.meta[NP_META_TYPE].valid=control.meta[NP_META_VERIFY].valid=0;
  catalog_refresh_after=test_ms+10000;
  control.meta[NP_META_PDOS].retry_ms=catalog_refresh_after;
 }
 if(delayed_wire())np_control_connector_policy_enable(&control,1);
 additional_sleep=tick;additional_log=record;
 nezha_power27_start();nezha_power27_start();assert(threads==1&&created_worker==worker);
 created_worker(created_context,0);
 if(scenario>=40&&scenario<=42){
  assert(workerExited&&!cp_on&&!input_paused&&!ffc_on);
  if(scenario==42)assert(lifecycle==-1&&control.buck.owned&&!normal_fcc_writes);
  else assert(lifecycle==5&&!np_buck_all_owned(&control.buck));
  printf("PASS ordinary input bad/unconfirmed reply case%u preserved without unsafe writes\n",scenario);return 0;
 }
 assert(workerExited&&(log_stopped||scenario==25)&&lifecycle==5&&!cp_on&&!input_paused&&!ffc_on);
 assert(control.buck.state==NP_BUCK_FINISHED&&!control.buck.pending&&!control.buck.owned&&!control.buck.unverified_effect);
 assert(enables==disables&&pauses==resumes&&(finish_queued||scenario==26));
 if(!ordinary_source()||quick_sdp())assert(feeds&&enables);
 if(async_scene())printf("PASS asyncSIC104 scene%u completed_cohorts=%u max_period_ms=%llu low_samples=%u recovery_samples=%u; one ADC owner\n",
  scenario,scene_samples,(unsigned long long)scene_max_period,scene_low_samples,scene_recovered_samples);
 assert(control.buck.stock_continuous&&io.stock_continuous);
 assert(test_ms>(scenario==26?450000u:660000u));
 printf("PASS stock lifecycle runtime%u connection continues past300s, logcap=%u, updates=%u, termination full%u/recharge%u normal%u enables%u disables%u first_enable_ms=%llu; single worker exit\n",scenario,runtime_log_capped,control.buck.cccv_updates,full_observed,recharge_ready,normal_seen,enables,disables,(unsigned long long)initial_enable_ms);
 return 0;
}
