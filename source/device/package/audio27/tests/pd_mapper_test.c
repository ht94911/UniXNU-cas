/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual GLINK/QRTR/locator code with a synthetic remote and stock-byte
 * answer oracle. No phone, PAS, DSP execution, MMIO or audio acceptance. */
#include "../platform/AudioPdMapper.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static QcomGlink local,remote;static NaPdMapper mapper;
static uint32_t th,tt,rh,rt;static uint8_t tx[4096],rx[4096],peer_storage[8*1024];
static QcomGlinkChannel *peer;
static struct {uint32_t bytes;uint8_t data[1024];} received[64];
static unsigned count,peer_up,gpr_up;
static uint32_t rd32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t v) {for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i));}
static void peer_state(void *v,QcomGlinkChannel *c,int up) {(void)v;(void)c;peer_up=!!up;}
static void gpr_state(void *v,QcomGlinkChannel *c,int up) {(void)v;(void)c;gpr_up=!!up;}
static void peer_receive(void *v,QcomGlinkChannel *c,const uint8_t *p,uint32_t n) {
    (void)v;(void)c;assert(count<64 && n<=1024);received[count].bytes=n;
    memcpy(received[count++].data,p,n);
}
static void step(unsigned iterations) {
    while (iterations--) {
        assert(na_pd_mapper_poll(&mapper)>=0);
        assert(qcom_glink_poll(&local)>=0);
        assert(na_pd_mapper_poll(&mapper)>=0);
        assert(qcom_glink_poll(&remote)>=0);
    }
}
static void cold(void) {
    memset(&mapper,0,sizeof mapper);th=tt=rh=rt=count=peer_up=gpr_up=0;
    QcomGlinkFifo t={&th,&tt,tx,sizeof tx},r={&rh,&rt,rx,sizeof rx};
    qcom_glink_init(&local,t,r,0,0);qcom_glink_init(&remote,r,t,0,0);
    const QcomGlinkClient gpr={gpr_state,0},client={peer_state,peer_receive};
    assert(qcom_glink_channel(&local,"adsp_apps",0,0,0,&gpr,0,0));
    assert(!na_pd_mapper_init(&mapper,&local));
    peer=qcom_glink_channel(&remote,"IPCRTR",1024,8,peer_storage,&client,0,1);assert(peer);
    step(16);assert(mapper.online && peer_up && mapper.qrtr.hello_sent);
    assert(mapper.tx_sent==1 && count==1 && rd32(received[0].data+4)==QCOM_QRTR_HELLO);
}
static uint32_t packet(uint8_t *out,uint32_t type,uint32_t confirm,const uint8_t *data,uint32_t bytes) {
    assert(bytes<=480);memset(out,0,512);
    put32(out,1);put32(out+4,type);put32(out+8,2);put32(out+12,type==QCOM_QRTR_DATA ? 99:QCOM_QRTR_PORT_CTRL);
    put32(out+16,confirm);put32(out+20,bytes);put32(out+24,1);
    put32(out+28,type==QCOM_QRTR_DATA ? 1:QCOM_QRTR_PORT_CTRL);
    memcpy(out+32,data,bytes);return 32+((bytes+3)&~3u);
}
static uint32_t query(uint8_t *out,uint16_t transaction,const char *service,uint32_t confirm) {
    uint8_t qmi[128]={0};size_t bytes=strlen(service);assert(bytes<64);
    qmi[1]=(uint8_t)transaction;qmi[2]=(uint8_t)(transaction>>8);qmi[3]=0x21;
    qmi[5]=(uint8_t)(3+bytes);qmi[7]=1;qmi[8]=(uint8_t)bytes;memcpy(qmi+10,service,bytes);
    return packet(out,QCOM_QRTR_DATA,confirm,qmi,(uint32_t)(10+bytes));
}
static void send_peer(const uint8_t *p,uint32_t bytes) {
    assert(qcom_glink_send(&remote,peer,p,bytes)==QcomGlinkOk);step(16);
}
static void routing(void) {
    cold();uint8_t p[512],hello[20]={0};put32(hello,QCOM_QRTR_HELLO);
    uint32_t n=packet(p,QCOM_QRTR_HELLO,0,hello,sizeof hello);send_peer(p,n);
    assert(mapper.qrtr.hello_received && mapper.qrtr.remote_node==2 && count==2);
    const uint8_t *announced=received[1].data;
    assert(rd32(announced+4)==QCOM_QRTR_NEW_SERVER && rd32(announced+24)==2);
    assert(rd32(announced+36)==0x40 && rd32(announced+40)==0x101 && rd32(announced+44)==1 && rd32(announced+48)==1);
    n=query(p,0x1234,"avs/audio",1);send_peer(p,n);
    assert(mapper.requests==1 && mapper.answers==1 && mapper.last_transaction==0x1234 && !strcmp(mapper.last_service,"avs/audio"));
    assert(count==4 && mapper.qrtr.resumes_sent==1 && mapper.tx_sent==4);
    static const uint8_t expected[]={
        2,0x34,0x12,0x21,0,74,0,
        2,4,0,0,0,0,0,0x10,2,0,2,0,0x11,2,0,1,0,0x12,54,0,2,
        17,'m','s','m','/','a','d','s','p','/','a','u','d','i','o','_','p','d',74,0,0,0,0,0,0,0,0,
        16,'m','s','m','/','c','d','s','p','/','r','o','o','t','_','p','d',76,0,0,0,0,0,0,0,0,
    };
    const uint8_t *answer=received[2].data;
    assert(rd32(answer+4)==QCOM_QRTR_DATA && rd32(answer+8)==1 && rd32(answer+12)==1 && rd32(answer+24)==2 && rd32(answer+28)==99);
    assert(rd32(answer+20)==sizeof expected && !memcmp(answer+32,expected,sizeof expected));
    assert(rd32(received[3].data+4)==QCOM_QRTR_RESUME_TX && rd32(received[3].data+36)==2 && rd32(received[3].data+40)==99);
    n=query(p,0x2345,"tms/servreg",0);send_peer(p,n);
    answer=received[count-1].data+32;
    assert(answer[17]==6 && answer[18]==0 && answer[27]==6);
    static const char *const names[]={"msm/adsp/audio_pd","msm/adsp/root_pd","msm/adsp/sensor_pd",
        "msm/cdsp/root_pd","msm/modem/root_pd","msm/modem/oem_pd"};
    static const uint32_t instance[]={74,74,74,76,180,180};
    unsigned position=28;
    for (unsigned i=0;i<6;++i) {
        unsigned length=answer[position];assert(length==strlen(names[i]));
        assert(!memcmp(answer+position+1,names[i],length) && rd32(answer+position+1+length)==instance[i]);
        position+=1+length+9;
    }
    /* Observed policy queries have no declared service in stock JSNs.
     * Preserve the empty result, rather than fabricate an enabling domain. */
    const char *const policy[]={"tms/pddump_disabled","tms/pdr_enabled"};
    for (unsigned i=0;i<2;++i) {
        n=query(p,(uint16_t)(0x2400+i),policy[i],0);send_peer(p,n);answer=received[count-1].data+32;
        assert(answer[17]==0 && answer[18]==0 && rd32(received[count-1].data+20)==24);
    }
    /* A later audio channel uses this same parser/owner, not a second link. */
    const QcomGlinkClient gpr={0,0};
    assert(qcom_glink_channel(&remote,"adsp_apps",0,0,0,&gpr,0,1));step(16);assert(gpr_up);
    /* No remote intent: retain the exact QMI reply until an intent is usable. */
    uint8_t saved[sizeof mapper.channel->remote];memcpy(saved,mapper.channel->remote,sizeof saved);
    memset(mapper.channel->remote,0,sizeof saved);uint32_t sent=mapper.tx_sent;
    n=query(p,0x3456,"no/such",0);send_peer(p,n);
    assert(mapper.head-mapper.tail==1 && mapper.tx_sent==sent && mapper.qrtr.tx_refused==0);
    unsigned pending=mapper.tail%NA_PD_TX_SLOTS;uint8_t retained[512];uint32_t retained_bytes=mapper.tx[pending].bytes;
    memcpy(retained,mapper.tx[pending].data,retained_bytes);step(16);
    assert(mapper.head-mapper.tail==1 && !memcmp(retained,mapper.tx[pending].data,retained_bytes));
    memcpy(mapper.channel->remote,saved,sizeof saved);step(16);
    assert(mapper.head==mapper.tail && mapper.tx_sent==sent+1);
    assert(received[count-1].bytes==retained_bytes && !memcmp(received[count-1].data,retained,retained_bytes));
    /* Full TX FIFO is a second wait for that same packet, never a dequeue. */
    n=query(p,0x5678,"avs/audio",0);mapper.channel->client.receive(mapper.channel->context,mapper.channel,p,n);
    assert(mapper.head-mapper.tail==1);sent=mapper.tx_sent;uint32_t original_head=th;
    th=(tt+sizeof tx-24)%sizeof tx;
    assert(na_pd_mapper_poll(&mapper)==NA_PENDING && mapper.head-mapper.tail==1 && mapper.tx_sent==sent);
    th=original_head;step(16);assert(mapper.head==mapper.tail && mapper.tx_sent==sent+1);
    /* Channel loss is terminal in this DSP lifetime; it does not clear queues. */
    sent=mapper.tx_sent;uint32_t head=mapper.head;
    mapper.channel->client.state(mapper.channel->context,mapper.channel,0);
    assert(na_pd_mapper_poll(&mapper)==NA_REMOTE && !mapper.online);
    mapper.channel->client.state(mapper.channel->context,mapper.channel,1);
    assert(!mapper.online && mapper.tx_sent==sent && mapper.head==head && na_pd_mapper_init(&mapper,&local)==NA_OWNERSHIP);
}
static void failure(void) {
    cold();uint8_t p[512];uint32_t n=query(p,7,"avs/audio",0);
    for (unsigned i=0;i<NA_PD_TX_SLOTS;++i) mapper.channel->client.receive(mapper.channel->context,mapper.channel,p,n);
    assert(mapper.head-mapper.tail==NA_PD_TX_SLOTS);
    mapper.channel->client.receive(mapper.channel->context,mapper.channel,p,n);
    assert(mapper.error==NA_OVERFLOW && mapper.qrtr.tx_refused==1 && na_pd_mapper_poll(&mapper)==NA_OVERFLOW);
    cold();mapper.channel->client.receive(mapper.channel->context,mapper.channel,p,31);
    assert(mapper.error==NA_WIRE && mapper.qrtr.rx_bad==1 && na_pd_mapper_poll(&mapper)==NA_WIRE);
}
int main(void) {
    routing();failure();
    puts("PD mapper PASS: production GLINK/QRTR, stock QMI answer bytes, flow control, same audio link, deferred exact replies, FIFO wait, loss/overflow/malformed terminal; synthetic peer only");
    return 0;
}
