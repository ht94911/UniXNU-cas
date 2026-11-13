// SPDX-License-Identifier: BSD-2-Clause-Patent
// Only the new auth-enabled PACK/ITERM0 combination, using the existing peer.
#define PE_parse_boot_argn fixture_default_parse_boot
#include "host_stubs/runtime/pexpert/pexpert.h"
#undef PE_parse_boot_argn
int PE_parse_boot_argn(const char *,void *,int);
#define main legacy_runtime_auth_cases_not_run
#include "runtime_auth.cpp"
#undef main

static unsigned packReads;
int PE_parse_boot_argn(const char *name,void *out,int bytes)
{
 if(strcmp(name,"nezha-power-pack-tbat"))return 0;
 int value=1;assert(bytes==(int)sizeof(value));memcpy(out,&value,sizeof(value));return 1;
}
static int combined_send(void *context,const uint8_t *packet,uint32_t bytes)
{
 if(np_le32(packet+12)==NP_PACK_TBAT_PROPERTY){
  assert(np_le32(packet+8)==NP_MCA_READ);++packReads;
 }
 return send(context,packet,bytes);
}
static int combined_poll(void *context)
{
 if(queued&&ms>=ready_at&&np_le32(wire+12)==NP_PACK_TBAT_PROPERTY){
  uint8_t answer[NP_MCA_RESPONSE]={};memcpy(answer,wire,16);
  np_put32(answer+20,np_le32(wire+16));np_put32(answer+24,26);
  queued=false;np_power27_receive(1,answer,sizeof(answer));return 0;
 }
 return poll(context);
}
int main(void)
{
 scenario=17;policy_iterm=0;
 memcpy(material.magic,"NP18AUTH",8);np_put32(material.version,1);np_put32(material.fg_selector,2);
 for(unsigned i=0;i<10;++i){
  for(unsigned j=0;j<16;++j)material.seeds[i][j]=(uint8_t)(i*19+j);
  for(unsigned j=0;j<32;++j)material.adapter_keys[i][j]=(uint8_t)(i*17+j);
 }
 for(unsigned j=0;j<32;++j)material.battery_key[j]=(uint8_t)(j+31);
 NpPowerOps ops={};ops.now_ms=now;ops.poll_link=combined_poll;ops.send_mca=combined_send;
 ops.read_gauge=gauge_read;ops.read_pump=pump_read;ops.quiesce=ops.resume=quiet;
 ops.set_pump_usb_ovp=ovp;ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
 ops.property_online=0x20001;ops.property_status=0x2001;
 ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
 ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
 ops.control_mode=5;ops.country_cn=1;ops.read_identity=identity;
 ops.auth_enabled=1;ops.auth_material=&material;ops.auth={nullptr,random_bytes,hmac,digest};
 assert(!np_power27_bind(&ops));lifecycle=1;persistentLog=1;manualDisable=0;
 stock_until(NP_BUCK_POLICY_MONITOR,1);
 assert(auth.verified&&auth.battery_ok&&battery_calls==1&&packReads);
 assert(control.buck.stock.baseline[2]==0&&policy_iterm==358&&policy_fcc==1500);
 NpPackThermal raw={};NpSnapshot snapshot={};
 assert(np_power_thermal_copy(&raw)&&np_snapshot_read(&snapshot));
 if(!(raw.valid_raw&&!raw.error&&raw.pack_tbat_raw==26&&raw.generation==snapshot.generation))
  fprintf(stderr,"PACK combo: valid=%u error=%d raw=%d generation=%llu snapshot=%llu ms=%llu sampled=%llu reads=%u\n",
   raw.valid_raw,raw.error,raw.pack_tbat_raw,(unsigned long long)raw.generation,
   (unsigned long long)snapshot.generation,(unsigned long long)ms,(unsigned long long)raw.sampled_ms,packReads);
 assert(raw.valid_raw&&!raw.error&&raw.pack_tbat_raw==26&&raw.generation==snapshot.generation);
 assert(std::any_of(rows.begin(),rows.end(),[](const std::string &row){
  return row.rfind("T1 ",0)==0&&row.find("pack_tbat_raw=26 ")!=std::string::npos;
 }));
 assert(np_power_control_request(0)==1);stock_until(NP_BUCK_FINISHED,0);
 assert(!control.buck.owned&&!control.buck.unverified_effect&&policy_iterm==0);
 const unsigned reads=packReads;
 assert(np_power_control_request(1)==1);stock_until(NP_BUCK_POLICY_MONITOR,1);
 assert(policy_iterm==358&&control.buck.stock.baseline[2]==0&&packReads>reads);
 assert(!drain_control()&&!control.buck.owned&&!auth.owned&&policy_iterm==0);
 puts("PASS new actual Runtime combination: auth1/pack1/ITERM0, live target358, typed stop/restart with zero restoration and final ownership release; old cases not run");
 return 0;
}
