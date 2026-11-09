/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Synthetic host-only peer; no stock constants, I2C, phone or USB access. */
#include "NezhaAuth.h"
#include "NezhaMca.h"
#include <openssl/hmac.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
 NpAuthMaterial material;
 uint8_t battery_key[32],nonce[16],response[16];
 unsigned random,battery_calls,writes[32],seed,vdm_state,process,verified;
 unsigned fail_battery,mismatch,wrong_svid,lost_nonce_reply,source_change,wrong_key;
 unsigned published_true,commands[7],command_count,data_role,role_requests,lost_role_reply,no_role_response;
} Peer;
static int hmac(void *v,const uint8_t *key,size_t nk,const uint8_t *data,size_t nd,uint8_t out[32])
{ (void)v;unsigned n=0;assert(HMAC(EVP_sha256(),key,(int)nk,data,nd,out,&n)&&n==32);return 0; }
static int random_bytes(void *v,uint8_t *out,size_t n)
{ Peer *p=v;for(size_t i=0;i<n;++i)out[i]=(uint8_t)++p->random;return 0; }
static int battery(void *v,const uint8_t nonce[32],uint8_t out[32])
{ Peer *p=v;++p->battery_calls;if(p->fail_battery)return -1;return hmac(p,p->battery_key,32,nonce,32,out); }
static void reverse(uint8_t out[16],const uint8_t in[16])
{ for(unsigned i=0;i<16;i+=4)for(unsigned j=0;j<4;++j)out[i+j]=in[i+3-j]; }
static void prepare(Peer *p)
{
 memset(p,0,sizeof(*p));memcpy(p->material.magic,"NP18AUTH",8);
 p->data_role=1;
 np_put32(p->material.version,1);np_put32(p->material.fg_selector,2);
 for(unsigned i=0;i<10;++i){
  for(unsigned j=0;j<16;++j)p->material.seeds[i][j]=(uint8_t)(i*19+j);
  for(unsigned j=0;j<32;++j)p->material.adapter_keys[i][j]=(uint8_t)(i*17+j);
 }
 for(unsigned j=0;j<32;++j)p->battery_key[j]=p->material.battery_key[j]=(uint8_t)(j+31);
}
static unsigned command_of(uint32_t property)
{
 switch(property){case 0x21003:return 1;case 0x21004:return 2;case 0x21005:return 3;
 case 0x21006:return 4;case 0x21007:return 5;case 0x21009:return 7;case 0x21008:return 6;default:return 0;}
}
static void accept_write(Peer *p,const NpAuthCommand *c)
{
 ++p->writes[c->property&31];
 if(c->property==0x21002){assert(c->write_bytes==4);p->process=np_le32(c->data);return;}
 if(c->property==0x2100b){assert(c->write_bytes==4);p->verified=np_le32(c->data);p->published_true+=p->verified==1;return;}
 if(c->property==0x2100d){assert(p->process==1&&c->write_bytes==4&&np_le32(c->data)==2&&!p->role_requests);
  if(!p->no_role_response)p->data_role=2;
  ++p->role_requests;return;}
 unsigned op=command_of(c->property);assert(op);
 assert(p->command_count<7);p->commands[p->command_count++]=op;p->vdm_state=op;
 if(op<=3){assert(c->write_bytes==2&&c->data[0]==0xe0&&c->data[1]==0xc5);return;}
 if(op==4){
  uint8_t natural[16];reverse(natural,c->data);p->seed=10;
  for(unsigned i=0;i<10;++i)if(!memcmp(natural,p->material.seeds[i],16))p->seed=i;
  assert(p->seed<10);return;
 }
 if(op==5){
  uint8_t input[20],digest[32];reverse(p->nonce,c->data);memcpy(input,p->nonce,16);
  input[16]=0xa1;input[17]=0xb2;input[18]=0xc3;input[19]=0xd4;
  hmac(p,p->material.adapter_keys[p->seed],32,input,20,digest);reverse(p->response,digest);
  if(p->mismatch)p->response[0]^=1;
  return;
 }
 assert(np_le32(c->data)==(unsigned)!p->mismatch);
}
static void run(Peer *p,NpAuth *a,NpAuthFacts *f)
{
 for(unsigned iteration=0;iteration<400;++iteration){
  f->now_ms+=20;NpAuthCommand c={0};int r=np_auth_step(a,f,&c);
  if(!r){
   if(a->state==NP_AUTH_DONE||a->state==NP_AUTH_FAILED||a->state==NP_AUTH_STOPPED)return;
   continue;
  }
  assert(r==1);uint8_t reply[16]={0};size_t bytes=c.reply_bytes;
  if(c.write){
   assert(np_auth_write_allowed(a,&c,f->now_ms,c.data,c.write_bytes));
   NpAuthCommand altered=c;altered.data[0]^=1;
   assert(!np_auth_write_allowed(a,&altered,f->now_ms,altered.data,altered.write_bytes));
   altered=c;altered.write_bytes=1;
   assert(!np_auth_write_allowed(a,&altered,f->now_ms,altered.data,1));
  }
  assert(np_auth_sent(a,f->now_ms+1)==0);
  assert(np_auth_sent(a,f->now_ms+1)==-1);
  if(c.write){
   assert(!np_auth_write_allowed(a,&c,f->now_ms+2,c.data,c.write_bytes));
   accept_write(p,&c);
   if(c.property==0x2100d&&p->lost_role_reply){
    assert(np_auth_complete(a,&c,-1,0,0,f->now_ms+5)==-1);continue;
   }
   if(c.property==0x21007&&p->lost_nonce_reply){
    assert(np_auth_complete(a,&c,-1,0,0,f->now_ms+5)==-1);continue;
   }
   if(c.property==0x21007&&p->source_change){
    ++f->attachment;f->real_type=1;np_auth_source(a,f->epoch,f->attachment);
    assert(np_auth_complete(a,&c,0,reply,bytes,f->now_ms+5)==-1);continue;
   }
  }else{
   switch(c.property){
   case 0x21012:assert(p->process==1&&p->data_role==2);np_put32(reply,p->wrong_svid?0x1234:0x2717);break;
   case 0x2100d:np_put32(reply,p->data_role);break;
   case 0x2100f:np_put32(reply,0xa1b2c3d4);break;
   case 0x21002:np_put32(reply,p->process);break;
   case 0x2100b:np_put32(reply,p->verified);break;
   case 0x21015:np_put32(reply,p->vdm_state);break;
   case 0x21007:memcpy(reply,p->response,16);break;
   default:assert(0);
   }
  }
  (void)np_auth_complete(a,&c,0,reply,bytes,f->now_ms+5);
 }
 assert(!"Authentication model exceeded bound");
}
static unsigned deadline_and_clear_cases(void)
{
 unsigned passed=0;
 for(unsigned timing=0;timing<2;++timing) {
  Peer peer;prepare(&peer);NpAuthOps io={&peer,random_bytes,hmac,battery};NpAuth a;
  np_auth_init(&a,&peer.material,&io,1);a.epoch=a.attachment=1;
  a.state=NP_AUTH_POLL;a.poll_until=500;a.poll_at=0;
  NpAuthFacts f={1,1,400,1,10,1,1};NpAuthCommand c;uint8_t reply[4];np_put32(reply,1);
  assert(np_auth_step(&a,&f,&c)==1&&np_auth_sent(&a,400)==0);
  int result=np_auth_complete(&a,&c,0,reply,4,timing?800:490);
  if(timing)assert(result==-1&&a.error==NP_AUTH_TIMEOUT&&a.state==NP_AUTH_FAILED&&a.index==0);
  else assert(!result&&a.state==NP_AUTH_COMMAND&&a.index==1);
 }
 ++passed;
 for(unsigned scenario=0;scenario<4;++scenario) {
  const unsigned delayed_zero=scenario&1,initial_ack_lost=scenario>>1;
  Peer peer;prepare(&peer);NpAuthOps io={&peer,random_bytes,hmac,battery};NpAuth a;
  np_auth_init(&a,&peer.material,&io,1);a.epoch=a.attachment=1;
  a.state=initial_ack_lost?NP_AUTH_CLEAR_VERIFIED:NP_AUTH_CLEAN_PROCESS_READ;
  a.owned=initial_ack_lost?0:NP_AUTH_OWNS_PROCESS;a.stop=!initial_ack_lost;
  NpAuthFacts f={1,1,100,1,10,1,1};unsigned writes=0;
  for(unsigned iteration=0;iteration<150;++iteration) {
   NpAuthCommand c;int result=np_auth_step(&a,&f,&c);
   if(result==1) {
    assert(!np_auth_sent(&a,f.now_ms));uint8_t reply[4];
    unsigned value=delayed_zero&&f.now_ms>300?0:1;
    if(initial_ack_lost&&c.property==0x21002)value=0;
    np_put32(reply,value);
    if(c.write)++writes;
    if(initial_ack_lost&&c.write)(void)np_auth_complete(&a,&c,-1,0,0,f.now_ms+1);
    else(void)np_auth_complete(&a,&c,0,c.write?0:reply,c.write?0:4,f.now_ms+1);
   }
   if(a.state==NP_AUTH_STOPPED||a.state==NP_AUTH_FAILED)break;
   f.now_ms+=20;
  }
  assert(writes==1);
  if(delayed_zero)assert(!a.owned&&!np_auth_busy(&a));
  else assert(a.state==NP_AUTH_FAILED&&a.error==NP_AUTH_CLEANUP_ERROR&&a.owned&&
              np_auth_resume(&a)==-1&&f.now_ms<=1200);
  ++passed;
 }
 Peer peer;prepare(&peer);NpAuthOps io={&peer,random_bytes,hmac,battery};NpAuth a;
 np_auth_init(&a,&peer.material,&io,1);a.epoch=a.attachment=1;a.state=NP_AUTH_PROCESS_START;
 NpAuthFacts f={1,1,100,1,10,1,1};NpAuthCommand c;
 assert(np_auth_step(&a,&f,&c)==1&&np_auth_write_allowed(&a,&c,f.now_ms,c.data,c.write_bytes));
 np_auth_stop(&a);
 assert(!np_auth_write_allowed(&a,&c,f.now_ms,c.data,c.write_bytes));++passed;
 np_auth_init(&a,&peer.material,&io,1);a.epoch=a.attachment=1;
 a.state=NP_AUTH_CLEAN_PROCESS_WRITE;a.owned=NP_AUTH_OWNS_PROCESS;a.stop=1;
 assert(np_auth_step(&a,&f,&c)==1&&np_auth_write_allowed(&a,&c,f.now_ms,c.data,c.write_bytes));++passed;
 return passed;
}
static void start_discovery(Peer *peer,NpAuth *a,NpAuthFacts *f)
{
 NpAuthCommand c;uint8_t reply[4];
 for(unsigned i=0;i<30&&a->state!=NP_AUTH_SVID;++i){
  f->now_ms+=20;int got=np_auth_step(a,f,&c);if(!got)continue;
  assert(got==1&&np_auth_sent(a,f->now_ms)==0);
  if(c.write)accept_write(peer,&c);
  else np_put32(reply,c.property==0x21002?peer->process:c.property==0x2100b?peer->verified:peer->data_role);
  assert(!np_auth_complete(a,&c,0,c.write?0:reply,c.write?0:4,f->now_ms+5));
 }
 assert(a->state==NP_AUTH_SVID&&peer->process==1&&peer->data_role==2&&peer->role_requests==1);
}
static unsigned discovery_and_primary_cases(void)
{
 unsigned passed=0;Peer peer;prepare(&peer);NpAuthOps io={&peer,random_bytes,hmac,battery};NpAuth a;
 NpAuthFacts f={1,1,100,1,1,1,0};NpAuthCommand c;uint8_t reply[4];
 np_auth_init(&a,&peer.material,&io,1);
 assert(!np_auth_step(&a,&f,&c)&&a.battery_ok&&a.battery_checked&&a.battery_checks==1&&peer.battery_calls==1);
 assert(!a.attempts&&!a.pending&&a.state==NP_AUTH_IDLE);
 for(unsigned i=0;i<5;++i){f.now_ms+=20;assert(!np_auth_step(&a,&f,&c));}
 assert(peer.battery_calls==1);++passed;
 ++f.attachment;f.real_type=10;f.now_ms=250;
 assert(!np_auth_step(&a,&f,&c)&&!a.attempts&&!a.pending);++passed;
 f.pd_ready=1;start_discovery(&peer,&a,&f);
 f.now_ms+=20;
 assert(np_auth_step(&a,&f,&c)==1&&c.property==0x21012&&!c.write);
 assert(peer.battery_calls==1&&np_auth_sent(&a,f.now_ms)==0);
 uint64_t completed=f.now_ms+5;
 np_put32(reply,0);assert(!np_auth_complete(&a,&c,0,reply,4,completed)&&a.state==NP_AUTH_SVID&&!a.error);
 f.now_ms=completed+19;assert(!np_auth_step(&a,&f,&c));
 ++f.now_ms;assert(np_auth_step(&a,&f,&c)==1&&!c.write&&np_auth_sent(&a,f.now_ms)==0);
 np_put32(reply,0x2717);assert(!np_auth_complete(&a,&c,0,reply,4,f.now_ms+5)&&a.state==NP_AUTH_ID&&a.svid==0x2717);
 assert(a.battery_ok&&peer.battery_calls==1);++passed;
 prepare(&peer);np_auth_init(&a,&peer.material,&io,1);f=(NpAuthFacts){1,1,100,1,10,1,1};
 assert(!np_auth_step(&a,&f,&c)&&a.battery_ok);
 start_discovery(&peer,&a,&f);f.now_ms+=20;
 assert(np_auth_step(&a,&f,&c)==1&&np_auth_sent(&a,f.now_ms)==0);np_put32(reply,0);
 assert(!np_auth_complete(&a,&c,0,reply,4,f.now_ms+5));f.now_ms=a.discovery_until;
 assert(!np_auth_step(&a,&f,&c)&&a.error==NP_AUTH_TIMEOUT&&!a.verified);
 run(&peer,&a,&f);assert(a.state==NP_AUTH_FAILED&&!a.owned);
 assert(a.svid==0&&a.battery_ok&&!a.verified);++passed;
 prepare(&peer);np_auth_init(&a,&peer.material,&io,1);f=(NpAuthFacts){1,1,100,1,10,1,1};
 assert(!np_auth_step(&a,&f,&c)&&a.battery_ok);
 start_discovery(&peer,&a,&f);f.now_ms+=20;
 assert(np_auth_step(&a,&f,&c)==1&&np_auth_sent(&a,f.now_ms)==0);np_put32(reply,0);
 assert(!np_auth_complete(&a,&c,0,reply,4,f.now_ms+5));f.now_ms=a.discovery_until-20;
 assert(np_auth_step(&a,&f,&c)==1&&np_auth_sent(&a,f.now_ms)==0);np_put32(reply,0x2717);
 assert(np_auth_complete(&a,&c,0,reply,4,a.discovery_until)==-1&&a.error==NP_AUTH_TIMEOUT&&a.svid==0x2717);
 run(&peer,&a,&f);assert(a.state==NP_AUTH_FAILED&&!a.pending&&!a.owned);++passed;
 prepare(&peer);np_auth_init(&a,&peer.material,&io,1);f=(NpAuthFacts){1,1,100,1,10,1,1};
 assert(!np_auth_step(&a,&f,&c)&&a.battery_ok);
 start_discovery(&peer,&a,&f);f.now_ms+=20;
 assert(np_auth_step(&a,&f,&c)==1&&np_auth_sent(&a,f.now_ms)==0);np_put32(reply,0x1234);
 assert(np_auth_complete(&a,&c,0,reply,4,f.now_ms+5)==-1&&a.error==NP_AUTH_PROTOCOL&&a.svid==0x1234);
 run(&peer,&a,&f);
 assert(a.battery_ok&&!a.verified&&!a.owned);++passed;
 return passed;
}
int main(void)
{
 unsigned passed=0;
 for(unsigned scenario=0;scenario<7;++scenario){
  Peer peer;prepare(&peer);NpAuthMaterial material=peer.material;
  peer.fail_battery=scenario==1;peer.wrong_svid=scenario==2;peer.mismatch=scenario==3;
  peer.lost_nonce_reply=scenario==4;peer.source_change=scenario==5;
  if(scenario==6)material.battery_key[0]^=1;
  NpAuthOps io={&peer,random_bytes,hmac,battery};NpAuth a;np_auth_init(&a,&material,&io,1);
  NpAuthFacts f={1,1,100,1,10,1,1};run(&peer,&a,&f);
  if(scenario==0){
   const unsigned expected[]={1,2,3,4,5,7,6};assert(peer.command_count==7&&!memcmp(peer.commands,expected,sizeof(expected)));
   assert(a.verified&&a.battery_ok&&a.hash_ok&&peer.verified==1&&peer.process==0);
   assert(a.owned==NP_AUTH_OWNS_VERIFIED&&!np_auth_busy(&a));
   np_auth_stop(&a);assert(!a.verified);run(&peer,&a,&f);
   assert(a.state==NP_AUTH_STOPPED&&!a.error&&!a.owned&&!np_auth_busy(&a));
   assert(peer.verified==0&&peer.process==0&&peer.writes[7]==1);
   assert(np_auth_resume(&a)==0);NpAuthCommand c;assert(np_auth_step(&a,&f,&c)==0);
  }else{
   assert(!a.verified&&!peer.verified&&!peer.process&&!a.owned&&!peer.published_true);
   if(scenario==1||scenario==2||scenario==6)assert(!peer.command_count);
   if(scenario==3)assert(a.error==NP_AUTH_MISMATCH&&peer.command_count==7);
   if(scenario==4||scenario==5)assert(peer.writes[7]==1);
   unsigned old_calls=peer.battery_calls;NpAuthCommand c;
   for(unsigned i=0;i<10;++i){f.now_ms+=20;assert(np_auth_step(&a,&f,&c)==0);}
   assert(peer.battery_calls==old_calls);
  }
  ++passed;
 }
 Peer peer;prepare(&peer);NpAuthOps io={&peer,random_bytes,hmac,battery};NpAuth a;
 peer.material.magic[0]^=1;np_auth_init(&a,&peer.material,&io,1);assert(!a.enabled&&a.error==NP_AUTH_UNAVAILABLE);++passed;
 passed+=deadline_and_clear_cases();
 passed+=discovery_and_primary_cases();
 /* A queued DR request is not a completed swap; loss never replays it or
  * reaches vendor challenges. Already-DFP needs no duplicate request. */
 for(unsigned scenario=0;scenario<3;++scenario){
  prepare(&peer);peer.no_role_response=scenario==0;peer.lost_role_reply=scenario==1;
  if(scenario==2)peer.data_role=2;
  np_auth_init(&a,&peer.material,&io,1);NpAuthFacts f={1,1,100,1,10,1,1};run(&peer,&a,&f);
  if(scenario<2)assert(!a.verified&&!a.owned&&!peer.process&&!peer.command_count&&peer.role_requests==1);
  else assert(a.verified&&peer.role_requests==0&&peer.command_count==7);
  ++passed;
 }
 printf("auth host-only cases passed=%u; no phone/I2C/USB\n",passed);
 return 0;
}
