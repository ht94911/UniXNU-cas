/* SPDX-License-Identifier: BSD-2-Clause */
/* Real state-machine source, synthetic peer. Execution awaits package choice. */
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/gnss/NbGnss.h"
static NbGnss state;
static unsigned checks,failures,answers,indications;static uint32_t last_error;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u %s\n",__LINE__,#x);}}while(0)
static int emit(void *context,uint32_t channel,uint32_t cid,const uint8_t *data,uint32_t n)
{
    (void)context;(void)channel;(void)cid;NbQmiMessage m;
    CHECK(nb_gnss_message(data,n,&m));
    if(m.type==NB_QMI_RESPONSE){++answers;last_error=nb_gnss_result(&m);}else ++indications;
    return 1;
}
static void request(uint32_t client,uint16_t txn,uint16_t command,uint8_t tag,uint8_t value)
{
    uint8_t raw[32];NbQmiWriter w;nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_REQUEST,txn,command);
    if(tag) nb_qmi_u8(&w,tag,value);
    nb_gnss_request(&state,client,raw,nb_qmi_end(&w));
}
static void reply(NbGnssPacket *p,uint32_t error,int ind_first)
{
    uint8_t raw[32];NbQmiWriter w;
    nb_gnss_sent(&state,p,0,100);
    if(ind_first){
        nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,p->message);nb_qmi_u32(&w,1,0);
        if(p->message==NB_LOC_SERVICE_REVISION)nb_qmi_u32(&w,2,134);
        nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),101);CHECK(state.waiting);
    }
    nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_RESPONSE,p->transaction,p->message);nb_qmi_result(&w,error?1:0,(uint16_t)error);
    nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),102);
    if(!ind_first&&!error&&(p->message==NB_LOC_SET_MODE||p->message==NB_LOC_SERVICE_REVISION)){
        CHECK(state.waiting);nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,p->message);nb_qmi_u32(&w,1,0);
        if(p->message==NB_LOC_SERVICE_REVISION)nb_qmi_u32(&w,2,134);
        nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),103);
    }
}
static void setup(void)
{
    NbGnssPacket p;nb_gnss_init(&state,0x4200,0,emit,0);nb_gnss_endpoint(&state,2,0,184);
    nb_gnss_attach(&state,1,0,1);nb_gnss_attach(&state,2,1,1);
    request(1,1,NB_PDS_GET_STATE,0,0);CHECK(answers==0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SERVICE_REVISION);reply(&p,0,1);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_REGISTER);
    NbQmiMessage registration;CHECK(nb_gnss_message(p.data,p.length,&registration));
    const uint8_t *type=nb_gnss_tlv(&registration,0x11,4);
    const uint8_t *notify=nb_gnss_tlv(&registration,0x12,1);
    uint32_t client_length=0;const uint8_t *client=nb_qmi_tlv(&registration,0x10,&client_length);
    CHECK(type&&nb_le32(type)==1&&notify&&*notify==1);
    CHECK(client&&client_length==strlen("NezhaCoreLocation")&&!memcmp(client,"NezhaCoreLocation",client_length));
    reply(&p,0,0);
    CHECK(state.phase==NbGnssReady);CHECK(!nb_gnss_next(&state,0,&p)&&answers==1);
}
int main(void)
{
    NbGnssPacket p;setup();
    /* Queued native requests must merge against the preceding acknowledged state. */
    request(1,2,NB_PDS_SET_STATE,1,1);request(1,3,NB_PDS_SET_AUTO,1,1);
    request(1,4,NB_PDS_REPORT,0x12,1);
    CHECK(!nb_gnss_next(&state,0,&p)&&state.clients[1].enabled);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SET_MODE);CHECK(!state.running);
    reply(&p,0,1);CHECK(!state.running);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);CHECK(!state.clients[1].automatic);
    reply(&p,0,0);CHECK(state.running&&state.clients[1].automatic);
    CHECK(!nb_gnss_next(&state,0,&p)&&state.clients[1].parsed_report);
    const unsigned starts=(unsigned)state.starts;
    request(2,5,NB_PDS_SET_STATE,1,1);request(2,6,NB_PDS_SET_AUTO,1,1);
    CHECK(!nb_gnss_next(&state,0,&p));CHECK(!nb_gnss_next(&state,0,&p));CHECK(state.starts==starts);
    nb_gnss_retire(&state,1);CHECK(!nb_gnss_next(&state,0,&p)&&state.running);
    nb_gnss_retire(&state,2);CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_STOP);
    reply(&p,0,0);CHECK(!state.running);CHECK(!nb_gnss_next(&state,0,&p));
    nb_gnss_attach(&state,1,0,2);request(1,7,NB_PDS_SET_STATE,1,1);request(1,8,NB_PDS_SET_AUTO,1,1);
    CHECK(!nb_gnss_next(&state,0,&p));CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SET_MODE);reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);
    nb_gnss_sent(&state,&p,0,200);nb_gnss_poll(&state,200+NB_GNSS_WAIT_MS);
    CHECK(last_error==NB_GNSS_ERROR_TIMEOUT&&!state.clients[1].automatic);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_STOP);reply(&p,0,0);
    CHECK(!nb_gnss_next(&state,0,&p));
    /* End-of-session without a native stop must stay stopped until a new
     * explicit native start, even if an unrelated query follows it. */
    request(1,10,NB_PDS_SET_AUTO,1,1);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SET_MODE);reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);reply(&p,0,0);
    uint8_t ended[24];NbQmiWriter ew;
    nb_qmi_begin(&ew,ended,sizeof ended,NB_QMI_INDICATION,0,NB_LOC_SESSION);
    nb_qmi_u32(&ew,1,2);nb_qmi_u8(&ew,0x10,state.session);
    nb_gnss_receive(&state,0,184,ended,nb_qmi_end(&ew),220);
    CHECK(!state.running&&state.clients[1].failed);
    request(1,11,NB_PDS_GET_STATE,0,0);CHECK(!nb_gnss_next(&state,0,&p));CHECK(!nb_gnss_next(&state,0,&p));
    request(1,9,0x37,0,0);CHECK(last_error==NB_QMI_ERR_NOT_SUPPORTED);
    const unsigned before=answers;nb_gnss_lost(&state);CHECK(state.phase==NbGnssAbsent);
    nb_gnss_sent(&state,&p,0,300);CHECK(!state.waiting&&answers==before);
    /* The actual GNSS002 INVALID_ARG must retain the rejected command and
     * close this endpoint instead of reporting ready or sending START. */
    nb_gnss_init(&state,0x4201,0,emit,0);nb_gnss_endpoint(&state,2,0,184);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SERVICE_REVISION);reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_REGISTER);reply(&p,NB_QMI_ERR_INVALID_ARG,0);
    CHECK(state.phase==NbGnssFailed&&state.failed_command==NB_LOC_REGISTER&&state.last_error==48);
    CHECK(!state.running&&!nb_gnss_next(&state,0,&p));
    printf("GNSS lifecycle %u checks, %u failures; %u state indications\n",checks,failures,indications);return failures?1:0;
}
