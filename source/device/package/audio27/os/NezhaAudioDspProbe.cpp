// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit na=2 diagnostic: real ADSP/PAS/GLINK and reset-muted TAS identity.
// No APM graph, PCM producer, HAL publication or microphone activation.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
extern "C" {
#include <QcomUsbDiagnostic.h>
}
#include "NezhaAudioAdspInputs.h"
#include "NezhaAudioSmmu.h"
#include "NezhaAudioTasBoard.h"
extern "C" volatile int nezha_touch_stage;
extern "C" kern_return_t thread_terminate(thread_t);
extern "C" void nezha_audio27_dsp_probe_start(void);
namespace {
struct AudioDspProbe {
    NaNativeAdspInputs input;NaAssemblyPlan plan;
    NaNativeSmmu domain;NaSmmu smmu;
    NaNativeMemory dma,tables;
    NaNativeWindow smem,qmp,cmdDb;
    NaPasLoad loader[2];NaProxyPower power;NaAdsp adsp;NaRxQueue rx;
    NaNativeTasBoard tasNative;NaGeniBoard tasBoard;NaTasPowerOps tasOps;
    uint64_t now,deadline,tasDeadline,tasWait,tasLease;
    unsigned phase,tasPhase,dmaAttempted,started,revision[2],pasQueries;
    unsigned tasClaimed,tasPowerAttempted,tasCleanupAttempted;
    unsigned postPoint;int postResult,headerResult;
    unsigned loadPas,loadPoint,loadSegment;uint64_t loadOffset,loadBytes,loadPhysical;
    int error,tasError,tasCleanup;
};
AudioDspProbe nativeAudioProbe;
uint32_t audioProbeSequence,audioProbeValues[52];
void publish(AudioDspProbe *d) {
    uint32_t fault=0,syndrome=0;uint64_t address=0;
    // IRQ publishes these once through fault_pending; never race its initial
    // non-atomic payload stores when a fault arrives during this snapshot.
    if (__atomic_load_n(&d->domain.fault_pending,__ATOMIC_ACQUIRE)) {
        fault=d->domain.fault_status;syndrome=d->domain.fault_syndrome;address=d->domain.fault_address;
    }
    const uint32_t values[]={d->phase,(uint32_t)d->error,d->adsp.stage,d->adsp.slave,
        d->adsp.handed,d->adsp.transport.online,d->smmu.started,fault,
        d->tasPhase,(uint32_t)d->tasError,(uint32_t)d->tasCleanup,d->revision[0],d->revision[1],
        d->input.resources.error ? 1u:0u,d->power.held,d->power.possible,d->power.stage,d->power.active_resource,
        d->power.last_irq,d->power.last_status,d->power.last_command_status,
        (uint32_t)address,(uint32_t)(address>>32),syndrome,d->power.last_irq_enable,
        d->pasQueries,d->adsp.initial_stop_mask,(uint32_t)d->adsp.initial_stop[0],(uint32_t)d->adsp.initial_stop[1],
        d->loadPas,d->loadPoint,d->loadSegment,(uint32_t)d->loadOffset,(uint32_t)(d->loadOffset>>32),
        (uint32_t)d->loadBytes,(uint32_t)(d->loadBytes>>32),(uint32_t)d->loadPhysical,(uint32_t)(d->loadPhysical>>32),
        d->adsp.retain_proxy,d->adsp.handover_seen,d->adsp.proxy_possible,d->postPoint,(uint32_t)d->postResult,
        d->adsp.glink.header_attempts,d->adsp.glink.last_cmd,d->adsp.glink.last_param1,d->adsp.glink.last_param2,
        (uint32_t)d->headerResult,d->adsp.glink.rx_messages,d->adsp.glink.tx_messages,
        d->adsp.glink.version_agreed,d->adsp.glink.rx.data ? 1u:0u};
    __atomic_add_fetch(&audioProbeSequence,1,__ATOMIC_ACQ_REL);
    for (unsigned i=0;i<52;++i) __atomic_store_n(&audioProbeValues[i],values[i],__ATOMIC_RELAXED);
    __atomic_add_fetch(&audioProbeSequence,1,__ATOMIC_RELEASE);
}
void post_ready(void *v,uint32_t point,int result) {
    auto *d=static_cast<AudioDspProbe *>(v);d->postPoint=point;d->postResult=result;publish(d);
    printf("NEZHA AUDIO27 POST: point=%u result=%d slave=%x seen=%u released=%u retain=%u held=%x rx=%u\n",
        point,result,d->adsp.slave,d->adsp.handover_seen,d->adsp.handed,d->adsp.retain_proxy,d->power.held,d->adsp.glink.rx.length);
}
void glink_header(void *v,uint32_t attempt,uint16_t command,uint16_t param1,uint32_t param2,int result) {
    auto *d=static_cast<AudioDspProbe *>(v);d->headerResult=result;publish(d);
    printf("NEZHA AUDIO27 GLINK: attempt=%u cmd=%u p1=%u p2=%#x result=%d\n",attempt,command,param1,param2,result);
}
void loader_progress(void *v,uint32_t pas,uint32_t point,uint32_t segment,uint64_t offset,uint64_t bytes,uint64_t pa) {
    auto *d=static_cast<AudioDspProbe *>(v);
    d->loadPas=pas;d->loadPoint=point;d->loadSegment=segment;d->loadOffset=offset;d->loadBytes=bytes;d->loadPhysical=pa;
    // Publish BEFORE a potentially blocking copy/sync/auth call, using the
    // existing snapshot seqlock. No second trace buffer or hardware reader.
    publish(d);
    if (!offset || point==NaPasAuthBegin || point==NaPasAuthReturn)
        printf("NEZHA AUDIO27 LOAD: pas=%u point=%u segment=%u offset=%llu bytes=%llu pa=%#llx\n",
            pas,point,segment,(unsigned long long)offset,(unsigned long long)bytes,(unsigned long long)pa);
}
int snapshot(void *,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    uint32_t v[52];bool valid=false;
    for (unsigned attempt=0;attempt<4;++attempt) {
        uint32_t seq=__atomic_load_n(&audioProbeSequence,__ATOMIC_ACQUIRE);if (seq&1) continue;
        for (unsigned i=0;i<52;++i) v[i]=__atomic_load_n(&audioProbeValues[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq==__atomic_load_n(&audioProbeSequence,__ATOMIC_RELAXED)) {valid=true;break;}
    }
    *bytes=0;*truncated=0;if (!valid) return QUD_BUSY;
    int n=snprintf(out,capacity,"{\"v\":2,\"mode\":\"dsp-tas-identity\",\"phase\":%u,\"error\":%d,"
        "\"adsp_stage\":%u,\"slave\":%u,\"handover\":%u,\"gpr_online\":%u,\"smmu\":%u,"
        "\"fault\":%u,\"tas_phase\":%u,\"tas_error\":%d,\"tas_cleanup\":%d,\"revision\":[%u,%u],"
        "\"owner_error\":%u,\"proxy_held\":%u,\"proxy_possible\":%u,\"proxy_stage\":%u,\"proxy_resource\":%u,"
        "\"tcs_irq\":%u,\"tcs_status\":%u,\"tcs_command\":%u,\"fault_address\":\"0x%llx\","
        "\"fault_syndrome\":%u,\"tcs_irq_enable\":%u,\"pas_queries\":%u,\"stop_mask\":%u,"
        "\"stop_main\":%d,\"stop_dtb\":%d,\"load_pas\":%u,\"load_point\":%u,\"load_segment\":%u,"
        "\"load_offset\":%llu,\"load_bytes\":%llu,\"load_pa\":\"0x%llx\","
        "\"retain_proxy\":%u,\"handover_seen\":%u,\"adsp_proxy_possible\":%u,\"post_point\":%u,\"post_result\":%d,"
        "\"glink_attempts\":%u,\"glink_cmd\":%u,\"glink_p1\":%u,\"glink_p2\":%u,\"glink_result\":%d,"
        "\"glink_rx\":%u,\"glink_tx\":%u,\"glink_version\":%u,\"glink_rx_bound\":%u,\"PCM\":false,\"HAL\":false}",
        v[0],(int32_t)v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],(int32_t)v[9],(int32_t)v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17],v[18],v[19],v[20],
        (unsigned long long)(((uint64_t)v[22]<<32)|v[21]),v[23],v[24],v[25],v[26],(int32_t)v[27],(int32_t)v[28],v[29],v[30],v[31],
        (unsigned long long)(((uint64_t)v[33]<<32)|v[32]),(unsigned long long)(((uint64_t)v[35]<<32)|v[34]),
        (unsigned long long)(((uint64_t)v[37]<<32)|v[36]),
        v[38],v[39],v[40],v[41],(int32_t)v[42],v[43],v[44],v[45],v[46],(int32_t)v[47],v[48],v[49],v[50],v[51]);
    if (n<0 || (size_t)n>=capacity) {*truncated=1;return QUD_FAILED;}
    *bytes=(size_t)n;return QUD_OK;
}
const qud_diag_provider dspProvider={8,2,1u<<QUD_SNAPSHOT,0,0,"nezhaaudio",nullptr,snapshot,nullptr};
int smem_lock(void *v) {return na_resource_owner_smem_lock(&static_cast<AudioDspProbe *>(v)->input.resources);}
void smem_unlock(void *v) {na_resource_owner_smem_unlock(&static_cast<AudioDspProbe *>(v)->input.resources);}
void doorbell(void *v,uint32_t signal) {na_resource_owner_doorbell(&static_cast<AudioDspProbe *>(v)->input.resources,signal);}
int proxy(void *v,int on) {
    auto *d=static_cast<AudioDspProbe *>(v);int r=na_proxy_request(&d->power,on);
    return r ? r:na_proxy_poll(&d->power,d->now);
}
int dma_setup(void *v) {
    auto *d=static_cast<AudioDspProbe *>(v);
    if (!d->adsp.masters_stopped) return NA_OWNERSHIP;
    if (d->smmu.started) return NA_OK;
    if (d->dmaAttempted) return NA_OWNERSHIP;
    d->dmaAttempted=1;
    const NaRange physical={d->dma.physical,d->dma.bytes};const uint32_t iova=0x40000000;
    const NaRange reserved[]={{d->input.metadata[0].physical,d->input.metadata[0].bytes},
                              {d->input.metadata[1].physical,d->input.metadata[1].bytes}};
    int r=na_smmu_start(&d->smmu,&d->plan.smmu,&physical,&iova,1,d->tables.va,d->tables.physical,
        (uint32_t)(d->tables.bytes/4096),reserved,2);
    if (r==NA_BUSY && !d->smmu.quarantine) d->dmaAttempted=0;
    return r;
}
int poll_shared(void *v) {
    auto *d=static_cast<AudioDspProbe *>(v);int r=na_native_smmu_faulted(&d->domain);if (r) return r;
    return na_resource_owner_dispatch(&d->input.resources);
}
int prepare(AudioDspProbe *d) {
    int r=na_native_prepare_adsp_inputs(&d->input,&d->plan);if (r) return r;
    d->input.progress_context=d;d->input.progress=loader_progress;
    uint64_t lease=d->plan.adsp_lease;
    r=na_native_map(&d->smem,{0x81d00000,0x200000},lease);if (r) return r;
    r=na_native_map(&d->qmp,{0xc300000,0x1000},lease);if (r) return r;
    r=na_native_map(&d->cmdDb,{0x81c60000,0x20000},lease);if (r) return r;
    r=na_native_allocate(&d->dma,65536);if (r) return r;
    r=na_native_allocate(&d->tables,64*4096);if (r) return r;
    r=na_native_smmu_bind(&d->domain,&d->input.resources,&d->plan.smmu);if (r) return r;
    const NaRange region[]={na_nezha_facts.firmware,na_nezha_facts.dtb};
    for (unsigned i=0;i<2;++i) {
        auto &m=d->input.metadata[i];
        r=na_pas_begin(&d->loader[i],&d->input.firmware[i],i ? 36:1,region[i],d->plan.firmware_map[i],
            m.va,m.physical,m.bytes,&d->plan.pas,d->now);if (r) return r;
    }
    NaProxyOps power=na_resource_owner_proxy(&d->input.resources);
    r=na_proxy_init(&d->power,reinterpret_cast<volatile uint32_t *>(d->qmp.va),0x400,
        const_cast<const uint8_t *>(d->cmdDb.va),0x20000,1,&power);if (r) return r;
    NaAdspOps ops={d,smem_lock,smem_unlock,doorbell,proxy,dma_setup,poll_shared,post_ready};
    r=na_adsp_init(&d->adsp,d->smem.va,0x200000,&d->loader[0],&d->loader[1],&d->rx,&ops);if (r) return r;
    // Audio016 diagnostic: keep the already-requested startup votes. This is
    // not a power-management fix and never claims that the votes were released.
    d->adsp.retain_proxy=1;
    return na_resource_owner_ipcc(&d->input.resources,&d->adsp);
}
void tas_cleanup(AudioDspProbe *d) {
    if (!d->tasLease || d->tasCleanupAttempted) return;
    d->tasCleanupAttempted=1;
    // Same existing GPIO/GENI shutdown methods; never execute tuning or an
    // activation program with an as-yet-unknown revision.
    int reset=d->tasOps.reset(d->tasOps.context,d->tasLease,1);
    int power=d->tasPowerAttempted ? d->tasOps.power(d->tasOps.context,d->tasLease,0):0;
    d->tasCleanup=reset ? reset:power;
    if (!d->tasClaimed && !d->tasCleanup) d->tasCleanup=NA_OWNERSHIP;
    if (!d->tasCleanup) d->tasCleanup=d->tasOps.release(d->tasOps.context,d->tasLease);
    if (!d->tasCleanup) d->tasLease=0;
}
int identify_tas(AudioDspProbe *d) {
    int r=0;
    if (d->tasPhase==0) {
        NaBoardIo board;
        r=na_native_tas_board(&d->tasNative,d->input.resources.common,&board);if (r) return r;
        r=na_geni_board_init(&d->tasBoard,&board,&d->tasOps);if (r) return r;
        r=d->tasOps.claim(d->tasOps.context,&d->tasLease);if (r) return r;
        d->tasClaimed=1;
        r=d->tasOps.reset(d->tasOps.context,d->tasLease,1);if (r) return r;
        d->tasPowerAttempted=1;
        r=d->tasOps.power(d->tasOps.context,d->tasLease,1);if (r) return r;
        d->tasDeadline=d->now+5000;d->tasWait=d->now+20;d->tasPhase=1;return NA_PENDING;
    }
    if (d->now>=d->tasDeadline) return NA_TIMEOUT;
    if (d->now<d->tasWait) return NA_PENDING;
    if (d->tasPhase==1) {
        r=d->tasOps.reset(d->tasOps.context,d->tasLease,0);if (r) return r;
        d->tasWait=d->now+20;d->tasPhase=2;return NA_PENDING;
    }
    if (d->tasPhase==2) {
        for (unsigned i=0;i<2;++i) {
            uint8_t revision=0;
            r=d->tasOps.io.read(d->tasOps.io.context,d->tasLease,(uint8_t)na_nezha_facts.amp_address[i],0x78,&revision);
            if (r) return r;d->revision[i]=revision;
        }
        tas_cleanup(d);if (d->tasCleanup) return d->tasCleanup;
        d->tasPhase=3;return NA_OK;
    }
    return d->tasPhase==3 ? NA_OK:NA_OWNERSHIP;
}
void crash_reason(AudioDspProbe *d) {
    if (d->adsp.error!=NA_REMOTE || !(d->adsp.slave&1) ||
        __atomic_load_n(&d->adsp.doorbells,__ATOMIC_ACQUIRE)==d->adsp.auth_doorbells) return;
    QcomSmemPartition global;uint32_t at=0,bytes=0;
    int r=qcom_smem_partition(&d->adsp.smem,QCOM_SMEM_HOST_GLOBAL,QCOM_SMEM_HOST_GLOBAL,&global);
    if (!r) r=qcom_smem_find(&d->adsp.smem,&global,423,&at,&bytes);
    if (r || !bytes || at>d->adsp.smem.size || bytes>d->adsp.smem.size-at) {
        printf("NEZHA AUDIO27 DSP: post-auth fatal, bounded SMEM crash reason unavailable\n");return;
    }
    char message[161];unsigned n=0,limit=bytes<160 ? bytes:160;
    while (n<limit) {uint8_t c=d->adsp.smem.base[at+n];if (!c) break;message[n++]=c>=32 && c<=126 ? (char)c:'.';}
    message[n]=0;
    printf("NEZHA AUDIO27 DSP: SMEM423 after fresh post-auth fatal (bounded snapshot): %s\n",message);
}
void run_probe(void *v,wait_result_t) {
    auto *d=static_cast<AudioDspProbe *>(v);d->deadline=na_native_now_us(nullptr)/1000+120000;
    unsigned last=UINT32_MAX,lastPhase=UINT32_MAX,lastSlave=UINT32_MAX,lastGpr=UINT32_MAX,lastHeld=UINT32_MAX;int lastError=0;
    for (;;) {
        d->now=na_native_now_us(nullptr)/1000;int r=NA_PENDING;
        if (d->phase==1 && __atomic_load_n(&nezha_touch_stage,__ATOMIC_ACQUIRE)==7) {
            r=prepare(d);
            if (r==NA_BUSY && !d->input.attempted) r=NA_PENDING;
            else if (!r) {d->phase=2;d->deadline=d->now+45000;}
        } else if (d->phase>=2 && d->phase<=4) {
            if (d->adsp.glink_initialized && !d->adsp.glink.observe_header) {
                d->adsp.glink.observe_context=d;d->adsp.glink.observe_header=glink_header;
            }
            r=na_adsp_poll(&d->adsp,d->now);
            if (d->adsp.stage==NaAdspStopMain && !d->pasQueries) {
                // Same boot after validated proxy power; four bounded read-only
                // calls, no arbitrary SMC selector exposed over USB. Neither
                // query success nor unsupported queries replace PAS stop proof.
                const unsigned ids[]={1,36},commands[]={7,9};
                for (unsigned i=0;i<2;++i) for (unsigned j=0;j<2;++j) {
                    uint64_t raw[4]={};
                    (void)na_native_pas_query(commands[j],ids[i],raw);
                    d->pasQueries|=1u<<(2*i+j);
                }
            }
            if (!r && (d->adsp.handed || (d->adsp.retain_proxy && d->adsp.handover_seen)) &&
                d->adsp.transport.online && d->phase==2) d->phase=3;
            if (r>=0 && d->phase==3) {
                int tas=identify_tas(d);
                if (tas<0) {d->tasError=tas;tas_cleanup(d);d->phase=4;}
                else if (!tas) d->phase=4;
            }
        }
        if (r<0 || (d->phase<4 && d->now>=d->deadline)) {d->error=r<0 ? r:NA_TIMEOUT;d->phase=5;tas_cleanup(d);crash_reason(d);}
        publish(d);
        if (last!=d->adsp.stage || lastPhase!=d->phase || lastError!=d->error || lastSlave!=d->adsp.slave || lastGpr!=d->adsp.transport.online || lastHeld!=d->power.held) {
            printf("NEZHA AUDIO27 DSP: phase=%u error=%d ADSP=%u slave=%x handover=%u GPR=%u TAS=%u/%u error=%d seen=%u retain=%u held=%x\n",
                d->phase,d->error,d->adsp.stage,d->adsp.slave,d->adsp.handed,d->adsp.transport.online,d->revision[0],d->revision[1],d->tasError,d->adsp.handover_seen,d->adsp.retain_proxy,d->power.held);
            last=d->adsp.stage;lastPhase=d->phase;lastError=d->error;lastSlave=d->adsp.slave;lastGpr=d->adsp.transport.online;lastHeld=d->power.held;
        }
        if (d->phase==5) break;
        assert_wait_timeout((event_t)d,THREAD_UNINT,1,kMillisecondScale);thread_block(THREAD_CONTINUE_NULL);
    }
    // Static state, firmware mappings, DMA and leases remain retained. The
    // existing USB path continues; no automatic reset or PAS replay follows.
    thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
}
extern "C" void nezha_audio27_dsp_probe_start(void) {
    auto *d=&nativeAudioProbe;
    if (__atomic_exchange_n(&d->started,1,__ATOMIC_ACQ_REL)) return;
    d->phase=1;publish(d);
    if (!qcom_usb_diag_register(&dspProvider)) {d->phase=5;d->error=NA_BUSY;publish(d);return;}
    thread_t thread=THREAD_NULL;
    if (kernel_thread_start(run_probe,d,&thread)!=KERN_SUCCESS) {d->phase=5;d->error=NA_IO;publish(d);return;}
    thread_deallocate(thread);
}
