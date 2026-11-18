// SPDX-License-Identifier: BSD-2-Clause-Patent
// Only the new real final-VDM/sample-cohort collision; original peer not run.
#define PE_parse_boot_argn fixture_default_parse_boot
#include "host_stubs/runtime/pexpert/pexpert.h"
#undef PE_parse_boot_argn
int PE_parse_boot_argn(const char *,void *,int);
#define main legacy_runtime_auth_not_run
#include "runtime_auth.cpp"
#undef main
static int packFlag=1;
static bool forceDue,gaugeDelay,lastSixRead;
static bool genuineLate,longNonce;
static unsigned packReads,packDuringPoll;
static uint64_t finalSent,finalReply,nonceSent;
int PE_parse_boot_argn(const char *name,void *out,int bytes)
{
 if(strcmp(name,"nezha-power-pack-tbat"))return 0;
 assert(bytes==(int)sizeof(packFlag));memcpy(out,&packFlag,sizeof(packFlag));return 1;
}
static int collision_send(void *context,const uint8_t *packet,uint32_t bytes)
{
 const uint32_t property=np_le32(packet+12);
 if(property==NP_PACK_TBAT_PROPERTY){
  assert(np_le32(packet+8)==NP_MCA_READ);++packReads;
  if(auth.state==NP_AUTH_POLL)++packDuringPoll;
 }
 int result=send(context,packet,bytes);
 if(property==0x21007&&np_le32(packet+8)==NP_MCA_WRITE)nonceSent=ms;
 if(genuineLate&&property==0x21015&&np_le32(packet+8)==NP_MCA_READ&&vdm_state==6)ready_at=ms+600;
 if(property==0x21008&&np_le32(packet+8)==NP_MCA_WRITE){
  finalSent=ms;verified_flag=1; // Actual OS4 already changes its diagnostic.
 }
 return result;
}
static int collision_poll(void *context)
{
 if(queued&&ms>=ready_at&&np_le32(wire+12)==NP_PACK_TBAT_PROPERTY){
  uint8_t answer[NP_MCA_RESPONSE]={};memcpy(answer,wire,16);
  np_put32(answer+20,np_le32(wire+16));np_put32(answer+24,36);
  queued=false;np_power27_receive(1,answer,sizeof(answer));return 0;
 }
 const bool finalACK=queued&&ms>=ready_at&&np_le32(wire+12)==0x21008&&np_le32(wire+8)==NP_MCA_WRITE;
 if(queued&&ms>=ready_at&&np_le32(wire+12)==0x21015&&vdm_state==6){lastSixRead=true;finalReply=ms;}
 const unsigned actualVDM=vdm_state;
 if(longNonce&&queued&&np_le32(wire+12)==0x21015&&vdm_state==5&&ms-nonceSent<2200)vdm_state=55;
 int result=poll(context);vdm_state=actualVDM;
 if(finalACK&&!forceDue){
  assert(auth.state==NP_AUTH_POLL&&auth.poll_until==finalSent+500);
  forceDue=true;gaugeDelay=true;refreshAt=ms; // Real029 had this cohort due.
 }
 return result;
}
static int collision_gauge(void *context,uint8_t out[NP_GAUGE_WORDS][2],uint32_t *valid)
{
 if(gaugeDelay){ms+=50;gaugeDelay=false;} // One bounded delayed gauge tick.
 return gauge_read(context,out,valid);
}
int main(int argc,char **argv)
{
 assert(argc>=3&&argc<=5);packFlag=atoi(argv[1]);const bool expectedPass=atoi(argv[2])!=0;
 genuineLate=argc>=4&&atoi(argv[3]);longNonce=argc>=5&&atoi(argv[4]);
 scenario=17;policy_iterm=0;
 memcpy(material.magic,"NP18AUTH",8);np_put32(material.version,1);np_put32(material.fg_selector,2);
 for(unsigned i=0;i<10;++i){
  for(unsigned j=0;j<16;++j)material.seeds[i][j]=(uint8_t)(i*19+j);
  for(unsigned j=0;j<32;++j)material.adapter_keys[i][j]=(uint8_t)(i*17+j);
 }
 for(unsigned j=0;j<32;++j)material.battery_key[j]=(uint8_t)(j+31);
 NpPowerOps ops={};ops.now_ms=now;ops.poll_link=collision_poll;ops.send_mca=collision_send;
 ops.read_gauge=collision_gauge;ops.read_pump=pump_read;ops.quiesce=ops.resume=quiet;
 ops.set_pump_usb_ovp=ovp;ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
 ops.property_online=0x20001;ops.property_status=0x2001;
 ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
 ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
 ops.control_mode=5;ops.country_cn=1;ops.read_identity=identity;
 ops.auth_enabled=1;ops.auth_material=&material;ops.auth={nullptr,random_bytes,hmac,digest};
 assert(!np_power27_bind(&ops));lifecycle=1;persistentLog=1;manualDisable=1;
 for(unsigned i=0;i<6000;++i){step();ms+=50;if(auth.state==NP_AUTH_DONE||auth.state==NP_AUTH_FAILED)break;}
 if(!(forceDue&&finalSent&&lastSixRead&&auth.poll_until==finalSent+500))
  fprintf(stderr,"collision ended: auth=%u error=%u owned=%u force=%u final_sent=%llu nonce_sent=%llu ms=%llu\n",
   auth.state,auth.error,auth.owned,(unsigned)forceDue,(unsigned long long)finalSent,(unsigned long long)nonceSent,(unsigned long long)ms);
 assert(forceDue&&finalSent&&lastSixRead&&auth.poll_until==finalSent+500);
 if(expectedPass){
  if(!auth.verified)fprintf(stderr,"auth=%u error=%u owned=%u sent=%llu reply=%llu until=%llu pack_during_poll=%u\n",
   auth.state,auth.error,auth.owned,(unsigned long long)finalSent,(unsigned long long)finalReply,
   (unsigned long long)auth.poll_until,packDuringPoll);
  assert(auth.state==NP_AUTH_DONE&&auth.verified&&finalReply<auth.poll_until);
  manualDisable=0;stock_until(NP_BUCK_POLICY_MONITOR,1);
  assert(policy_iterm==358&&control.buck.stock.baseline[2]==0);
  assert(!drain_control()&&!control.buck.owned&&!auth.owned&&policy_iterm==0);
 }else{
  assert(auth.state==NP_AUTH_FAILED&&auth.error==NP_AUTH_CLEANUP_ERROR&&auth.owned==NP_AUTH_OWNS_VDM_EFFECT);
  assert(finalReply>=auth.poll_until&&(packDuringPoll||genuineLate)&&policy_writes==0);
 }
 printf("PASS new collision case: pack%d expected_auth%d final6_reply_delta=%llu ms wire_budget=500 pack_reads=%u during_POLL=%u; unchanged deadlines, host only\n",
  packFlag,(int)expectedPass,(unsigned long long)(finalReply-finalSent),packReads,packDuringPoll);
 return 0;
}
