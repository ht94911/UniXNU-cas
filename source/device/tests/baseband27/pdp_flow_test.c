/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/bridge/NbPdpFlow.h"
static void u32(uint8_t *p, uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
int main(void)
{
    struct nb_pdp_flow flows = {0};uint8_t bytes[9];uint64_t in[2]={0},out[2]={0};uint32_t count;
    for(unsigned cls=0;cls<NB_PDP_FLOW_CLASSES;cls++){
        in[0]=cls;count=1;assert(!nb_pdp_flow_call(&flows,2,in,1,0,0,out,&count)&&out[0]==0&&count==1);
        uint32_t value=0x12340000+cls;u32(bytes,cls);u32(bytes+4,value);count=0;
        assert(!nb_pdp_flow_call(&flows,0,0,0,bytes,8,0,&count)&&!count);
        count=1;assert(!nb_pdp_flow_call(&flows,2,in,1,0,0,out,&count)&&out[0]==value);
        count=0;assert(!nb_pdp_flow_call(&flows,1,in,1,0,0,0,&count));
        count=1;assert(!nb_pdp_flow_call(&flows,2,in,1,0,0,out,&count)&&out[0]==0);
    }
    struct nb_pdp_flow before=flows;
    for(unsigned len=0;len<8;len++){count=0;assert(nb_pdp_flow_call(&flows,0,0,0,bytes,len,0,&count)==CBC_BAD_ARGUMENT);}
    count=0;assert(nb_pdp_flow_call(&flows,0,0,0,bytes,9,0,&count)==CBC_BAD_ARGUMENT);
    u32(bytes,10);count=0;assert(nb_pdp_flow_call(&flows,0,0,0,bytes,8,0,&count)==CBC_BAD_ARGUMENT);
    in[0]=10;count=1;assert(nb_pdp_flow_call(&flows,2,in,1,0,0,out,&count)==CBC_BAD_ARGUMENT);
    in[0]=0;count=0;assert(nb_pdp_flow_call(&flows,2,in,1,0,0,out,&count)==CBC_BAD_ARGUMENT);
    count=1;assert(nb_pdp_flow_call(&flows,2,in,2,0,0,out,&count)==CBC_BAD_ARGUMENT);
    count=2;assert(nb_pdp_flow_call(&flows,2,in,1,0,0,out,&count)==CBC_BAD_ARGUMENT);
    count=1;assert(nb_pdp_flow_call(&flows,2,in,1,bytes,8,out,&count)==CBC_BAD_ARGUMENT);
    count=1;assert(nb_pdp_flow_call(&flows,1,in,1,0,0,out,&count)==CBC_BAD_ARGUMENT);
    count=0;assert(nb_pdp_flow_call(&flows,0,in,1,bytes,8,out,&count)==CBC_BAD_ARGUMENT);
    count=1;assert(nb_pdp_flow_call(&flows,3,in,1,0,0,out,&count)==CBC_UNSUPPORTED);
    assert(!memcmp(&flows,&before,sizeof flows));
    puts("PASS PDP flow: original ten-class table, exact8-byte add, scalar remove/get, returned mapping and failure leaves state");
}
