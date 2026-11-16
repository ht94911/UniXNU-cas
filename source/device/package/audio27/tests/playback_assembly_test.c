/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Full production cold Assembly, real saved input pack, synthetic CPU-owned
 * allocations/SMEM/cmd-db. Firmware mappings are PROT_NONE. Any PAS, hardware
 * write, doorbell or endpoint claim during preparation aborts this test.
 * Does not run the worker or claim DSP/target-XNU/hardware verification. */
#include "../platform/AudioPlaybackInputs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static _Alignas(4096) uint8_t smem[0x200000];
static uint8_t db[0x20000];static uint32_t qmp[256];
static NaAudioStatus status;
static uint64_t ring[NA_AUDIO_RING_FRAMES],input_ring[NA_AUDIO_RING_FRAMES];
static unsigned speaker_context,microphone_context;
static uint64_t microphone_trace_us;
static unsigned metadata_cleans;
static void wr(uint8_t *p,uint32_t v) {memcpy(p,&v,4);}
static void w16(uint8_t *p,uint16_t v) {memcpy(p,&v,2);}
static int forbidden(void *v) {(void)v;abort();}
static void forbidden_void(void *v) {(void)v;abort();}
static void doorbell(void *v,uint32_t signal) {(void)v;(void)signal;abort();}
static int pas(void *v,uint32_t cmd,uint32_t id,uint64_t a,uint64_t b) {(void)v;(void)cmd;(void)id;(void)a;(void)b;abort();}
static int clean(void *v,void *cpu,uint64_t pa,uint64_t bytes) {(void)v;assert(cpu && pa && bytes);++metadata_cleans;return NA_OK;}
static int acquire(void *v,uint32_t id,uint64_t *lease) {(void)v;(void)id;(void)lease;abort();}
static int release(void *v,uint32_t id,uint64_t lease) {(void)v;(void)id;(void)lease;abort();}
static uint32_t read_rsc(void *v,uint32_t at) {(void)v;assert(at==0 || at==12);return at ? 0x800c8104:0x40300;}
static void write_rsc(void *v,uint32_t at,uint32_t value) {(void)v;(void)at;(void)value;abort();}
static int read_board(void *v,uint64_t lease,uint64_t pa,uint32_t *out) {(void)v;(void)lease;(void)pa;(void)out;abort();}
static int update_board(void *v,uint64_t lease,uint64_t pa,uint32_t mask,uint32_t value) {(void)v;(void)lease;(void)pa;(void)mask;(void)value;abort();}
static int rails(void *v,uint64_t lease,int on) {(void)v;(void)lease;(void)on;abort();}
static uint64_t host(void *v) {(void)v;return 2361600;}
static void pause_us(void *v,uint32_t us) {(void)v;(void)us;abort();}
static int sync_dma(void *v,const NaOwnedBuffer *b,uint32_t at,uint32_t n,int device) {(void)v;(void)b;(void)at;(void)n;(void)device;abort();}
static NaOwnedBuffer allocate(uint32_t bytes,uint64_t pa) {
    void *p=aligned_alloc(4096,bytes);assert(p);memset(p,0,bytes);return (NaOwnedBuffer){p,{pa,bytes}};
}
static void shared_inputs(void) {
    uint32_t toc=sizeof smem-4096,part=0x10000,size=0x10000;
    wr(smem+toc,0x434f5424);wr(smem+toc+4,1);wr(smem+toc+8,1);
    wr(smem+toc+32,part);wr(smem+toc+36,size);wr(smem+toc+44,2);wr(smem+toc+48,64);
    wr(smem+part,0x54525024);wr(smem+part+4,2);wr(smem+part+8,size);wr(smem+part+12,32);wr(smem+part+16,size);
    wr(db,1);wr(db+4,QCOM_CMD_DB_MAGIC);w16(db+8,3);w16(db+12,0x200);w16(db+14,3);
    const char *names[]={"LP0","lcx.lvl","lmx.lvl"};
    const uint16_t levels[]={0,16,48,64,128,192,256,384};
    for (unsigned i=0;i<3;++i) {
        uint8_t *e=db+144+24*i;memcpy(e,names[i],strlen(names[i]));wr(e+16,0x30000+0x10*i);w16(e+20,16);w16(e+22,32*i);
        for (unsigned j=0;j<8;++j) w16(db+144+0x200+32*i+2*j,levels[j]);
    }
}
int main(int argc,char **argv) {
    assert(argc==5 || (argc==6 && !strcmp(argv[5],"capture")));int capture=argc==6;
    FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long bytes=ftell(f);assert(bytes>0 && bytes<=0x8000000);rewind(f);
    void *blob=malloc((size_t)bytes);assert(blob && fread(blob,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    NaInputPack pack;assert(!na_input_pack(&pack,blob,(uint64_t)bytes));
    NaTasSelection choice={.revision={0x51,0x52},.revision_mask={0xff,0xff},.profile=argv[2],.rx_format=argv[3],.tx_format=argv[4]};
    NaPlaybackInputs input={0};assert(!na_playback_inputs(&input,&pack,&choice));
    NaAssemblyPlan p={0};assert(!na_playback_plan(&p,&input));shared_inputs();
    p.adsp_lease=42;p.smmu.lease=43;p.smem=smem;p.qmp=qmp;p.cmd_db=db;p.tcs=1;
    p.pas=(NaPasOps){.pas=pas,.sync=clean};p.proxy=(NaProxyOps){0,acquire,release,read_rsc,write_rsc,forbidden_void};
    p.board=(NaBoardIo){.context=&speaker_context,.claim=acquire,.release=release,.read=read_board,.update=update_board,.now_us=host,.pause_us=pause_us};
    p.ops=(NaAssemblyOps){0,forbidden,forbidden_void,doorbell,sync_dma,host,forbidden};
    p.status=&status;p.output_ring=ring;p.timebase_numer=625;p.timebase_denom=12;
    p.resources[0]=(NaPrmResource){NaPrmClock,0x200,3072000,4};p.resource_count=p.output_votes=1;
    NaCaptureInputs microphone={0};
    if (capture) {
        assert(!na_capture_inputs(&microphone,&pack) && !na_capture_plan(&p,&microphone));
        p.capture_board=(NaBoardIo){.context=&microphone_context,.claim=acquire,.release=release,.read=read_board,.update=update_board,.mic_rails=rails,.now_us=host,.pause_us=pause_us,.trace_wait_us=&microphone_trace_us};
        p.resources[1]=(NaPrmResource){NaPrmCore,1,0,0};p.resources[2]=(NaPrmResource){NaPrmCore,2,0,0};
        p.resources[3]=(NaPrmResource){NaPrmClock,0x30c,19200000,1};p.resources[4]=(NaPrmResource){NaPrmClock,0x307,19200000,1};
        p.resource_count=5;p.input_votes=0x1e;p.input_ring=input_ring;
        p.stream[1].dma=allocate(65536,0x60600000);p.stream[1].iova=0x40020000;p.stream[1].port=0x7001;
    }
    NaFirmwarePiece pieces[2][QCOM_MDT_MAX_SEGMENTS],mdt[2];
    for (unsigned i=0;i<2;++i) {
        assert(!na_input_firmware(&pack,i,pieces[i],QCOM_MDT_MAX_SEGMENTS,&p.firmware_count[i],&mdt[i]));p.firmware[i]=pieces[i];
        p.metadata[i]=allocate((uint32_t)((mdt[i].bytes+4095)&~UINT64_C(4095)),0x60100000+0x100000*i);
        p.firmware_map[i]=mmap(0,(size_t)(i ? na_nezha_facts.dtb.bytes:na_nezha_facts.firmware.bytes),PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(p.firmware_map[i]!=MAP_FAILED);
        p.stream[i ? 2:0].dma=allocate(65536,0x60300000+0x100000*i);
        p.stream[i ? 2:0].iova=0x40000000+65536*i;p.stream[i ? 2:0].port=0x7000+2*i;
    }
    p.tables=allocate(64*4096,0x60500000);memset(p.tables.cpu,0xa5,p.tables.physical.bytes);
    NaAssembly *a=calloc(1,sizeof *a);assert(a);int r=na_assembly_init(a,&p,123);
    if (r) fprintf(stderr,"assembly result=%d\n",r);assert(!r);
    assert(a->initialized && a->driver.feedback_attached && a->dma_count==(capture ? 3u:2u) && metadata_cleans==2);
    assert(!a->smmu.started && !a->dma_attempted && a->driver.input_enabled==(unsigned)capture && !a->driver.runtime.service_ready && !a->driver.speaker->lease);
    if (capture) assert(a->mic_board.adc_gain==6 && a->mic_board.micbias==1);
    assert(a->driver.graph[0].stage==NaGraphOff && a->driver.graph[2].stage==NaGraphOff);
    assert(!a->driver.runtime.capture_proven && !a->driver.runtime.status.in_channels && !(a->driver.runtime.status.state&NaAudioInputReady));
    if (capture) {
        assert(a->driver.graph[1].stream && a->driver.graph[1].stage==NaGraphOff && a->driver.runtime.capture_probe_allowed);
        assert(a->mic_board.io.context==&microphone_context && a->speaker_board.io.context==&speaker_context);
        assert(a->driver.graph[1].endpoint_trace_wait_us==&microphone_trace_us);
        for (unsigned i=0;i<3;++i) assert(a->iova[i]==0x40000000+65536*i);
    } else assert(!a->driver.graph[1].stream && !a->driver.runtime.capture_probe_allowed);
    assert(a->driver.protection_instance==input.protection[0] && a->driver.graph[0].topology.configs==5);
    for (unsigned i=0;i<2;++i) assert(!memcmp(p.metadata[i].cpu,mdt[i].data,(size_t)mdt[i].bytes));
    for (unsigned i=0;i<p.tables.physical.bytes;++i) assert(((uint8_t *)p.tables.cpu)[i]==0xa5);
    assert(na_assembly_init(a,&p,124)==NA_ARGUMENT);
    for (unsigned i=0;i<2;++i) {free(p.metadata[i].cpu);free(p.stream[i ? 2:0].dma.cpu);munmap(p.firmware_map[i],(size_t)(i ? na_nezha_facts.dtb.bytes:na_nezha_facts.firmware.bytes));}
    if (capture) free(p.stream[1].dma.cpu);
    free(p.tables.cpu);free(a);free(blob);
    puts("real cold Assembly PASS: exact firmware metadata and graphs; optional input DMA with separate microphone owner; no PAS/MMIO/endpoint activation or HAL input publication");
}
