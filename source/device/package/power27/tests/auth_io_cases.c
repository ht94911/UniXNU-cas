/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Executes the production volatile MAC reader with a synthetic I2C peer. */
#include "NezhaGaugeBus.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static unsigned writes,reads,waits,closed,fail_write,fail_read;
static unsigned identity_case,identity_mode,selector;
static int acquire(void *p){(void)p;return 0;}
static void release(void *p,int stopped){(void)p;assert(stopped);}
static void wait_ms(void *p,uint32_t n){(void)p;assert(n==400);++waits;}
static void wait_us(void *p,uint32_t n){(void)p;assert(n==4000);++waits;}
int np_gpi_open(NpGpi *g,uint64_t pa,uint32_t n,uint32_t ee,uint32_t i,uint32_t se,uint32_t proto)
{assert(pa==0xa00000&&n==0x60000&&ee==0x10000&&i==1&&se==5&&proto==3);g->open=g->owned=true;return 0;}
int np_gpi_close(NpGpi *g){g->open=g->owned=false;++closed;return 0;}
int np_gpi_i2c(NpGpi *g,const PgpiI2cClock *c,uint8_t addr,const uint8_t *w,unsigned nw,uint8_t *r,unsigned nr)
{
 (void)c;assert(g->open&&addr==0x55);
 if(identity_case){
  if(!nr){assert(nw==3&&w[0]==0x3e&&w[2]==0&&(w[1]==0x4a||w[1]==0x4b));
   assert(w[1]==(writes?0x4b:0x4a));selector=w[1];++writes;return identity_mode==1?EIO:0;}
  assert(nw==1&&w[0]==0x3e&&nr==36&&waits==writes&&reads+1==writes);++reads;
  if(identity_mode==2)return EIO;
  memset(r,0,nr);r[0]=(uint8_t)selector;
  if(selector==0x4a)memcpy(r+2,"fg@BM6N#rev",11);
  else {r[2]='X';r[3]='F';}
  r[35]=36;
  if(identity_mode==3)r[0]^=1;
  if(identity_mode==4)r[35]=37;
  if(identity_mode==5)r[35]=4;
  if(identity_mode==7&&selector==0x4a){memset(r+2,0,16);memcpy(r+2,"BM6N",4);}
  unsigned sum=0;for(unsigned i=0;i<34;++i)sum+=r[i];r[34]=(uint8_t)~sum;
  if(identity_mode==6)r[34]^=1;
  return 0;
 }
 if(nr){assert(nw==1&&w[0]==0x40&&nr==32&&waits==1&&writes==1);++reads;
  if(fail_read)return EIO;
  for(unsigned i=0;i<32;++i)r[i]=(uint8_t)(i^0x5a);
  return 0;
 }
 assert(nw==37&&w[0]==0x3e&&w[1]==0&&w[2]==0&&w[36]==0x24);
 unsigned sum=0;for(unsigned i=0;i<32;++i){assert(w[i+3]==i);sum+=i;}
 assert(w[35]==(uint8_t)~sum);++writes;return fail_write?EIO:0;
}
int main(void)
{
 for(unsigned mode=0;mode<4;++mode){
  NpGaugeBus b={0};b.acquire=acquire;b.release=release;b.gpii=1;
  b.clock=(PgpiI2cClock){2,12,11,22,0};b.auth_qualified=1;b.auth_wait_ms=wait_ms;
  uint8_t nonce[32],out[32];for(unsigned i=0;i<32;++i)nonce[i]=(uint8_t)i;
  memset(out,0xaa,sizeof(out));writes=reads=waits=closed=0;fail_write=mode==1;fail_read=mode==2;
  if(mode==3)b.auth_wait_ms=0;
  int result=np_gauge_bus_auth_digest(&b,nonce,out);
  if(!mode){assert(!result&&writes==1&&reads==1&&waits==1&&closed==1);for(unsigned i=0;i<32;++i)assert(out[i]==(uint8_t)(i^0x5a));}
  else{
   assert(result==(mode==3?EACCES:EIO));for(unsigned i=0;i<32;++i)assert(!out[i]);
   assert(writes==(mode==3?0u:1u));assert(waits==(mode==2?1u:0u));assert(reads==(mode==2?1u:0u));
  }
 }
 puts("PASS 4 host-only volatile MAC cases: exact37B/400ms/32B; failures do not retry");
 for(identity_mode=0;identity_mode<8;++identity_mode){
  identity_case=1;writes=reads=waits=closed=0;
  NpGaugeBus b={0};b.acquire=acquire;b.release=release;b.gpii=1;
  b.clock=(PgpiI2cClock){2,12,11,22,0};b.pause_us=wait_us;
  NpGaugeIdentity out;memset(&out,0xaa,sizeof(out));
  int result=np_gauge_bus_identity(&b,&out);
  if(!identity_mode)assert(!result&&out.valid==3&&out.cell_code=='F'&&!strcmp(out.device_name,"BM6N")&&writes==2&&reads==2&&waits==2);
  else{assert(result==EIO&&!out.valid&&out.error==EIO&&!out.device_name[0]);
   assert(writes==((identity_mode==7)?2u:1u));assert(reads==((identity_mode==1)?0u:(identity_mode==7)?2u:1u));}
  assert(closed==1);
 }
 puts("PASS 8 actual identity-bus cases: MAC4a/4b, echoed command/length/CRC, parsed name, cleanup and no write retry");
 return 0;
}
