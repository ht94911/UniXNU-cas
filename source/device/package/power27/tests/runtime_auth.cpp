// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real runtime/MCA/auth callers against a synthetic peer; no phone or bus I/O.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <openssl/hmac.h>
#include "../platform/NezhaPowerRuntime.cpp"
static uint64_t ms=1000,ready_at;
static uint8_t wire[NP_MCA_REQUEST],response[16];
static bool queued,linked,stopped,source_note;
static bool logger_busy_once,logger_busy_used;
static unsigned scenario,random_count,battery_calls,nonce_writes,process_flag,verified_flag,vdm_state,seed_index,type=10,svid_reads,data_role=1,role_writes;
static NpAuthMaterial material;
static std::vector<std::string> rows;
extern "C" {uint32_t system_inshutdown;}
void IOSleep(unsigned n){ms+=n;}
extern "C" IOService *np_battery_service_create(void){static IOService s;return &s;}
extern "C" void np_power_diagnostic_start(void){}
extern "C" int np_power_log_start(void){return 0;}
extern "C" void np_power_log_stop(void){stopped=true;}
extern "C" int np_power_log_status(NpPowerLogStatus *s){if(logger_busy_once){logger_busy_once=false;return 0;}memset(s,0,sizeof(*s));s->phase=stopped?3:2;return 1;}
extern "C" int np_power_log_push(const char *p,size_t n){rows.emplace_back(p,n);return 0;}
extern "C" void qcom_usb_debug_get_status(qcom_usb_debug_status *s){memset(s,0,sizeof(*s));s->phase=3;}
static uint64_t now(void *){return ms;}
static int hmac(void *,const uint8_t *key,size_t nk,const uint8_t *data,size_t nd,uint8_t out[32]){
 unsigned n=0;assert(HMAC(EVP_sha256(),key,(int)nk,data,nd,out,&n)&&n==32);return 0;
}
static int random_bytes(void *,uint8_t *out,size_t n){for(size_t i=0;i<n;++i)out[i]=(uint8_t)++random_count;return 0;}
static int digest(void *,const uint8_t nonce[32],uint8_t out[32]){++battery_calls;ms+=400;return hmac(nullptr,material.battery_key,32,nonce,32,out);}
static void reverse(uint8_t out[16],const uint8_t in[16]){for(unsigned i=0;i<16;i+=4)for(unsigned j=0;j<4;++j)out[i+j]=in[i+3-j];}
static unsigned command(uint32_t p){switch(p){case 0x21003:return 1;case 0x21004:return 2;case 0x21005:return 3;case 0x21006:return 4;case 0x21007:return 5;case 0x21009:return 7;case 0x21008:return 6;default:return 0;}}
static int send(void *,const uint8_t *p,uint32_t bytes){
 assert(bytes==sizeof(wire)&&!queued);
 if(np_le32(p+8)==NP_MCA_WRITE){
  if(scenario==4&&!logger_busy_used&&np_le32(p+12)==0x21007) {
   logger_busy_once=true;
   assert(np_power27_write_allowed(p,bytes)==-1);
   logger_busy_used=true;return -1; // Nothing sent; the same request can be submitted when the logger is available.
  }
  assert(np_power27_write_allowed(p,bytes));
  uint8_t altered[NP_MCA_REQUEST];memcpy(altered,p,sizeof(altered));altered[20]^=1;
  assert(!np_power27_write_allowed(altered,bytes));
  unsigned prop=np_le32(p+12),value=np_le32(p+20);
  if(prop==0x21002)process_flag=value;
  else if(prop==0x2100b)verified_flag=value;
  else if(prop==0x2100d){assert(value==2&&process_flag==1&&!role_writes);data_role=value;++role_writes;}
  else{
   vdm_state=command(prop);assert(vdm_state);
   if(vdm_state==4){
    uint8_t natural[16];reverse(natural,p+20);seed_index=10;
    for(unsigned i=0;i<10;++i)if(!memcmp(natural,material.seeds[i],16))seed_index=i;
    assert(seed_index<10);
   }
   if(vdm_state==5){
    ++nonce_writes;uint8_t input[20],mac[32];reverse(input,p+20);
    input[16]=0xa1;input[17]=0xb2;input[18]=0xc3;input[19]=0xd4;
    hmac(nullptr,material.adapter_keys[seed_index],32,input,20,mac);reverse(response,mac);
    if(scenario==1)return 0; // Accepted nonce; no ACK, and no replay.
    if(scenario==2)source_note=true;
   }
  }
 }
 memcpy(wire,p,bytes);queued=true;ready_at=ms+5;return 0;
}
static int poll(void *){
 if(!linked){linked=true;np_power27_link(1,1);}
 if(source_note){
  source_note=false;if(scenario!=5)type=1;uint8_t note[NP_MCA_NOTIFICATION]={0};np_put32(note,NP_MCA_OWNER);np_put32(note+4,2);
  np_power27_receive(1,note,sizeof(note));
 }
 if(queued&&ms>=ready_at){
  uint8_t answer[NP_MCA_RESPONSE]={0};np_put32(answer,NP_MCA_OWNER);np_put32(answer+4,1);
  np_put32(answer+8,np_le32(wire+8));np_put32(answer+12,np_le32(wire+12));np_put32(answer+20,np_le32(wire+16));
  if(np_le32(wire+8)==NP_MCA_READ){
   uint32_t prop=np_le32(wire+12),value=0;
   switch(prop){
   case 0x21012:
    ++svid_reads;assert(battery_calls==1&&process_flag==1&&data_role==2);
    value=scenario==6?0:scenario==7?0x1234:scenario==5&&svid_reads<3?0:0x2717;break;
   case 0x2100f:value=0xa1b2c3d4;break;
   case 0x21002:value=process_flag;break;case 0x2100b:value=verified_flag;break;
   case 0x21015:value=vdm_state;break;
   case 0x21007:memcpy(answer+24,response,16);break;
   case 0x21014:
    np_put32(answer+24,5000);np_put32(answer+28,5000);np_put32(answer+32,3000);
    np_put32(answer+40,9000);np_put32(answer+44,9000);np_put32(answer+48,3000);break;
   case 0x2100e:value=type==10?2:1;break;
   case 0x2100d:value=data_role;break;
   case 0x20001:case 0x2001:case 0x2003:value=1;break;
   case 0x20002:value=5000000;break;case 0x20004:value=500000;break;
   case 0x2008:value=3000;break;case 0x2007:value=4400;break;case 0x2000c:value=type;break;
   }
   if(prop!=0x21007&&prop!=0x21014)np_put32(answer+24,value);
  }
  queued=false;np_power27_receive(1,answer,sizeof(answer));
 }
 return 0;
}
static int gauge_read(void *,uint8_t out[NP_GAUGE_WORDS][2],uint32_t *valid){
 uint16_t words[10]={4250,100,3032,79,4500,5729,6800,327,90,0};
 for(unsigned i=0;i<10;++i){out[i][0]=(uint8_t)words[i];out[i][1]=(uint8_t)(words[i]>>8);}*valid=0x3ff;ms+=4;return 0;
}
static int pump_read(void *,NpPump *p){memset(p,0,sizeof(*p));p->chip=0x85;p->controls_valid=p->switching_valid=1;p->valid=18;p->bus_mv=5000;p->die_centi_c=3000;return 0;}
static int ovp(void *,uint8_t,uint8_t,uint8_t *,uint8_t *){assert(!"Auth must not change OVP");return -1;}
static int quiet(void *){return 0;}
int main(int argc,char **argv){
 if(argc>1)scenario=(unsigned)strtoul(argv[1],nullptr,10);
 assert(scenario<=7);if(scenario==5)type=1;
 memcpy(material.magic,"NP18AUTH",8);np_put32(material.version,1);np_put32(material.fg_selector,2);
 for(unsigned i=0;i<10;++i){for(unsigned j=0;j<16;++j)material.seeds[i][j]=(uint8_t)(i*19+j);for(unsigned j=0;j<32;++j)material.adapter_keys[i][j]=(uint8_t)(i*17+j);}
 for(unsigned j=0;j<32;++j)material.battery_key[j]=(uint8_t)(j+31);
 NpPowerOps ops={};ops.now_ms=now;ops.poll_link=poll;ops.send_mca=send;ops.read_gauge=gauge_read;
 ops.quiesce=quiet;ops.resume=quiet;ops.read_pump=pump_read;ops.set_pump_usb_ovp=ovp;
 ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;ops.property_online=0x20001;ops.property_status=0x2001;
 ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
 ops.control_mode=4;ops.auth_enabled=1;ops.auth_material=&material;ops.auth={nullptr,random_bytes,hmac,digest};
 assert(!np_power27_bind(&ops));persistentLog=1;manualDisable=1;
 bool drained=false;
 for(unsigned i=0;i<2500;++i){
  step();ms+=20;
  if(scenario==5&&type==1&&battery_calls){assert(!nonce_writes&&!svid_reads&&auth.battery_ok);type=10;source_note=true;}
  if(scenario==3&&nonce_writes&&!drained){assert(!drain_control());drained=true;break;}
  if(auth.state==NP_AUTH_DONE||(auth.state==NP_AUTH_FAILED&&!auth.owned))break;
 }
 if(scenario==0||scenario==4||scenario==5){
  if(!auth.verified)fprintf(stderr,"runtime auth state=%u error=%u owned=%u stage=%u\n",auth.state,auth.error,auth.owned,auth.index);
  assert(auth.verified&&verified_flag==1&&!process_flag&&nonce_writes==1&&!np_auth_busy(&auth));
  NpAuthStatus status={};assert(np_power_auth_copy(&status)&&status.adapter_verified&&status.primary_gauge_verified);
  assert(!drain_control());
  assert(scenario!=4||logger_busy_used);
 }else if(scenario>=6){
  assert(!auth.verified&&!verified_flag&&!process_flag&&!auth.owned&&!nonce_writes&&auth.battery_ok);
  assert(auth.error==(scenario==6?NP_AUTH_TIMEOUT:NP_AUTH_PROTOCOL));
  assert(auth.svid==(scenario==6?0u:0x1234u));
 }else assert(!auth.verified&&!verified_flag&&!process_flag&&!auth.owned&&nonce_writes==1);
 assert(!np_auth_busy(&auth)&&battery_calls==1);
 unsigned count=0;
 for(const auto &row:rows)if(row.rfind("A2 ",0)==0){++count;assert(row.find("raw=")==std::string::npos&&row.find("challenge=")==std::string::npos&&row.find("digest=")==std::string::npos);}
 assert(count>(scenario==7?1u:5u));
 assert(auth.battery_checks==1);
 assert(std::any_of(rows.begin(),rows.end(),[](const std::string &s){return s.find("stage=primary checks=1 verified=1 error=0")!=std::string::npos;}));
 printf("PASS actual runtime auth scenario%u: one nonce, epoch/attachment checks, redacted A2 and normal drain\n",scenario);
}
