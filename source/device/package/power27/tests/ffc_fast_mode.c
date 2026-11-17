/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaGaugeBus.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned test,writes,waits,opens,closes;static uint8_t frames[4][8];static unsigned lengths[4];
static int acquire(void *x){(void)x;return 0;}static void release(void *x,int stopped){(void)x;assert(stopped);}
int np_gpi_open(NpGpi *g,uint64_t base,uint32_t size,uint32_t ee,uint32_t id,uint32_t se,uint32_t protocol){assert(base==0xa00000&&size==0x60000&&ee==0x10000&&id==1&&se==5&&protocol==3);g->open=g->owned=1;opens++;return 0;}
int np_gpi_close(NpGpi *g){g->open=g->owned=0;closes++;return 0;}
int np_gpi_i2c(NpGpi *g,const PgpiI2cClock *clock,uint8_t addr,const uint8_t *p,unsigned n,uint8_t *out,unsigned length){(void)clock;(void)out;assert(g->open&&addr==0x55&&!length&&n<8&&writes<4);memcpy(frames[writes],p,n);lengths[writes]=n;writes++;return((test==3||test==9)&&writes==1)||((test==4||test==10)&&writes==2)?EIO:0;}
static void waitms(void *x,uint32_t ms){(void)x;assert(ms==100);waits++;}
static void frame(unsigned at,unsigned request){const uint8_t expected[5]={0x3e,(uint8_t)(request?0x3e:0x3f),0,(uint8_t)request,0};assert(lengths[at]==5&&lengths[at+1]==3&&!memcmp(frames[at],expected,5)&&!memcmp(frames[at+1],"\x60\xc0\x06",3));}
int main(int argc,char **argv){assert(argc==2);test=(unsigned)atoi(argv[1]);assert(test<12);NpGaugeBus b={0};b.acquire=acquire;b.release=release;b.gpii=1;b.clock=(PgpiI2cClock){2,12,11,22,0};b.auth_qualified=1;b.auth_wait_ms=waitms;b.fastmode_valid=test!=6&&test!=8&&test!=9&&test!=10&&test!=11;b.fastmode=test==1||test==5;NpFastModeReading r={0};unsigned request=test==1||test>=8?0:1;int result=np_gauge_bus_fast_mode(&b,request,test==2||test==11,&r);
 if(test==2||test==5||test==11)assert(!result&&!r.attempted&&!writes&&!waits);
 else if(test==6)assert(result==EACCES&&!writes&&!r.valid);
 else if(test==3||test==4){assert(result==EIO&&b.fastmode_failed&&b.fastmode_owned&&!r.valid);unsigned prior=writes;assert(np_gauge_bus_fast_mode(&b,1,0,&r)==EACCES&&writes==prior);assert(!np_gauge_bus_fast_mode(&b,0,0,&r)&&r.valid&&!r.enabled&&!r.owned&&writes==prior+2);frame(prior,0);assert(np_gauge_bus_fast_mode(&b,1,0,&r)==EACCES&&writes==prior+2);}
 else if(test==9||test==10){assert(result==EIO&&b.fastmode_owned&&b.fastmode_disable_attempted&&!r.valid);unsigned prior=writes;assert(np_gauge_bus_fast_mode(&b,0,0,&r)==EACCES&&writes==prior);}
 else{assert(!result&&r.attempted&&r.valid&&r.enabled==request&&r.owned==request&&writes==2&&waits==1);frame(0,request);assert(!np_gauge_bus_fast_mode(&b,request,0,&r)&&!r.attempted&&writes==2);}
 assert(opens==closes);printf("PASS volatileFFC case%u writes%u wait%u mode_valid%u owned%u\n",test,writes,waits,b.fastmode_valid,b.fastmode_owned);return 0;}
