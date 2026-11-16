/* SPDX-License-Identifier: BSD-2-Clause */
/* Public CTL/PDS framing, synthetic endpoints; awaits the selected test package. */
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/bridge/coachz_bb_qmux.h"
static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u %s\n",__LINE__,#x);}}while(0)
int main(void)
{
    struct cbq_bridge b;uint8_t out[64],frame[32]={1};cbq_init(&b);
    CHECK(cbq_client_alloc(&b,0,6)<0);
    CHECK(cbq_server_new(&b,16,2,0,184));b.local_pds=1;
    int local=cbq_client_alloc(&b,0,6);CHECK(local>=0&&b.client[local].local_service);
    CHECK(cbq_client_server(&b,(unsigned)local)==0);
    cbq_wr16(frame+1,12);frame[4]=6;frame[5]=b.client[local].cid;
    cbq_wr16(frame+7,27);cbq_wr16(frame+9,0x20);
    struct cbq_result r=cbq_from_host(&b,0,frame,13,out,sizeof out);CHECK(r.kind==CBQ_LOCAL);
    CHECK(cbq_server_new(&b,6,1,0,190));
    int real=cbq_client_alloc(&b,1,6);CHECK(real>=0&&!b.client[real].local_service);
    CHECK(cbq_client_server(&b,(unsigned)real)->port==190);
    cbq_client_free(&b,(unsigned)local);CHECK(cbq_next_release(&b)==0);
    cbq_client_free(&b,(unsigned)real);CHECK(cbq_next_release(&b)==CBQ_PORT_BASE+(unsigned)real);
    printf("GNSS QMUX %u checks, %u failures\n",checks,failures);return failures?1:0;
}
