/* SPDX-License-Identifier: BSD-2-Clause */
#include "../../package/baseband27/platform/NbPas.h"
#include "../../package/baseband27/platform/NbMss.h"
#include "../../package/baseband27/transport/NbQrtr.h"
#include "../../package/baseband27/services/NbRmtfs.h"
#include "../../package/baseband27/services/NbQmi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t file[16384], mapped[16384], metadata[4096] __attribute__((aligned(4096)));
static unsigned calls,syncs,busy,fail_auth;static uint32_t commands[16];
static void w16(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;}
static void w32(uint8_t *p,uint32_t v){w16(p,v);w16(p+2,v>>16);}
static void w64(uint8_t *p,uint64_t v){w32(p,(uint32_t)v);w32(p+4,(uint32_t)(v>>32));}
static int read_image(void *v,uint64_t off,void *to,size_t n){(void)v;if(off>sizeof file||n>sizeof file-off)return -1;memcpy(to,file+off,n);return 0;}
static int sync_image(void *v,void *cpu,uint64_t pa,uint64_t n){(void)v;assert(cpu&&pa&&n);syncs++;return 0;}
static int pas(void *v,uint32_t cmd,uint32_t id,uint64_t a,uint64_t b){(void)v;(void)a;(void)b;assert(id==4);if(busy){busy--;return NB_BUSY;}assert(calls<16);commands[calls++]=cmd;return fail_auth&&cmd==5 ? NB_IO:0;}
static void elf(int wide,int reloc){
    memset(file,0,sizeof file);memcpy(file,"\177ELF",4);file[4]=wide?2:1;file[5]=1;
    unsigned h=wide?64:52,p=wide?56:32,head=h+3*p;
    if(wide){w64(file+32,h);w16(file+54,p);w16(file+56,3);}else{w32(file+28,h);w16(file+42,p);w16(file+44,3);}
    for(unsigned i=0;i<3;i++){
        uint8_t *q=file+h+i*p;uint32_t off=i==0?0:i==1?4096:8192,size=i==0?head:i==1?32:17;
        uint32_t flags=i==1?QCOM_MDT_TYPE_HASH:reloc?QCOM_MDT_RELOCATABLE:0;
        uint64_t addr=i==2?0x8aa00000:0;w32(q,i==2?1:0);
        if(wide){w32(q+4,flags);w64(q+8,off);w64(q+24,addr);w64(q+32,size);w64(q+40,i==2?4096:size);}
        else{w32(q+4,off);w32(q+12,(uint32_t)addr);w32(q+16,size);w32(q+20,i==2?4096:size);w32(q+24,flags);}
    }
    memset(file+4096,0x35,32);memset(file+8192,0x72,17);
}
static void loading(void){
    NbPasOps ops={0,pas,sync_image};NbPas l;
    for(unsigned wide=0;wide<2;wide++)for(unsigned reloc=0;reloc<2;reloc++){
        elf(wide,reloc);memset(mapped,0xcc,sizeof mapped);calls=syncs=0;busy=1;fail_auth=0;
        assert(!nb_pas_begin(&l,4,(NbRange){0x8aa00000,sizeof mapped},mapped,sizeof file,read_image,0,metadata,0x90000000,sizeof metadata,&ops,0,100));
        assert(l.image.elf64==wide && calls==0);assert(nb_pas_poll(&l,0)==NB_PENDING && calls==0);
        int status=NB_PENDING;for(unsigned t=1;t<20&&status==NB_PENDING;t++)status=nb_pas_poll(&l,t);
        assert(status==0&&l.stage==NbPasReady&&commands[0]==1&&commands[calls-1]==5);
        assert(calls==(reloc?3u:2u));for(unsigned i=0;i<17;i++)assert(mapped[i]==0x72);
        for(unsigned i=17;i<4096;i++)assert(mapped[i]==0);
        assert(mapped[4096]==0xcc);
        assert(!nb_pas_poll(&l,200));
    }
    elf(1,0);calls=0;busy=0;fail_auth=1;
    assert(!nb_pas_begin(&l,4,(NbRange){0x8aa00000,sizeof mapped},mapped,sizeof file,read_image,0,metadata,0x90000000,sizeof metadata,&ops,0,100));
    int status=1;for(unsigned t=0;t<20&&status==1;t++)status=nb_pas_poll(&l,t);assert(status==NB_IO);
    unsigned n=calls;assert(nb_pas_poll(&l,20)==NB_IO&&calls==n);
    assert(nb_pas_begin(&l,4,(NbRange){0x8aa00000,sizeof mapped},mapped,sizeof file,read_image,0,metadata,UINT64_MAX-4095,sizeof metadata,&ops,0,100)<0);
    assert(nb_pas_begin(&l,4,(NbRange){0x8aa00000,sizeof mapped},mapped,sizeof file,read_image,0,metadata,0xfffff000,8192,&ops,0,100)<0);
    puts("PASS PAS: ELF32/ELF64, fixed/relocated, copy/zero, busy/error retention, low-memory bounds");
}
static uint8_t tx[NB_QRTR_MAX_PACKET],packet[64];static unsigned emitted,gone,removed;static uint32_t last_node;
static int send_packet(void *v,const uint8_t *p,uint32_t n){(void)v;NbQrtrHeader h;assert(!nb_qrtr_parse_header(p,n,&h));emitted++;last_node=h.dst_node;return 0;}
static void receive(void *v,NbQrtr *q,uint32_t p,uint32_t n,uint32_t f,const uint8_t *d,uint32_t l){(void)v;(void)q;(void)p;(void)n;(void)f;(void)d;(void)l;}
static void server_del(void *v,uint32_t s,uint32_t i,uint32_t n,uint32_t p){(void)v;(void)s;(void)i;(void)n;(void)p;removed++;}
static void client_gone(void *v,uint32_t n,uint32_t p){(void)v;(void)n;(void)p;gone++;}
static void incoming(NbQrtr *q,uint32_t type,uint32_t node,uint32_t service){
    memset(packet,0,sizeof packet);w32(packet,1);w32(packet+4,type);w32(packet+8,node);w32(packet+12,NB_QRTR_PORT_CTRL);
    w32(packet+20,20);w32(packet+24,1);w32(packet+28,NB_QRTR_PORT_CTRL);w32(packet+32,type);
    w32(packet+36,service);w32(packet+40,1);w32(packet+44,node);w32(packet+48,99);
    assert(!nb_qrtr_receive(q,packet,52));
}
static void routers(void){
    NbQrtr q;NbQrtrEvents events={0,0,0,server_del,client_gone};assert(!nb_qrtr_init(&q,1,send_packet,0,tx,&events));
    assert(!nb_qrtr_add_service(&q,14,1,77,receive,0));nb_qrtr_link(&q,1);
    incoming(&q,NB_QRTR_HELLO,0,0);assert(last_node==0);incoming(&q,NB_QRTR_NEW_SERVER,0,30);
    incoming(&q,NB_QRTR_HELLO,2,0);assert(last_node==2);incoming(&q,NB_QRTR_NEW_SERVER,2,40);
    assert(q.node_count==2&&q.remote_count==2&&removed==0&&gone==0);
    unsigned sent=emitted;assert(!nb_qrtr_del_client(&q,123)&&emitted==sent+2);
    incoming(&q,NB_QRTR_BYE,0,0);assert(q.node_count==1&&q.remote_count==1&&q.remote[0].node==2&&gone==1&&removed==1);
    const uint8_t msg[]={1};assert(!nb_qrtr_send_data(&q,123,2,99,msg,1)&&last_node==2);
    nb_qrtr_link(&q,0);assert(!q.node_count&&!q.remote_count&&!q.hello_received&&gone==2&&removed==2);
    puts("PASS multi-remote: independent announcements, directed controls, per-node removal");
}
static void ram_store(void){
    uint8_t ram[1024],shared[1024],original[1024],req[128],reply[128];memset(original,0x12,sizeof original);memcpy(ram,original,sizeof ram);memset(shared,0x34,sizeof shared);
    NbRmtfs r={0};r.count=1;r.image[0]=(NbRmtfsImage){"/boot/modem_fs1",ram,sizeof ram};r.shared=shared;r.shared_phys=0x90000000;r.shared_bytes=sizeof shared;
    assert(!nb_rmtfs_init(&r));NbQmiWriter w;assert(!nb_qmi_begin(&w,req,sizeof req,NB_QMI_REQUEST,1,NB_RMTFS_RW_IOVEC));
    nb_qmi_u32(&w,1,1);nb_qmi_u8(&w,2,1);uint8_t list[13]={1};w32(list+1,0);w32(list+5,(uint32_t)r.shared_phys);w32(list+9,1);
    nb_qmi_put(&w,3,list,sizeof list);uint32_t len=nb_qmi_end(&w);assert(nb_rmtfs_handle(&r,req,len,reply,sizeof reply));
    assert(r.writes==1&&ram[0]==0x34&&ram[511]==0x34&&ram[512]==0x12&&original[0]==0x12);
    puts("PASS remote store: writes only modify the RAM copy");
}
static unsigned trace_calls;
static void trace(void *v,const char *stage,int result){(void)v;(void)result;assert(!strcmp(stage,"glink-first-poll"));trace_calls++;}
static void first_poll(void){
    NbMss m={0};uint32_t th=0,tt=0,rh=0,rt=0;uint8_t tx[512]={0},rx[512]={0};
    QcomGlinkFifo a={&th,&tt,tx,sizeof tx},b={&rh,&rt,rx,sizeof rx};qcom_glink_init(&m.glink,a,b,0,0);
    m.stage=NbMssOnline;m.glink_ready=1;m.glink_rx_attached=1;m.ops.trace=trace;
    assert(!nb_mss_poll(&m,0)&&trace_calls==2&&m.glink_polled);
    assert(!nb_mss_poll(&m,1)&&trace_calls==2);
    puts("PASS first GLINK poll: before/after stage evidence emitted once");
}
int main(void){loading();routers();ram_store();first_poll();return 0;}
