// SPDX-License-Identifier: BSD-2-Clause-Patent
// Drive the real worker, then consume its snapshot through the OS entry.
#define NP_BUCK_CV_CASE_MAIN prior_buck_cv_cases_not_executed
#include "runtime_buck_cv069.cpp"
#undef NP_BUCK_CV_CASE_MAIN

static unsigned projected,raw_preserved,post_stop;
static void observe_publisher(unsigned tick)
{
 advance(tick);
 NpSnapshot s={};
 if(!np_snapshot_read(&s))return;
 if(cp_on&&s.charger.state_valid&&s.charger.state_raw==7&&
    (s.valid&NP_BIT(NP_CHARGING))) {
  assert(s.charging==1&&s.charger.charging==0&&!s.usb_power_valid);
  ++projected;++raw_preserved;
 }
 if(!cp_on&&disable_calls&&s.charger.state_valid&&s.charger.state_raw==7) {
  assert(!s.charging);++post_stop;
 }
}
int main(void)
{
 variant=13;cccv_test=true;test_mode=100;test_icl=3000;test_type=11;
 NpAuthMaterial material={};NpPowerOps ops={};
 ops.now_ms=now;ops.poll_link=wire_poll;ops.send_mca=wire_send;
 ops.read_gauge=gauge63;ops.read_pump=cp_read;ops.quiesce=quiet;ops.resume=quiet;
 ops.control_mode=9;ops.stock_support100w_valid=1;ops.stock_support100w=1;
 ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
 ops.property_online=0x20001;ops.property_status=0x2001;
 ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
 ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
 ops.auth_enabled=1;ops.auth_material=&material;
 ops.begin_connector=connector_begin;ops.poll_connector=connector_poll;ops.quiesce_adc=connector_quiet;
 ops.set_pump_mode1=cp_mode1;ops.set_pump_watchdog=cp_watchdog;ops.feed_pump_watchdog=cp_feed;
 ops.set_pump_forward_mode=cp_forward;ops.enable_pump_forward_mode=cp_enable_forward;
 ops.enable_pump=cp_enable;ops.disable_pump=cp_disable;
 ops.prepare_pump_exit=cp_exit;ops.restore_pump_mode=cp_restore;
 assert(!np_power27_bind(&ops));persistentLog=1;
 np_control_link(&control,1,1);np_mca_link(&mca,1);transportEpoch=1;
 auth.enabled=auth.battery_ok=auth.battery_checked=auth.verified=1;
 auth.epoch=auth.attachment=1;auth.state=NP_AUTH_DONE;
 NpGaugeIdentity id={};id.valid=3;id.cell_code='F';memcpy(id.device_name,"BM6N",5);
 np_control_profile(&control,&id,1);gaugeIdentity=id;io.country_cn=1;
 for(auto &entry:control.meta){entry.valid=1;entry.at_ms=test_ms;}
 control.meta[NP_META_ICL].value=5000;control.meta[NP_META_TERM].value=4400;
 control.meta[NP_META_ENABLED].value=1;control.meta[NP_META_TYPE].value=11;
 control.meta[NP_META_DATA_ROLE].value=1;control.meta[NP_META_PD_STATE].value=2;
 control.caps.valid=1;control.caps.epoch=1;control.caps.present_mask=7;control.caps.at_ms=test_ms;
 control.caps.entry[0]={5000,5000,3000,NP_PD_FIXED};
 control.caps.entry[1]={3600,11000,6100,NP_PD_APDO};
 control.caps.entry[2]={3600,20000,5000,NP_PD_APDO};
 fresh_policy();additional_log=logging;additional_sleep=observe_publisher;
 for(unsigned i=0;i<2700&&!system_inshutdown;++i){step();IOSleep(50);}
 assert(system_inshutdown&&projected&&projected==raw_preserved&&post_stop);
 assert(control.buck.cccv.stage==3&&control.buck.cccv_updates>=4);
 assert(enable_calls==1&&disable_calls==1&&!cp_on&&restore_calls==1&&test_vbus==5000);
 assert(!control.buck.owned&&control.buck.state==NP_BUCK_FINISHED);
 assert(!drain_control()&&shutdown_lines==1);
 invalidate(NP_FAULT_STALE);NpSnapshot s={};
 assert(np_snapshot_read(&s)&&!s.valid&&!s.charging);
 printf("PASS actual worker publishes charging with rawbuck7: %u snapshots; stops and withdraws stale data\n",projected);
 return 0;
}
