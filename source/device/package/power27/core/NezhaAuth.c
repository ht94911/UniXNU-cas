/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Current OS4's typed PD setters and observed VDM/HMAC contract. The original
 * protocol/transport and the selected private material remain separate. */
#include "NezhaAuth.h"
#include "NezhaMca.h"
#include <string.h>

#define PROCESS 0x21002u
#define VERIFIED 0x2100bu
#define SVID 0x21012u
#define ID 0x2100fu
#define VDM_STATE 0x21015u
static const uint8_t sequence[]={1,2,3,4,5,7,6};
static const uint16_t budgets[]={500,500,1000,1500,2500,1000,500};
static const uint32_t properties[]={0x21003,0x21004,0x21005,0x21006,0x21007,0x21009,0x21008};
static void erase(void *p,size_t n) { volatile uint8_t *b=p;while(n--)*b++=0; }
static int equal(const uint8_t *a,const uint8_t *b,size_t n)
{ unsigned different=0;for(size_t i=0;i<n;++i)different|=a[i]^b[i];return !different; }
static void reverse_words(uint8_t out[16],const uint8_t in[16])
{ for(unsigned i=0;i<16;i+=4)for(unsigned j=0;j<4;++j)out[i+j]=in[i+3-j]; }
int np_auth_material_valid(const void *p,size_t n)
{
 const NpAuthMaterial *m=p;
 return m&&n==NP_AUTH_MATERIAL_BYTES&&sizeof(*m)==n&&
   !memcmp(m->magic,"NP18AUTH",8)&&np_le32(m->version)==1&&np_le32(m->fg_selector)==2;
}
void np_auth_init(NpAuth *a,const NpAuthMaterial *m,const NpAuthOps *io,int enable)
{
 memset(a,0,sizeof(*a));a->material=m;
 if(io)a->io=*io;
 a->enabled=enable&&np_auth_material_valid(m,sizeof(*m))&&io&&
   io->random_bytes&&io->hmac_sha256&&io->battery_digest;
 if(enable&&!a->enabled)a->error=NP_AUTH_UNAVAILABLE;
}
static int cleaning(const NpAuth *a)
{ return a->state>=NP_AUTH_CLEAN_VERIFIED_READ&&a->state<=NP_AUTH_CLEAN_PROCESS_WRITE; }
static void fail(NpAuth *a,unsigned error)
{
 if(!a->error)a->error=error;
 a->verified=0;a->hash_ok=0;erase(a->expected,sizeof(a->expected));
 erase(a->challenge,sizeof(a->challenge));erase(a->command.data,sizeof(a->command.data));a->pending=0;
 a->state=a->owned?NP_AUTH_CLEAN_VERIFIED_READ:NP_AUTH_FAILED;
}
void np_auth_stop(NpAuth *a) { if(a){a->stop=1;a->verified=0;} }
void np_auth_source(NpAuth *a,uint64_t epoch,uint64_t attachment)
{
 if(!a||(a->epoch==epoch&&a->attachment==attachment))return;
 a->epoch=epoch;a->attachment=attachment;a->verified=0;
 if(a->state!=NP_AUTH_IDLE)fail(a,NP_AUTH_SOURCE);
}
int np_auth_busy(const NpAuth *a)
{ return a&&(a->pending||(a->owned&&(a->state!=NP_AUTH_DONE||a->stop))||
   (a->state>NP_AUTH_IDLE&&a->state<NP_AUTH_DONE)||cleaning(a)); }
