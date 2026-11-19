/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Deferred tests of production state machines. Synthetic callbacks, no MMIO,
 * firmware, audio input or phone access; not compiled/executed in this phase. */
#include "../platform/AdspLink.h"
#include "../platform/AdspPower.h"
#include "../platform/Tas25xxEndpoint.h"
#include "../platform/SoundWire.h"
#include "../core/AudioRuntime.h"
#include <assert.h>
#include <string.h>
typedef struct {unsigned stops,dma,writes,power_calls,powered;uint32_t fail_pas;uint64_t now;int dma_result,io_error,power_error;} Fake;
static int proxy_start(void *v,int on) {
    Fake *f=v;assert(on==1 && !f->stops && !f->dma);
    if (++f->power_calls<=2) return NA_PENDING;
    if (f->power_error) return f->power_error;
    f->powered=1;return NA_OK;
}
static int pas(void *v,uint32_t command,uint32_t subsystem,uint64_t a,uint64_t b) {
    Fake *f=v;assert(f->powered && command==6 && !a && !b);assert(subsystem==(f->stops ? 36u:1u));
    ++f->stops;return subsystem==f->fail_pas ? NA_IO:NA_OK;
}
static int setup_dma(void *v) {Fake *f=v;assert(f->stops==2);++f->dma;return f->dma_result;}
static void startup_order(void) {
    for (unsigned fail=0;fail<4;++fail) {
        Fake f={.fail_pas=fail==1 ? 1:fail==2 ? 36:0,.dma_result=NA_IO,.power_error=fail==3 ? NA_IO:0};
        NaPasLoad main={.ops={.context=&f,.pas=pas}},dtb=main;
        NaAdsp a={.ops={.context=&f,.dma_setup=setup_dma,.proxy=proxy_start},.stage=NaAdspPower,.main_image=&main,.dtb_image=&dtb};
        for (unsigned i=0;i<6 && a.stage!=NaAdspFailed;++i) na_adsp_poll(&a,100+i);
        assert(a.stage==NaAdspFailed && a.error==NA_IO);
        assert(f.power_calls==3 && f.powered==(fail==3 ? 0u:1u));
        assert(f.stops==(fail==3 ? 0u:2u) && f.dma==(fail ? 0u:1u));
        assert(a.initial_stop_mask==(fail==3 ? 0u:3u));
        assert(a.initial_stop[0]==(fail==1 ? NA_IO:0) && a.initial_stop[1]==(fail==2 ? NA_IO:0));
        assert(a.masters_stopped==(fail==0));
        unsigned stops=f.stops,dma=f.dma,power=f.power_calls;
        assert(na_adsp_poll(&a,200)==NA_IO && f.stops==stops && f.dma==dma && f.power_calls==power);
    }
}
static int amp_read(void *v,uint64_t lease,uint8_t addr,uint32_t reg,uint8_t *out) {
    (void)v;(void)lease;(void)addr;(void)reg;*out=0;return NA_OK;
}
static int amp_write(void *v,uint64_t lease,uint8_t addr,uint32_t reg,const uint8_t *data,uint32_t bytes) {
    Fake *f=v;assert(lease==42 && addr==0x4a && reg==2 && bytes==1 && *data==1);++f->writes;return NA_OK;
}
static void repeated_off(void) {
    /* 20ms delay then one write. Repeated Off requests must neither restart
     * this delay nor reset the endpoint's five-second overall deadline. */
    const uint8_t program[]={11,0,0,0,3,20,0,0,0,0,2,0,0,0,1};
    Fake f={0};NaTasEndpoint e={.state=NaTasActive,.target=NaTasActive,.lease=42,.powered=1,.identified=1};
    e.ops.io=(NaTasIo){&f,amp_read,amp_write};e.count[2]=1;
    e.steps[2][0]=(NaTasStep){{program,sizeof program},0,0};
    for (uint64_t now=100;now<122;++now) {
        assert(!na_tas_endpoint_request(&e,NaTasOff));
        int r=na_tas_endpoint_poll(&e,now);assert(r==NA_PENDING || !r);
        assert(e.deadline==5100);
    }
    assert(e.state==NaTasOff && !e.changing && f.writes==1);
    assert(!na_tas_endpoint_request(&e,NaTasOff) && !na_tas_endpoint_poll(&e,1000) && f.writes==1);
}
static uint32_t mmio_read(void *v,uint32_t reg) {(void)v;(void)reg;return 0;}
static void mmio_write(void *v,uint32_t reg,uint32_t value) {(void)reg;(void)value;++((Fake *)v)->writes;}
static uint64_t now_us(void *v) {return ((Fake *)v)->now;}
static void pause_us(void *v,uint32_t us) {((Fake *)v)->now+=us;}
static int mmio_error(void *v) {return ((Fake *)v)->io_error;}
static void bus_error(void) {
    Fake f={.io_error=1};NaSwr s={.io={&f,mmio_read,mmio_write,now_us,pause_us,mmio_error},.lease=42,.device=1};
    assert(na_swr_write(&s,0x120,2)==NA_IO && s.failed && !f.writes);
    f.io_error=0;assert(na_swr_write(&s,0x120,2)==NA_OWNERSHIP && !f.writes);
}
static _Alignas(8) uint64_t old_out[NA_AUDIO_RING_FRAMES],new_out[NA_AUDIO_RING_FRAMES];
static uint64_t host(void *v) {(void)v;return 123456;}
static void client_maps(void) {
    NaAudioStatus old={0},fresh={0};NaPrm prm={0};
    NaRuntime r={.prm=&prm,.shared=&old,.output=old_out,.ops={.host_time=host}};
    r.status=(NaAudioStatus){.magic=NA_AUDIO_MAGIC,.version=NA_AUDIO_ABI_VERSION,.bytes=sizeof old,
        .rate=NA_AUDIO_RATE,.ring_frames=NA_AUDIO_RING_FRAMES,.period_frames=NA_AUDIO_PERIOD_FRAMES,
        .lead_frames=NA_AUDIO_LEAD_FRAMES,.out_channels=2,.epoch=1,.ticks_per_frame_q32=400ULL<<32,.state=NaAudioOutputReady};
    r.request=1;assert(na_runtime_rebind(&r,&fresh,new_out,0)==NA_BUSY);
    r.request=0;prm.possible=1;assert(na_runtime_rebind(&r,&fresh,new_out,0)==NA_BUSY);
    prm.possible=0;assert(!na_runtime_rebind(&r,&fresh,new_out,0));
    assert(!old.state && fresh.epoch==2 && fresh.anchor_host==123456 && r.output==new_out);
    assert(!na_ring_mix(old_out,10,123,456));
    int16_t left=1,right=1;
    assert(na_ring_take(r.output,10,&left,&right)==NA_MISSING && !left && !right);
    assert(na_runtime_rebind(&r,&fresh,new_out,0)==NA_ARGUMENT);
}
static _Alignas(4096) uint8_t out_dma[65536],in_dma[65536];
static int nothing(void *v) {(void)v;return NA_OK;}
static int send_packet(void *v,const uint8_t *p,uint32_t n) {(void)v;(void)p;(void)n;return NA_OK;}
static int sync_buffer(void *v,uint32_t at,uint32_t n,int d) {(void)v;(void)at;(void)n;(void)d;return NA_OK;}
static int activate(void *v,int d) {(void)v;(void)d;return NA_OK;}
static void diagnostic_capture(void) {
    NaSession session;na_session_init(&session);NaRxQueue queue={0};NaTransport transport={0};
    NaStream streams[2];NaGraph graphs[2];NaPrm prm={.count=2};NaRuntime r;NaAudioStatus status={0};
    NaTopology topology={.open={32768,8},.config={{32776,8}},.configs=1,.subgraphs={42},.subgraph_count=1};
    NaGraphOps go={0,send_packet,sync_buffer,nothing,nothing,nothing};
    assert(!na_stream_init(&streams[0],out_dma,0x110000000ULL,sizeof out_dma,1920,4,7,42,0));
    assert(!na_stream_init(&streams[1],in_dma,0x110010000ULL,sizeof in_dma,960,2,8,43,1));
    for (unsigned i=0;i<2;++i) assert(!na_graph_init(&graphs[i],&session,&streams[i],&topology,&go));
    NaRuntimeOps ro={0,host,activate,nothing,nothing,0};
    assert(!na_runtime_init(&r,&session,&queue,&graphs[0],&graphs[1],&prm,&transport,
        &status,old_out,new_out,625,12,1,2,0,&ro));
    r.service_ready=1;
    assert(na_runtime_probe_capture(&r,3,100)==NA_MISSING); /* disabled by default */
    assert(na_runtime_request(&r,NaAudioStartInput)==NA_MISSING); /* HAL cannot bypass proof */
    r.capture_probe_allowed=1;
    assert(na_runtime_probe_capture(&r,96001,100)==NA_ARGUMENT);
    assert(!na_runtime_probe_capture(&r,3,100));
    assert(na_runtime_request(&r,NaAudioStartOutput)==NA_BUSY);
    const int16_t input[4]={-32768,1,0,32767};
    assert(!r.engine.ops.capture(r.engine.ops.context,0,input,sizeof input));
    assert(r.probe.frames==3 && r.probe.nonzero==2 && r.probe.peak==32768 && r.probe.squares==1073741825ULL);
    assert(r.probe.state==NaProbeDraining && !r.capture_proven && !status.in_channels);
    assert(!r.engine.ops.capture(r.engine.ops.context,3,input,sizeof input) && r.probe.frames==3);
}
static void observed_rsc(void) {
    QcomRscLayout layout;
    assert(!na_proxy_layout(0x40300,0x800c8104,&layout));
    assert(layout.tcs_stride==0x2a0 && layout.cmd_stride==24 && layout.cmd_addr==0x38 && layout.cmd_data==0x3c);
    assert(!na_proxy_layout(0x30000,0x800c8104,&layout));
    assert(na_proxy_layout(0x40300,0,&layout)==NA_MISSING);
    assert(na_proxy_layout(0x40300,0xffffffff,&layout)==NA_MISSING);
    assert(na_proxy_layout(0xffffffff,0x800c8104,&layout)==NA_MISSING);
    assert(na_proxy_layout(0x50000,0x800c8104,&layout)==NA_MISSING);
}
int main(void) {startup_order();repeated_off();bus_error();client_maps();diagnostic_capture();observed_rsc();return 0;}
