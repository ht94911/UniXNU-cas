/* SPDX-License-Identifier: BSD-2-Clause */
/* Real state-machine source, synthetic peer. Execution awaits package choice. */
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/gnss/NbGnss.h"
static NbGnss state;
static unsigned checks,failures,answers,indications;static uint32_t last_error;
static unsigned measurement_indications,measurement_drop_cid;
static uint64_t measurement_channels;
static uint8_t last_measurement_sequence, last_measurement_maximum;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u %s\n",__LINE__,#x);}}while(0)
static int emit(void *context,uint32_t channel,uint32_t cid,const uint8_t *data,uint32_t n)
{
    (void)context;NbQmiMessage m;
    CHECK(nb_gnss_message(data,n,&m));
    if(m.message==NB_PDS_MEASUREMENT){
        CHECK(m.type==NB_QMI_INDICATION&&channel<CBQ_CHANNELS);
        if(cid==measurement_drop_cid)return 0;
        ++measurement_indications;measurement_channels|=UINT64_C(1)<<channel;
        last_measurement_sequence=*nb_gnss_tlv(&m,1,1);
        last_measurement_maximum=*nb_gnss_tlv(&m,2,1);
    }
    if(m.type==NB_QMI_RESPONSE){++answers;last_error=nb_gnss_result(&m);}else ++indications;
    return 1;
}
static void request(uint32_t client,uint16_t txn,uint16_t command,uint8_t tag,uint8_t value)
{
    uint8_t raw[32];NbQmiWriter w;nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_REQUEST,txn,command);
    if(tag) nb_qmi_u8(&w,tag,value);
    nb_gnss_request(&state,client,raw,nb_qmi_end(&w));
}
static void state_request(uint16_t txn,uint8_t enabled,uint8_t services_tag,uint32_t bytes,uint64_t services)
{
    uint8_t raw[64],value[8];NbQmiWriter w;nb_put64(value,services);
    nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_REQUEST,txn,NB_PDS_SET_STATE);
    nb_qmi_u8(&w,1,enabled);
    if(services_tag)nb_qmi_put(&w,services_tag,value,bytes);
    nb_gnss_request(&state,1,raw,nb_qmi_end(&w));
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
    if(!ind_first&&!error&&(p->message==NB_LOC_SET_MODE||p->message==NB_LOC_SERVICE_REVISION||
                          p->message==NB_LOC_INJECT_POSITION||p->message==NB_LOC_REPORT_CONSTELLATIONS)){
        CHECK(state.waiting);nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,p->message);nb_qmi_u32(&w,1,0);
        if(p->message==NB_LOC_SERVICE_REVISION)nb_qmi_u32(&w,2,134);
        nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),103);
    }
}
static void reference_request(uint16_t txn)
{
    uint8_t raw[128];NbQmiWriter w;
    nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_REQUEST,txn,NB_PDS_INJECT_POSITION);
    nb_qmi_u64(&w,0x10,1700000000123ull);nb_qmi_u64(&w,0x11,0);nb_qmi_u64(&w,0x12,0);
    nb_qmi_u32(&w,0x13,0x41a00000);nb_qmi_u32(&w,0x15,0x42b40000);nb_qmi_u32(&w,0x16,0x41f00000);
    nb_qmi_u8(&w,0x17,68);nb_qmi_u8(&w,0x18,68);nb_qmi_u8(&w,0x19,1);nb_qmi_u8(&w,0x1a,0);
    const uint8_t reliability[]={0,2},altinfo[]={9,2,2};
    nb_qmi_put(&w,0x1b,reliability,2);nb_qmi_put(&w,0x1c,altinfo,3);
    nb_gnss_request(&state,1,raw,nb_qmi_end(&w));
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
    CHECK(type&&nb_le32(type)==1&&notify&&*notify==0);
    CHECK(client&&client_length==3&&!memcmp(client,"HAL",3));
    CHECK(p.length==35); /* Header7 + mask11 + identifier6 + type7 + notify4. */
    const uint8_t *mask=nb_gnss_tlv(&registration,1,8);
    CHECK(mask&&nb_le64(mask)==(UINT64_C(0x187)|NB_GNSS_MEASUREMENT_EVENT));
    reply(&p,0,0);
    CHECK(state.phase==NbGnssMeasurementConfig&&!state.running);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_REPORT_CONSTELLATIONS);
    CHECK(nb_gnss_message(p.data,p.length,&registration));
    mask=nb_gnss_tlv(&registration,0x10,8);
    CHECK(mask&&nb_le64(mask)==0x3fu&&registration.length==22u);
    mask=nb_gnss_tlv(&registration,0x11,8);CHECK(mask&&!nb_le64(mask));
    reply(&p,0,0);
    CHECK(state.phase==NbGnssReady&&state.measurement_configured);
    CHECK(!nb_gnss_next(&state,0,&p)&&answers==1);
}
static void measurement(uint32_t tick,uint32_t resets,uint8_t sequence,uint8_t maximum,uint64_t now)
{
    uint8_t raw[128],clock[17]={0};NbQmiWriter w;
    nb_put32(clock,tick);clock[4]=1;nb_put64(clock+5,1700000000000ull);nb_put32(clock+13,7);
    nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,NB_LOC_MEASUREMENT);
    nb_qmi_u8(&w,1,sequence);nb_qmi_u8(&w,2,maximum);nb_qmi_u32(&w,3,1);
    nb_qmi_put(&w,0x1a,clock,sizeof clock);nb_qmi_u32(&w,0x1c,resets);
    nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),now);
}
static void measurement_configuration_tests(void)
{
    NbGnssPacket p;uint8_t raw[32];NbQmiWriter w;
    const uint32_t statuses[]={0,2,3,4,6,7,8};
    const uint32_t errors[]={0,NB_QMI_ERR_NOT_SUPPORTED,NB_QMI_ERR_INVALID_ARG,
        NB_GNSS_ERROR_BUSY,NB_GNSS_ERROR_TIMEOUT,NB_QMI_ERR_NOT_SUPPORTED,NB_QMI_ERR_NO_MEMORY};
    for(unsigned i=0;i<sizeof statuses/sizeof statuses[0];++i){
        nb_gnss_init(&state,0x4200,0,emit,0);nb_gnss_endpoint(&state,2,0,184);
        CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);
        CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);
        CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_REPORT_CONSTELLATIONS);
        nb_gnss_sent(&state,&p,0,4000);
        nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,p.message);nb_qmi_u32(&w,1,statuses[i]);
        nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),4001);
        if(!statuses[i])CHECK(state.waiting&&!state.measurement_configured);
        nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_RESPONSE,p.transaction,p.message);nb_qmi_result(&w,0,0);
        nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),4002);
        CHECK(state.measurement_config_error==errors[i]&&!state.running);
        CHECK(state.measurement_configured==(statuses[i]==0));
        CHECK(state.phase==((!statuses[i]||statuses[i]==2||statuses[i]==7)?NbGnssReady:NbGnssFailed));
    }
    /* Missing status and missing ACK never establish measurement readiness. */
    for(unsigned mode=0;mode<3;++mode){
        nb_gnss_init(&state,0x4200,0,emit,0);nb_gnss_endpoint(&state,2,0,184);
        CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);
        CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);
        CHECK(nb_gnss_next(&state,0,&p));nb_gnss_sent(&state,&p,0,5000);
        nb_qmi_begin(&w,raw,sizeof raw,mode==1?NB_QMI_RESPONSE:NB_QMI_INDICATION,
            mode==1?p.transaction:0,p.message);
        if(mode==1)nb_qmi_result(&w,0,0);else if(mode==2)nb_qmi_u32(&w,1,0);
        nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),5001);
        if(mode)CHECK(state.waiting&&!state.measurement_configured);
        nb_gnss_poll(&state,5000+NB_GNSS_WAIT_MS);
        CHECK(state.phase==NbGnssFailed&&!state.running&&!state.measurement_configured);
        CHECK(state.measurement_config_error==(mode?NB_GNSS_ERROR_TIMEOUT:NB_QMI_ERR_MALFORMED_MSG));
    }
}
static void measurement_lifecycle_tests(void)
{
    NbGnssPacket p;answers=indications=0;setup();
    measurement(100,1,1,2,6000);
    CHECK(state.measurement_reports==1&&state.measurement_rejected==1&&!state.measurement_published);
    request(1,70,NB_PDS_SET_STATE,1,1);request(1,71,NB_PDS_SET_AUTO,1,1);
    request(1,72,NB_PDS_REPORT,0x12,1);
    CHECK(!nb_gnss_next(&state,0,&p));CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);CHECK(!nb_gnss_next(&state,0,&p));
    measurement(100,1,1,2,6001);
    CHECK(state.measurement_published==1&&state.measurement_seen&&state.measurement_received_ms==6001);
    const uint64_t fix_sequence=state.fix.sequence,positions=state.positions,fix_deliveries=state.published;
    measurement(100,1,2,2,6002);CHECK(state.measurement_published==2);
    measurement(100,1,2,2,6003);measurement(99,1,1,2,6004);measurement(100,1,3,3,6005);
    CHECK(state.measurement_published==2&&state.measurement_rejected==4&&state.measurement_received_ms==6002);
    measurement(101,1,1,2,6006);CHECK(state.measurement_published==3);
    measurement(1,2,1,2,6007);CHECK(state.measurement_published==4&&state.measurement.tick==1);
    measurement(500,1,1,2,6007);CHECK(state.measurement_published==4&&state.measurement.clock_resets==2);
    CHECK(state.positions==positions&&state.fix.sequence==fix_sequence&&state.published==fix_deliveries);
    request(2,73,NB_PDS_SET_STATE,1,1);request(2,74,NB_PDS_SET_AUTO,1,1);
    request(2,75,NB_PDS_REPORT,0x12,1);
    CHECK(!nb_gnss_next(&state,0,&p));CHECK(!nb_gnss_next(&state,0,&p));CHECK(!nb_gnss_next(&state,0,&p));
    measurement(2,2,1,2,6008);CHECK(state.measurement_published==6&&measurement_channels==3);
    measurement_drop_cid=state.clients[2].cid;
    const uint64_t old_drop=state.output_drops;
    /* Give the two channels distinct client IDs for the controlled emit failure. */
    state.clients[2].cid=9;measurement_drop_cid=9;
    measurement(3,2,1,2,6009);CHECK(state.measurement_published==7&&state.output_drops==old_drop+1);
    measurement_drop_cid=0;nb_gnss_retire(&state,1);
    measurement(4,2,1,2,6010);CHECK(state.measurement_published==8);
    request(2,76,NB_PDS_SET_AUTO,1,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_STOP);reply(&p,0,0);
    CHECK(!state.running&&!state.measurement_seen);
    measurement(5,2,1,2,6011);CHECK(state.measurement_published==8&&state.measurement_rejected==6);
    request(2,77,NB_PDS_SET_AUTO,1,1);
    CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);
    measurement(0xfffffff0u,2,1,2,6012);measurement(16,2,1,2,6013);
    CHECK(state.measurement_published==10&&state.measurement.tick==16);
    nb_gnss_lost(&state);
    CHECK(!state.measurement_configured&&!state.measurement_seen&&!state.measurement.tick);
    measurement(17,2,1,2,6014);CHECK(state.measurement_published==10);
    /* A real zero-band, zero-SV slot must pass through the production
     * lifecycle between its neighbors, without becoming a position. */
    answers=indications=0;setup();request(1,80,NB_PDS_SET_STATE,1,1);request(1,81,NB_PDS_SET_AUTO,1,1);
    request(1,82,NB_PDS_REPORT,0x12,1);
    CHECK(!nb_gnss_next(&state,0,&p));CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p));reply(&p,0,0);CHECK(!nb_gnss_next(&state,0,&p));
    for(unsigned i=1;i<=10;++i){
        uint8_t raw[128],clock[17]={0};NbQmiWriter w;
        nb_put32(clock,100);clock[4]=1;nb_put32(clock+13,7);
        nb_qmi_begin(&w,raw,sizeof raw,NB_QMI_INDICATION,0,NB_LOC_MEASUREMENT);
        nb_qmi_u8(&w,1,i);nb_qmi_u8(&w,2,10);nb_qmi_u32(&w,3,i==4u?5u:1u);
        nb_qmi_put(&w,0x1a,clock,sizeof clock);nb_qmi_u32(&w,0x1c,1);
        if(i==4u){const uint8_t empty=0;nb_qmi_u64(&w,0x1e,0);nb_qmi_put(&w,0x1b,&empty,1);}
        nb_gnss_receive(&state,0,184,raw,nb_qmi_end(&w),7000+i);
        CHECK(state.measurement_published==i&&!state.measurement_rejected);
        CHECK(last_measurement_sequence==i&&last_measurement_maximum==10);
    }
    CHECK(!state.positions&&!state.fix.sequence&&!state.published);
}
int main(void)
{
    NbGnssPacket p;setup();
    /* Actual24A446 enable is header7+TLV1/1+TLV10/8 =22bytes.
     * This must enable the client before automatictracking reaches realLOC.
     * Keep other services, malformed widths and flags rejected. */
    state_request(90,1,0x10,8,1);
    CHECK(state.count==1&&!state.clients[1].enabled);
    CHECK(!nb_gnss_next(&state,0,&p)&&state.clients[1].enabled&&!state.running&&last_error==0);
    state_request(91,0,0,0,0);
    CHECK(!nb_gnss_next(&state,0,&p)&&!state.clients[1].enabled);
    state_request(92,1,0x10,8,2);CHECK(last_error==NB_QMI_ERR_NOT_SUPPORTED&&!state.clients[1].enabled&&!state.count);
    state_request(93,1,0x10,7,1);CHECK(last_error==NB_QMI_ERR_INVALID_ARG&&!state.count);
    state_request(94,1,0x10,8,0);CHECK(last_error==NB_QMI_ERR_INVALID_ARG&&!state.count);
    state_request(95,0,0x10,8,1);CHECK(last_error==NB_QMI_ERR_INVALID_ARG&&!state.count);
    state_request(96,1,0x11,8,1);CHECK(last_error==NB_QMI_ERR_INVALID_ARG&&!state.count);
    state_request(97,2,0x10,8,1);CHECK(last_error==NB_QMI_ERR_INVALID_ARG&&!state.count);
    /* Queued native requests must merge against the preceding acknowledged state. */
    state_request(2,1,0x10,8,1);request(1,3,NB_PDS_SET_AUTO,1,1);
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
    /* GNSS030's original initialization requests force-off=1, service=1,
     * automatic=0, then service=0. Later the original tracking controller
     * explicitly enables service and automatic tracking, without a second
     * force-off request. The earlier off must not suppress this new start. */
    answers=indications=0;setup();
    request(1,20,NB_PDS_FORCE_OFF,1,1);
    CHECK(!nb_gnss_next(&state,0,&p)&&state.clients[1].forced_off&&!state.running);
    state_request(21,1,0x10,8,1);request(1,22,NB_PDS_SET_AUTO,1,0);
    state_request(23,0,0,0,0);
    CHECK(!nb_gnss_next(&state,0,&p));CHECK(!nb_gnss_next(&state,0,&p));
    CHECK(!nb_gnss_next(&state,0,&p)&&!state.running&&!state.clients[1].enabled);
    state_request(24,1,0x10,8,1);request(1,25,NB_PDS_SET_AUTO,1,1);
    CHECK(!nb_gnss_next(&state,0,&p)&&state.clients[1].enabled&&!state.running);
    if(nb_gnss_next(&state,0,&p)) {
        CHECK(p.message==NB_LOC_SET_MODE);reply(&p,0,1);
        CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);reply(&p,0,0);
        CHECK(state.running&&state.clients[1].automatic&&!state.clients[1].forced_off);
        request(1,26,NB_PDS_FORCE_OFF,1,1);
        CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_STOP);reply(&p,0,0);
        CHECK(!state.running&&!nb_gnss_next(&state,0,&p));
        /* A new explicit automatic start also supersedes the earlier off;
         * an intervening query must keep the stopped session stopped. */
        request(1,27,NB_PDS_GET_STATE,0,0);
        CHECK(!nb_gnss_next(&state,0,&p)&&!state.running);
        request(1,28,NB_PDS_SET_AUTO,1,1);
        CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SET_MODE);reply(&p,0,0);
        CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);reply(&p,0,0);
        CHECK(state.running&&!state.clients[1].forced_off&&state.starts==2&&state.stops==1);
    } else CHECK(0&&"explicit native start blocked by earlier initialization off");
    /* Real reference ACK plus status complete the same queued request.
     * Assistance never starts a stopped receiver or becomes a GPS fix. */
    request(1,29,NB_PDS_SET_AUTO,1,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_STOP);reply(&p,0,0);
    CHECK(!state.active&&!state.running&&!state.clients[1].automatic);
    CHECK(!nb_gnss_next(&state,0,&p)&&!state.running&&!state.active);
    reference_request(30);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_INJECT_POSITION);
    const unsigned prior_answers=answers;
    reply(&p,0,0);CHECK(answers==prior_answers+1&&!state.running&&state.reference_completed==1);
    CHECK(!state.positions&&!state.fix.deliveries&&!state.inhibited);
    reference_request(31);CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_INJECT_POSITION);
    reply(&p,0,1);CHECK(state.reference_completed==2&&!state.running);
    reference_request(32);CHECK(nb_gnss_next(&state,0,&p));
    reply(&p,NB_QMI_ERR_NOT_SUPPORTED,0);
    CHECK(last_error==NB_QMI_ERR_NOT_SUPPORTED&&state.reference_errors==1&&state.phase==NbGnssReady&&!state.inhibited);
    CHECK(!nb_gnss_next(&state,0,&p)&&!state.running);
    reference_request(33);CHECK(nb_gnss_next(&state,0,&p));nb_gnss_sent(&state,&p,0,1000);
    CHECK(state.reference_status==UINT32_MAX);
    nb_gnss_poll(&state,1000+NB_GNSS_WAIT_MS);
    CHECK(last_error==NB_GNSS_ERROR_TIMEOUT&&state.reference_errors==2&&!state.running);
    CHECK(!nb_gnss_next(&state,0,&p)&&state.phase==NbGnssReady);
    /* Keep the LOC status result, and leave ordinary tracking usable after
     * an optional reference is rejected by the engine. */
    const uint32_t engine_status[]={2,3,4,6,7,8};
    const uint32_t pds_error[]={NB_QMI_ERR_NOT_SUPPORTED,NB_QMI_ERR_INVALID_ARG,
        NB_GNSS_ERROR_BUSY,NB_GNSS_ERROR_TIMEOUT,NB_QMI_ERR_NOT_SUPPORTED,NB_QMI_ERR_NO_MEMORY};
    for(unsigned i=0;i<sizeof engine_status/sizeof engine_status[0];i++){
        reference_request((uint16_t)(40+i));CHECK(nb_gnss_next(&state,0,&p));
        nb_gnss_sent(&state,&p,0,2000);
        nb_qmi_begin(&ew,ended,sizeof ended,NB_QMI_RESPONSE,p.transaction,p.message);nb_qmi_result(&ew,0,0);
        nb_gnss_receive(&state,0,184,ended,nb_qmi_end(&ew),2001);CHECK(state.waiting);
        nb_qmi_begin(&ew,ended,sizeof ended,NB_QMI_INDICATION,0,p.message);nb_qmi_u32(&ew,1,engine_status[i]);
        nb_gnss_receive(&state,0,184,ended,nb_qmi_end(&ew),2002);
        CHECK(last_error==pds_error[i]&&state.reference_status==engine_status[i]);
        CHECK(state.phase==NbGnssReady&&!state.running&&!state.inhibited);
    }
    request(1,50,NB_PDS_SET_AUTO,1,1);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SET_MODE);reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);reply(&p,0,0);
    reference_request(51);CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_INJECT_POSITION);
    nb_gnss_sent(&state,&p,0,3000);
    nb_qmi_begin(&ew,ended,sizeof ended,NB_QMI_INDICATION,0,NB_LOC_SESSION);
    nb_qmi_u32(&ew,1,2);nb_qmi_u8(&ew,0x10,state.session);
    nb_gnss_receive(&state,0,184,ended,nb_qmi_end(&ew),3001);
    CHECK(!state.running&&state.clients[1].failed&&state.inhibited);
    reply(&p,0,0);
    CHECK(state.clients[1].failed&&state.inhibited&&!state.running);
    CHECK(!nb_gnss_next(&state,0,&p));
    reference_request(52);CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_INJECT_POSITION);
    reply(&p,0,0);CHECK(state.inhibited&&!nb_gnss_next(&state,0,&p)&&!state.running);
    /* Retired client generations cannot consume the completion. */
    reference_request(53);CHECK(nb_gnss_next(&state,0,&p));
    nb_gnss_retire(&state,1);nb_gnss_attach(&state,1,0,7);
    const unsigned before_retired_reply=answers;
    reply(&p,0,0);CHECK(answers==before_retired_reply&&!state.running&&!state.clients[1].enabled);
    /* A normal stop queued behind assistance still gets its real ACK.
     * Assistance failures do not inhibit that stop or a subsequent start. */
    state_request(60,1,0x10,8,1);request(1,61,NB_PDS_SET_AUTO,1,1);
    CHECK(!nb_gnss_next(&state,0,&p));
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SET_MODE);reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);reply(&p,0,0);
    reference_request(62);CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_INJECT_POSITION);
    request(1,63,NB_PDS_SET_AUTO,1,0);reply(&p,NB_QMI_ERR_NOT_SUPPORTED,0);
    CHECK(state.running&&!state.inhibited&&!state.active&&state.count==1);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_STOP);reply(&p,0,0);
    CHECK(!state.running&&!state.active&&!state.clients[1].automatic);
    /* Updating a live interval must keep its request until the new START
     * is acknowledged; only a final stop finishes at its STOP ACK. */
    request(1,64,NB_PDS_SET_AUTO,1,1);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SET_MODE);reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);reply(&p,0,0);
    uint8_t tracking[32],criteria[10]={0,255};NbQmiWriter tw;
    nb_put32(criteria+2,2);nb_put32(criteria+6,400);
    nb_qmi_begin(&tw,tracking,sizeof tracking,NB_QMI_REQUEST,65,NB_PDS_SET_TRACKING);
    nb_qmi_put(&tw,1,criteria,sizeof criteria);
    nb_gnss_request(&state,1,tracking,nb_qmi_end(&tw));
    const unsigned before_tracking=answers;
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_STOP);reply(&p,0,0);
    CHECK(state.active&&answers==before_tracking&&!state.running);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_SET_MODE);reply(&p,0,0);
    CHECK(nb_gnss_next(&state,0,&p)&&p.message==NB_LOC_START);reply(&p,0,0);
    CHECK(!state.active&&state.running&&state.applied.interval_seconds==2&&answers==before_tracking+1);
    measurement_configuration_tests();measurement_lifecycle_tests();
    CHECK(measurement_indications==20);
    printf("GNSS lifecycle %u checks, %u failures; %u state indications\n",checks,failures,indications);return failures?1:0;
}