int np_auth_resume(NpAuth *a)
{
 if(!a||a->pending||a->owned)return -1;
 a->stop=0;a->state=NP_AUTH_IDLE;a->verified=a->battery_ok=a->hash_ok=a->battery_checked=0;return 0;
}
static void verify_battery(NpAuth *a)
{
 uint8_t nonce[32]={0},actual[32]={0},expected[32]={0};
 int error=a->io.random_bytes(a->io.context,nonce,sizeof(nonce));
 if(!error)error=a->io.battery_digest(a->io.context,nonce,actual);
 if(!error)error=a->io.hmac_sha256(a->io.context,a->material->battery_key,32,nonce,32,expected);
 a->battery_checked=1;++a->battery_checks;
 a->battery_ok=!error&&equal(actual,expected,32);
 a->battery_error=a->battery_ok?NP_AUTH_NO_ERROR:error?NP_AUTH_BATTERY_ERROR:NP_AUTH_MISMATCH;
 erase(nonce,sizeof(nonce));erase(actual,sizeof(actual));erase(expected,sizeof(expected));
 if(!a->battery_ok)fail(a,a->battery_error);
}
static int emit(NpAuth *a,const NpAuthFacts *f,NpAuthCommand *out,uint32_t prop,
                const uint8_t *data,unsigned size,unsigned reply)
{
 if(a->next_id==UINT64_MAX||f->now_ms>UINT64_MAX-1000){fail(a,NP_AUTH_TIMEOUT);return -1;}
 NpAuthCommand c={0};c.id=++a->next_id;c.epoch=f->epoch;c.attachment=f->attachment;
 c.deadline_ms=f->now_ms+1000;c.property=prop;c.write=!!size;
 c.write_bytes=(uint8_t)size;c.reply_bytes=(uint8_t)reply;
 if(size)memcpy(c.data,data,size);
 a->command=c;a->pending=1;a->sent=0;*out=c;return 1;
}
static int scalar(NpAuth *a,const NpAuthFacts *f,NpAuthCommand *out,uint32_t prop,unsigned write,uint32_t value)
{ uint8_t data[4];np_put32(data,value);return emit(a,f,out,prop,write?data:0,write?4:0,write?0:4); }
int np_auth_step(NpAuth *a,const NpAuthFacts *f,NpAuthCommand *out)
{
 if(!a||!f||!out||!a->enabled)return 0;
 if(a->epoch!=f->epoch||a->attachment!=f->attachment)np_auth_source(a,f->epoch,f->attachment);
 if(a->pending)return 0;
 if(cleaning(a)&&(a->state==NP_AUTH_CLEAN_VERIFIED_READ||a->state==NP_AUTH_CLEAN_PROCESS_READ)) {
  unsigned i=a->state==NP_AUTH_CLEAN_PROCESS_READ;
  if(a->clear_pending&(1u<<i)) {
   if(f->now_ms>=a->clear_deadline[i]){a->error=NP_AUTH_CLEANUP_ERROR;a->state=NP_AUTH_FAILED;return -1;}
   if(f->now_ms<a->poll_at)return 0;
  }
 }
 if(a->stop&&a->state!=NP_AUTH_FAILED&&a->state!=NP_AUTH_STOPPED&&!cleaning(a)){
  fail(a,NP_AUTH_NO_ERROR);if(!a->owned)a->state=NP_AUTH_STOPPED;
 }
 // Gauge identity belongs to this driver's resource lifetime, independently
 // from adapter discovery. A PC source can verify the real primary battery;
 // an unavailable or foreign SVID cannot skip that check.
 if(!a->stop&&!a->battery_checked&&!a->owned&&a->state==NP_AUTH_IDLE&&f->sample_valid) {
  verify_battery(a);
  return 0; /* The blocking bus wait requires new facts before a wire deadline. */
 }
 if(a->battery_checked&&!a->battery_ok)return 0;
 if(!cleaning(a)&&a->state!=NP_AUTH_DONE&&a->state!=NP_AUTH_FAILED&&a->state!=NP_AUTH_STOPPED&&
    (!f->sample_valid||!f->online||f->real_type!=10)){
  if(a->state!=NP_AUTH_IDLE)fail(a,NP_AUTH_SOURCE);
  return 0;
 }
 if(!f->epoch)return 0;
 if(a->state==NP_AUTH_IDLE||a->state==NP_AUTH_FAILED) {
  if(a->stop||a->owned||!a->battery_ok||!f->sample_valid||!f->online||f->real_type!=10||
     (a->last_epoch==f->epoch&&a->last_attachment==f->attachment))return 0;
  a->last_epoch=f->epoch;a->last_attachment=f->attachment;++a->attempts;
  a->error=0;a->index=0;a->svid=a->adapter_id=0;a->state=NP_AUTH_SVID;
  a->discovery_until=f->now_ms+3000;a->poll_at=f->now_ms;
 }
 switch(a->state) {
 case NP_AUTH_SVID:
  if(f->now_ms>=a->discovery_until){fail(a,NP_AUTH_TIMEOUT);return 0;}
  if(f->now_ms<a->poll_at)return 0;
  return scalar(a,f,out,SVID,0,0);
 case NP_AUTH_ID:return scalar(a,f,out,ID,0,0);
 case NP_AUTH_PROCESS_CHECK:return scalar(a,f,out,PROCESS,0,0);
 case NP_AUTH_BATTERY: {
  uint8_t seed=0;
  int error=a->io.random_bytes(a->io.context,&seed,1);
  if(!error)error=a->io.random_bytes(a->io.context,a->challenge,16);
  if(error){fail(a,NP_AUTH_CRYPTO_ERROR);return 0;}
  a->seed_index=seed%10;
  uint8_t input[20];memcpy(input,a->challenge,16);
  input[16]=(uint8_t)(a->adapter_id>>24);input[17]=(uint8_t)(a->adapter_id>>16);
  input[18]=(uint8_t)(a->adapter_id>>8);input[19]=(uint8_t)a->adapter_id;
  error=a->io.hmac_sha256(a->io.context,a->material->adapter_keys[a->seed_index],32,input,20,a->expected);
  erase(input,sizeof(input));
  if(error){fail(a,NP_AUTH_CRYPTO_ERROR);return 0;}
  a->state=NP_AUTH_CLEAR_VERIFIED;return 0;
 }
 case NP_AUTH_CLEAR_VERIFIED:return scalar(a,f,out,VERIFIED,1,0);
 case NP_AUTH_VERIFY_CLEAR:return scalar(a,f,out,VERIFIED,0,0);
 case NP_AUTH_PROCESS_START:return scalar(a,f,out,PROCESS,1,1);
 case NP_AUTH_VERIFY_START:return scalar(a,f,out,PROCESS,0,0);
 case NP_AUTH_COMMAND: {
  uint8_t data[16]={0};unsigned command=sequence[a->index],size=4;
  if(command<=3){data[0]=0xe0;data[1]=0xc5;size=2;} /* observed stock "null" parser */
  else if(command==4){reverse_words(data,a->material->seeds[a->seed_index]);size=16;}
  else if(command==5){reverse_words(data,a->challenge);size=16;}
  else np_put32(data,a->hash_ok&&a->battery_ok);
  int result=emit(a,f,out,properties[a->index],data,size,0);erase(data,sizeof(data));return result;
 }
 case NP_AUTH_POLL:
  if(f->now_ms>=a->poll_until){fail(a,NP_AUTH_TIMEOUT);return 0;}
  if(f->now_ms<a->poll_at)return 0;
  return scalar(a,f,out,VDM_STATE,0,0);
 case NP_AUTH_RESPONSE:return emit(a,f,out,0x21007,0,0,16);
 case NP_AUTH_PUBLISH:return scalar(a,f,out,VERIFIED,1,a->hash_ok&&a->battery_ok);
 case NP_AUTH_VERIFY_PUBLISH:return scalar(a,f,out,VERIFIED,0,0);
 case NP_AUTH_PROCESS_END:return scalar(a,f,out,PROCESS,1,0);
 case NP_AUTH_VERIFY_END:return scalar(a,f,out,PROCESS,0,0);
 case NP_AUTH_CLEAN_VERIFIED_READ:return scalar(a,f,out,VERIFIED,0,0);
 case NP_AUTH_CLEAN_VERIFIED_WRITE:return scalar(a,f,out,VERIFIED,1,0);
 case NP_AUTH_CLEAN_PROCESS_READ:return scalar(a,f,out,PROCESS,0,0);
 case NP_AUTH_CLEAN_PROCESS_WRITE:return scalar(a,f,out,PROCESS,1,0);
 default:return 0;
 }
}
int np_auth_sent(NpAuth *a,uint64_t now)
{
 if(!a||!a->pending||a->sent||now>=a->command.deadline_ms)return -1;
 a->sent=1;
 if(a->command.write&&(a->command.property==VERIFIED||a->command.property==PROCESS)&&
    np_le32(a->command.data)==0) {
  unsigned i=a->command.property==PROCESS;
  a->clear_pending|=(uint8_t)(1u<<i);a->clear_deadline[i]=a->command.deadline_ms;
 }
 if(a->command.write&&a->command.property==PROCESS&&np_le32(a->command.data)==1)a->owned|=NP_AUTH_OWNS_PROCESS;
 if(a->command.write&&a->command.property==VERIFIED)a->owned|=NP_AUTH_OWNS_VERIFIED;
 if(a->state==NP_AUTH_COMMAND){
  if(now>UINT64_MAX-budgets[a->index])return -1;
  a->poll_until=now+budgets[a->index];
 }
 return 0;
}
static void next_command(NpAuth *a)
{
 if(++a->index==sizeof(sequence))a->state=NP_AUTH_PUBLISH;
 else a->state=NP_AUTH_COMMAND;
}
int np_auth_complete(NpAuth *a,const NpAuthCommand *c,int error,const uint8_t *data,size_t bytes,uint64_t now)
{
 if(!a||!c||!a->pending||c->id!=a->command.id||c->epoch!=a->epoch||
    c->attachment!=a->attachment||c->property!=a->command.property||
    c->write!=a->command.write||c->write_bytes!=a->command.write_bytes||
    c->reply_bytes!=a->command.reply_bytes)return -1;
 a->pending=0;
 if(!error&&a->state==NP_AUTH_POLL&&now>=a->poll_until){fail(a,NP_AUTH_TIMEOUT);return -1;}
 if(!error&&(!a->sent||now>=a->command.deadline_ms||bytes<c->reply_bytes||(c->reply_bytes&&!data)))error=-1;
 if(error){if(cleaning(a)){a->error=NP_AUTH_CLEANUP_ERROR;a->state=NP_AUTH_FAILED;}else fail(a,NP_AUTH_TRANSPORT);return -1;}
 uint32_t value=c->reply_bytes>=4?np_le32(data):0;
 switch(a->state) {
 case NP_AUTH_SVID:
  a->svid=value;
  if(now>=a->discovery_until){fail(a,NP_AUTH_TIMEOUT);return -1;}
  if(!value){a->poll_at=now+20;break;}
  if(value!=0x2717){fail(a,NP_AUTH_PROTOCOL);return -1;}
  a->state=NP_AUTH_ID;break;
 case NP_AUTH_ID:
  a->adapter_id=value;
  if(!value){fail(a,NP_AUTH_PROTOCOL);return -1;}
  a->state=NP_AUTH_PROCESS_CHECK;break;
 case NP_AUTH_PROCESS_CHECK:
  if(value){fail(a,NP_AUTH_PROTOCOL);return -1;}
  a->state=NP_AUTH_BATTERY;break;
 case NP_AUTH_CLEAR_VERIFIED:a->state=NP_AUTH_VERIFY_CLEAR;break;
 case NP_AUTH_VERIFY_CLEAR:
  if(value){fail(a,NP_AUTH_PROTOCOL);return -1;}
  a->owned&=~NP_AUTH_OWNS_VERIFIED;a->clear_pending&=~1u;a->state=NP_AUTH_PROCESS_START;break;
 case NP_AUTH_PROCESS_START:a->state=NP_AUTH_VERIFY_START;break;
 case NP_AUTH_VERIFY_START:
  if(value!=1){fail(a,NP_AUTH_PROTOCOL);return -1;}
  a->state=NP_AUTH_COMMAND;break;
 case NP_AUTH_COMMAND:
  if(now>=a->poll_until){fail(a,NP_AUTH_TIMEOUT);return -1;}
  a->poll_at=now+10;a->state=NP_AUTH_POLL;break;
 case NP_AUTH_POLL:
  if(value==sequence[a->index]+100u){fail(a,NP_AUTH_PROTOCOL);return -1;}
  if(value!=sequence[a->index]){a->poll_at=now+10;break;}
  if(sequence[a->index]==5)a->state=NP_AUTH_RESPONSE;
  else next_command(a);
  break;
 case NP_AUTH_RESPONSE: {
  uint8_t natural[16];reverse_words(natural,data);a->hash_ok=equal(natural,a->expected,16);
  erase(natural,sizeof(natural));erase(a->expected,sizeof(a->expected));erase(a->challenge,sizeof(a->challenge));
  if(!a->hash_ok)a->error=NP_AUTH_MISMATCH;
  next_command(a);break;
 }
 case NP_AUTH_PUBLISH:a->state=NP_AUTH_VERIFY_PUBLISH;break;
 case NP_AUTH_VERIFY_PUBLISH:
  if(value!=(unsigned)(a->hash_ok&&a->battery_ok)){fail(a,NP_AUTH_PROTOCOL);return -1;}
  if(!value)a->owned&=~NP_AUTH_OWNS_VERIFIED;
  a->state=NP_AUTH_PROCESS_END;break;
 case NP_AUTH_PROCESS_END:a->state=NP_AUTH_VERIFY_END;break;
 case NP_AUTH_VERIFY_END:
  if(value){fail(a,NP_AUTH_PROTOCOL);return -1;}
  a->owned&=~NP_AUTH_OWNS_PROCESS;a->clear_pending&=~2u;a->verified=a->hash_ok&&a->battery_ok;
  a->verified_ms=now;a->state=NP_AUTH_DONE;break;
 case NP_AUTH_CLEAN_VERIFIED_READ:
  if(value>1){a->error=NP_AUTH_CLEANUP_ERROR;a->state=NP_AUTH_FAILED;return -1;}
  if(value) {
   if(a->clear_pending&1u)a->poll_at=now+20;
   else a->state=NP_AUTH_CLEAN_VERIFIED_WRITE;
  } else{a->owned&=~NP_AUTH_OWNS_VERIFIED;a->clear_pending&=~1u;a->state=NP_AUTH_CLEAN_PROCESS_READ;}
  break;
 case NP_AUTH_CLEAN_VERIFIED_WRITE:a->state=NP_AUTH_CLEAN_VERIFIED_READ;break;
 case NP_AUTH_CLEAN_PROCESS_READ:
  if(value>1){a->error=NP_AUTH_CLEANUP_ERROR;a->state=NP_AUTH_FAILED;return -1;}
  if(value) {
   if(a->clear_pending&2u)a->poll_at=now+20;
   else a->state=NP_AUTH_CLEAN_PROCESS_WRITE;
  } else{a->owned=0;a->clear_pending&=~2u;a->state=a->stop&&!a->error?NP_AUTH_STOPPED:NP_AUTH_FAILED;}
  break;
 case NP_AUTH_CLEAN_PROCESS_WRITE:a->state=NP_AUTH_CLEAN_PROCESS_READ;break;
 default:return -1;
 }
 erase(a->command.data,sizeof(a->command.data));return 0;
}
int np_auth_write_allowed(const NpAuth *a,const NpAuthCommand *c,uint64_t now,const uint8_t *data,size_t bytes)
{
 if(!a||!c||!a->enabled||!a->pending||a->sent||!c->write||now>=a->command.deadline_ms||
    c->id!=a->command.id||c->epoch!=a->epoch||c->attachment!=a->attachment||
    c->property!=a->command.property||c->write!=a->command.write||
    c->write_bytes!=a->command.write_bytes||c->reply_bytes!=a->command.reply_bytes||
    bytes!=a->command.write_bytes||!data||!equal(c->data,a->command.data,bytes)||
    !equal(data,a->command.data,bytes))return 0;
 if(a->stop&&!(bytes==4&&(c->property==PROCESS||c->property==VERIFIED)&&np_le32(data)==0))return 0;
 if(c->property==VERIFIED&&np_le32(data)==1&&(!a->hash_ok||!a->battery_ok||a->state!=NP_AUTH_PUBLISH))return 0;
 return 1;
}
