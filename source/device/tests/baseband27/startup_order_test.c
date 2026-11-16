/* SPDX-License-Identifier: BSD-2-Clause */
/* The real worker/MSS/SMEM state machines, with counted external IPA/PAS ops. */
#include "../../package/baseband27/platform/NbBaseband.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(4096) uint8_t smem[0x200000], before[0x40000];
static uint8_t qrtr[NB_QRTR_MAX_PACKET], packet[4+NB_TFTP_MAX_BLKSIZE];
static uint8_t rw[NB_TFTP_RW_FILES*NB_TFTP_RW_FILE_BYTES];
static uint8_t intents[NB_IPCRTR_INTENTS*NB_IPCRTR_INTENT_BYTES];
static uint8_t ds[NB_DS_INTENTS*NB_DS_INTENT_BYTES], tx[NB_TX_QUEUE*NB_TX_SLOT_BYTES];
static NbBaseband worker;
static NbPas modem, dtb, ipa;
static NbRmtfs rmtfs;
static unsigned stops, votes, power, mappings, configs;
static unsigned fail_main, fail_dtb;
static void word(unsigned at, uint32_t value) { memcpy(smem+at,&value,4); }
static int lock(void *context) { (void)context;return 0; }
static void unlock(void *context) { (void)context; }
static void bell(void *context,uint32_t signal) { (void)context;(void)signal; }
static int proxy(void *context,int on) { (void)context;assert(on);votes++;return NB_OK; }
static int assign(void *context,int on) { (void)context;(void)on;assert(0);return NB_IO; }
static int pas(void *context,uint32_t cmd,uint32_t image,uint64_t a,uint64_t b)
{
    (void)context;(void)a;(void)b;assert(cmd==6 && !power && !mappings && !configs && votes);
    stops++;
    return (image==4 && fail_main)||(image==0x26 && fail_dtb) ? NB_IO:NB_OK;
}
static int ipa_power(void *context,int on)
{
    (void)context;assert(on && stops==2 && worker.mss.masters_stopped);power++;return NB_OK;
}
static int ipa_smmu(void *context,int on,const NbMss *m,const NbIpaMemory *memory,uint32_t *address)
{
    (void)context;(void)memory;assert(on && m==&worker.mss && power && !configs);
    NbRange region={0};int r=nb_ipa_smem_prepare(m,0x81d00000,&region,address);
    assert(!r && *address==0x81d10030 && region.base==0x81d10000 && region.bytes==0x1a000);
    mappings++;return r;
}
int nb_ipa_configure(const NbIpaBoard *board,const NbIpaMemory *memory)
{
    (void)board;(void)memory;assert(power && mappings && stops==2 && worker.mss.masters_stopped);
    configs++;return 0;
}
void nb_ipa_set_smem(uint32_t size,uint32_t address) { assert(mappings && size==NB_IPA_SMEM_BYTES && address==0x81d10030); }
void nb_ipa_server_new(uint32_t service,uint32_t instance,uint32_t node,uint32_t port) { (void)service;(void)instance;(void)node;(void)port; }
int nb_ipa_client_packet(uint32_t port,uint32_t node,uint32_t from,const uint8_t *data,uint32_t bytes)
{ (void)port;(void)node;(void)from;(void)data;(void)bytes;return 0; }
void nb_ipa_clock_query(void) {}
void nb_ipa_poll(void) {}
void nb_ipa_modem_starting(void) {}
int nb_ipa_setup(void) { assert(0);return -1; }
void nb_ipa_modem_down(void) {}
static int lookup(void *ctx,const char *path,uint64_t *bytes,uint32_t *handle)
{ (void)ctx;(void)path;(void)bytes;(void)handle;return -1; }
static int64_t read_file(void *ctx,uint32_t handle,uint64_t off,void *dst,uint32_t bytes)
{ (void)ctx;(void)handle;(void)off;(void)dst;(void)bytes;return -1; }
static int alloc(void *ctx,uint32_t index,uint32_t bytes,uint64_t *pa)
{ (void)ctx;(void)index;(void)bytes;(void)pa;assert(0);return -1; }
static void release(void *ctx,uint32_t index,uint64_t pa,uint32_t bytes)
{ (void)ctx;(void)index;(void)pa;(void)bytes;assert(0); }
static int shared(void *ctx,uint64_t pa,uint32_t bytes,int on,int common)
{ (void)ctx;(void)pa;(void)bytes;(void)on;(void)common;assert(0);return -1; }

