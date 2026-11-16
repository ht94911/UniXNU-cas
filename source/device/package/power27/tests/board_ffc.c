/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "../platform/NezhaPowerBoard.c"
static unsigned scenario,calls;
static int lease(void *context,uint64_t id){(void)context;assert(id==1);return scenario!=1&&!(scenario==5&&calls);}
int np_gauge_bus_fast_mode(NpGaugeBus *b,unsigned enabled,unsigned pending,NpFastModeReading *r){
 assert(b&&enabled==1&&!pending);++calls;r->attempted=r->valid=r->enabled=r->owned=1;return scenario==4?EIO:0;
}
int main(int argc,char **argv){
 assert(argc==2);scenario=(unsigned)strtoul(argv[1],0,10);assert(scenario<6);
 NpPowerBoard b={0};b.resources.lease=1;b.resources.lease_valid=lease;b.resources.enable_auth=1;
 if(scenario==2)b.suspend=1;
 if(scenario==3)b.resources.enable_auth=0;
 NpFastModeReading r={1,1,1,1};int result=fast_mode(&b,1,0,&r);
 if(scenario>=1&&scenario<=3)assert(result==EACCES&&!calls&&!r.valid&&!r.attempted);
 else if(scenario==5)assert(result==EIO&&calls==1&&!r.valid&&r.owned);
 else assert(calls==1&&r.owned&&result==(scenario==4?EIO:0));
 printf("PASS sole board FFC callback%u calls%u result%d\n",scenario,calls,result);return 0;
}
