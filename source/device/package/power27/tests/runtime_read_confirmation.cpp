// SPDX-License-Identifier: BSD-2-Clause-Patent
// A delayed ITERM peer at the existing write deadline, using actual Runtime.
#define PE_parse_boot_argn fixture_default_parse_boot
#include "host_stubs/runtime/pexpert/pexpert.h"
#undef PE_parse_boot_argn
int PE_parse_boot_argn(const char *,void *,int);
#define main legacy_runtime_auth_cases_not_run
#include "runtime_auth.cpp"
#undef main

static unsigned packReads,termReads,termWrites;
static bool pendingTerm,forcedDue,gaugeDelay;
static uint32_t wantedTerm;
static uint64_t applyAt,initialSent,initialDeadline,initialObserved;
int PE_parse_boot_argn(const char *name,void *out,int bytes)
{
 if(strcmp(name,"nezha-power-pack-tbat"))return 0;
 int value=1;assert(bytes==(int)sizeof(value));memcpy(out,&value,sizeof(value));return 1;
}
static int delayed_send(void *context,const uint8_t *packet,uint32_t bytes)
{
 const uint32_t property=np_le32(packet+12),operation=np_le32(packet+8);
 const uint32_t oldTerm=policy_iterm;
 if(operation==NP_MCA_WRITE&&np_power27_write_allowed(packet,bytes)<=0){
  NpBuckFacts facts={};np_control_facts(&control,ms,&facts);
  fprintf(stderr,"new confirmation write refused: property=%x value=%u now=%llu state=%u stop=%u owned=%u index=%u until=%llu facts_valid=%u age=%llu draining=%u\n",
   property,np_le32(packet+20),(unsigned long long)ms,control.buck.state,control.buck.stop,
   control.buck.owned,control.buck.stock.index,(unsigned long long)control.buck.stock.confirm_until_ms,
   facts.valid,(unsigned long long)(ms-facts.sampled_ms),(unsigned)draining);
 }
 if(property==NP_PACK_TBAT_PROPERTY)++packReads;
 int result=send(context,packet,bytes);
 if(!result&&property==NP_BUCK_TERM_CURRENT){
  if(operation==NP_MCA_WRITE){
   ++termWrites;wantedTerm=np_le32(packet+20);policy_iterm=oldTerm;
   pendingTerm=true;applyAt=ms+1100;
   if(wantedTerm){initialSent=ms;initialDeadline=control.buck.command.deadline_ms;}
  }else if(initialSent)++termReads;
 }
 return result;
}
static int delayed_poll(void *context)
{
 if(pendingTerm&&ms>=applyAt){policy_iterm=wantedTerm;pendingTerm=false;}
 if(queued&&ms>=ready_at&&np_le32(wire+12)==NP_PACK_TBAT_PROPERTY){
  uint8_t answer[NP_MCA_RESPONSE]={};memcpy(answer,wire,16);
  np_put32(answer+20,np_le32(wire+16));np_put32(answer+24,36);
  queued=false;np_power27_receive(1,answer,sizeof(answer));return 0;
 }
 const bool termRead=queued&&ms>=ready_at&&np_le32(wire+12)==NP_BUCK_TERM_CURRENT&&
  np_le32(wire+8)==NP_MCA_READ&&initialSent;
 if(termRead&&policy_iterm==358&&!initialObserved)initialObserved=ms;
 int result=poll(context);
 if(termRead&&!forcedDue&&pendingTerm&&ms-initialSent>=1000){
  forcedDue=true;gaugeDelay=true;refreshAt=ms;
 }
 return result;
}
static int delayed_gauge(void *context,uint8_t out[NP_GAUGE_WORDS][2],uint32_t *valid)
{
 if(gaugeDelay){ms+=50;gaugeDelay=false;}
 return gauge_read(context,out,valid);
}
int main(int argc,char **argv)
{
 assert(argc==2);const bool expectedPass=atoi(argv[1])!=0;
 scenario=17;policy_iterm=0;
 memcpy(material.magic,"NP18AUTH",8);np_put32(material.version,1);np_put32(material.fg_selector,2);
 for(unsigned i=0;i<10;++i){
  for(unsigned j=0;j<16;++j)material.seeds[i][j]=(uint8_t)(i*19+j);
  for(unsigned j=0;j<32;++j)material.adapter_keys[i][j]=(uint8_t)(i*17+j);
 }
 for(unsigned j=0;j<32;++j)material.battery_key[j]=(uint8_t)(j+31);
 NpPowerOps ops={};ops.now_ms=now;ops.poll_link=delayed_poll;ops.send_mca=delayed_send;
 ops.read_gauge=delayed_gauge;ops.read_pump=pump_read;ops.quiesce=ops.resume=quiet;
 ops.set_pump_usb_ovp=ovp;ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
 ops.property_online=0x20001;ops.property_status=0x2001;
 ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
 ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
 ops.control_mode=5;ops.country_cn=1;ops.read_identity=identity;
 ops.auth_enabled=1;ops.auth_material=&material;ops.auth={nullptr,random_bytes,hmac,digest};
 assert(!np_power27_bind(&ops));lifecycle=1;persistentLog=1;manualDisable=0;
 for(unsigned i=0;i<6000;++i){
  step();ms+=50;
  if(control.buck.state==NP_BUCK_UNCERTAIN||control.buck.state==NP_BUCK_POLICY_MONITOR)break;
 }
 assert(auth.verified&&forcedDue&&initialSent&&termWrites==1&&termReads>1&&packReads);
 if(expectedPass){
  assert(control.buck.state==NP_BUCK_POLICY_MONITOR&&policy_fcc==1500&&policy_iterm==358);
  assert(initialObserved<initialDeadline&&!control.buck.unverified_effect);
  assert(!drain_control()&&!control.buck.owned&&!auth.owned&&policy_iterm==0&&termWrites==2);
 }else{
  assert(control.buck.state==NP_BUCK_UNCERTAIN&&control.buck.unverified_effect&&policy_fcc==500);
  assert(ms>=initialDeadline);
 }
 printf("PASS new actual Runtime read confirmation: expected%d first_match=%llu write_deadline=%llu read_count=%u; one configuration write, retained1500ms window, host only\n",
  (int)expectedPass,(unsigned long long)initialObserved,(unsigned long long)initialDeadline,termReads);
 return 0;
}