static void fixture(int existing)
{
    memset(smem,0,sizeof smem);memset(&modem,0,sizeof modem);memset(&dtb,0,sizeof dtb);
    memset(&ipa,0,sizeof ipa);memset(&rmtfs,0,sizeof rmtfs);
    stops=votes=power=mappings=configs=fail_main=fail_dtb=0;
    unsigned toc=sizeof smem-4096,part=0x10000,size=0x40000;
    word(toc,0x434f5424);word(toc+4,1);word(toc+8,1);
    word(toc+32,part);word(toc+36,size);word(toc+44,1u<<16);word(toc+48,64);
    word(part,0x54525024);word(part+4,1u<<16);word(part+8,size);word(part+12,32);word(part+16,size);
    if(existing){
        word(part+12,32+16+NB_IPA_SMEM_BYTES);word(part+32,0xa5a5u|(NB_SMEM_IPA_FILTER_TABLE<<16));
        word(part+36,NB_IPA_SMEM_BYTES);memset(smem+part+48,0x5a,NB_IPA_SMEM_BYTES);
    }
    memcpy(before,smem+part,sizeof before);
    modem.pas=4;dtb.pas=0x26;ipa.pas=15;
    modem.stage=dtb.stage=ipa.stage=NbPasInit;
    modem.ops.pas=dtb.ops.pas=pas;
    NbBasebandConfig c={0};
    c.platform=(NbBasebandPlatform){0,ipa_power,ipa_smmu,0};
    c.mss=(NbMssOps){0,lock,unlock,bell,proxy,assign,0,0};
    c.memshare=(NbMemshareOps){0,alloc,release,shared};
    c.tftp=(NbTftpStore){0,lookup,read_file};
    c.smem=smem;c.smem_bytes=sizeof smem;c.smem_physical=0x81d00000;
    c.ipa_fws=&ipa;c.modem=&modem;c.modem_dtb=&dtb;c.rmtfs=&rmtfs;
    c.qrtr_tx=qrtr;c.tftp_packet=packet;c.tftp_files=rw;
    c.ipcrtr_intents=intents;c.ds_intents=ds;c.tx_queue=tx;
    assert(!nb_baseband_init(&worker,&c) && worker.stage==NbBasebandInitialStop);
}

int main(void)
{
    for(int existing=0;existing<=1;existing++){
        fixture(existing);
        for(unsigned now=0;now<3;now++){
            assert(nb_baseband_poll(&worker,now)==NB_PENDING);
            assert(!power && !mappings && !configs && !memcmp(before,smem+0x10000,sizeof before));
        }
        assert(stops==2 && worker.stage==NbBasebandPower);
        assert(nb_baseband_poll(&worker,3)==NB_PENDING && power==1 && !mappings);
        assert(nb_baseband_poll(&worker,4)==NB_PENDING && mappings==1 && !configs);
        assert(nb_baseband_poll(&worker,5)==NB_PENDING && configs==1 && worker.stage==NbBasebandFirmware);
        if(existing)assert(!memcmp(before,smem+0x10000,sizeof before));
    }
    for(unsigned failed=0;failed<2;failed++){
        fixture(0);if(failed)fail_dtb=1;else fail_main=1;
        assert(nb_baseband_poll(&worker,0)==NB_PENDING);
        assert(nb_baseband_poll(&worker,1)==NB_PENDING);
        assert(nb_baseband_poll(&worker,2)==NB_IO && !power && !mappings && !configs);
        assert(!memcmp(before,smem+0x10000,sizeof before));
        assert(nb_baseband_poll(&worker,3)==NB_IO && stops==2);
    }
    puts("PASS startup order: real worker stops both MSS images before IPA power/SMEM/map/firmware; both failed-stop cases leave SMEM intact");
}
