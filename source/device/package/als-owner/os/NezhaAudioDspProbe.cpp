// SPDX-License-Identifier: BSD-2-Clause-Patent
// na=2 diagnoses ADSP/PAS/GLINK; na=4 reads reset-muted TAS identity alone.
// No APM graph, PCM producer, HAL publication or microphone activation.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
extern "C" {
#include <QcomUsbDiagnostic.h>
}
#include "NezhaAudioAdspInputs.h"
#ifdef NEZHA_ALS_NATIVE_OWNER
#include "NezhaAlsNativeCaller.h"
#endif
#include "NezhaAudioSmmu.h"
#include "NezhaAudioTasBoard.h"
#include "NezhaAudioXpuTrace.h"
#include <Sm8850ResourcesNative.h>
extern "C" volatile int nezha_touch_stage;
extern "C" kern_return_t thread_terminate(thread_t);
extern "C" IOReturn armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *);
extern "C" void nezha_audio27_dsp_probe_start(void);
extern "C" void nezha_audio27_tas_probe_start(void);
namespace {
struct AudioDspProbe {
    NaNativeAdspInputs input;NaAssemblyPlan plan;
    NaNativeSmmu domain;NaSmmu smmu;
    NaNativeMemory dma,tables;
    NaNativeWindow smem,qmp,cmdDb;NaNativeAdspWatchdog watchdog;
    NaPasLoad loader[2];NaProxyPower power;NaAdsp adsp;NaRxQueue rx;
    NaNativeTasBoard tasNative;NaGeniBoard tasBoard;NaTasPowerOps tasOps;
    uint64_t now,deadline,tasDeadline,tasWait,tasLease;
    unsigned phase,tasPhase,dmaAttempted,started,revision[2],pasQueries,mode;
    unsigned tasClaimed,tasPowerAttempted,tasCleanupAttempted;
    unsigned postPoint;int postResult,headerResult;
    unsigned loadPas,loadPoint,loadSegment;uint64_t loadOffset,loadBytes,loadPhysical;
    uint32_t watchdogStops,watchdogStopAttempted;
    int watchdogMain,watchdogDtb;
    char crashBaseline[161],crashLast[161];
    uint32_t crashBaselinePresent,crashLastPresent,crashLogs;
    uint64_t crashNext;
    int error,tasError,tasCleanup;
};
AudioDspProbe nativeAudioProbe;
uint32_t audioProbeSequence,audioProbeValues[79];
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
        d->adsp.glink.version_agreed,d->adsp.glink.rx.data ? 1u:0u,
        d->input.resources.common ? d->input.resources.common->ipcc_unknown_count:0u,
        d->input.resources.common ? d->input.resources.common->ipcc_last_unknown:0u,
        d->adsp.mapper.online,d->adsp.mapper.qrtr.hello_received,d->adsp.mapper.qrtr.rx_packets,
        d->adsp.mapper.qrtr.tx_packets,d->adsp.mapper.tx_sent,d->adsp.mapper.qrtr.rx_bad,
        d->adsp.mapper.qrtr.tx_refused,d->adsp.mapper.qrtr.remote_count,
        d->adsp.mapper.requests,d->adsp.mapper.answers,d->adsp.mapper.last_transaction,
        (uint32_t)d->adsp.mapper.error,d->adsp.gpr_initiator,
        d->smmu.stream_result.context.fault_status,(uint32_t)d->smmu.error,
        d->adsp.glink_delay_ms,(uint32_t)(d->now<d->adsp.glink_after ? d->adsp.glink_after-d->now:0),
        __atomic_load_n(&d->adsp.watchdogs,__ATOMIC_ACQUIRE),d->adsp.auth_watchdogs,
        d->watchdog.irq,d->watchdogStops,(uint32_t)d->watchdogMain,(uint32_t)d->watchdogDtb,
        d->adsp.glink_observe_ms,(uint32_t)(d->now<d->adsp.glink_observe_until ? d->adsp.glink_observe_until-d->now:0)};
    static_assert(sizeof values==sizeof audioProbeValues,"Diagnostic word layout mismatch");
    __atomic_add_fetch(&audioProbeSequence,1,__ATOMIC_ACQ_REL);
    for (unsigned i=0;i<79;++i) __atomic_store_n(&audioProbeValues[i],values[i],__ATOMIC_RELAXED);
    __atomic_add_fetch(&audioProbeSequence,1,__ATOMIC_RELEASE);
}
void glink_header(void *,uint32_t,uint16_t,uint16_t,uint32_t,int);
void capture_crash_baseline(AudioDspProbe *);
void glink_fifo_state(AudioDspProbe *d,uint32_t point) {
    uint32_t at=0,bytes=0;
    int r=qcom_smem_find(&d->adsp.smem,&d->adsp.partition,478,&at,&bytes);
    if (r || bytes<16 || (at&3) || at>d->adsp.smem.size || bytes>d->adsp.smem.size-at) {
        printf("NEZHA AUDIO27 GLINK FIFO: point=%u present=0 lookup=%d\n",point,r);
        return;
    }
    // Read only the existing descriptor, before/after this worker's normal
    // setup/poll/notification. DSP-owned indices can legitimately change
    // between the four loads; this is a bounded observation, not a lock or
    // a claim that the peer has accepted any packet.
    auto *desc=reinterpret_cast<volatile uint32_t *>(d->adsp.smem.base+at);
    const uint32_t txTail=desc[0],txHead=desc[1],rxTail=desc[2],rxHead=desc[3];
    printf("NEZHA AUDIO27 GLINK FIFO: point=%u present=1 tx-tail=%u tx-head=%u rx-tail=%u rx-head=%u tx=%u rx=%u version=%u\n",
        point,txTail,txHead,rxTail,rxHead,d->adsp.glink.tx_messages,d->adsp.glink.rx_messages,d->adsp.glink.version_agreed);
}
void post_ready(void *v,uint32_t point,int result) {
    auto *d=static_cast<AudioDspProbe *>(v);d->postPoint=point;d->postResult=result;publish(d);
    if (point==NaAdspGlinkDeferred || point==NaAdspGlinkSetupBegin || point==NaAdspGlinkSetupReturn ||
        point==NaAdspGlinkBegin || point==NaAdspGlinkReturn ||
        point==NaAdspGlinkKickBegin || point==NaAdspGlinkKickReturn) glink_fifo_state(d,point);
    if (point==NaAdspGlinkSetupReturn && !result) {
        // Register the existing header observer before the first GLINK poll.
        d->adsp.glink.observe_context=d;d->adsp.glink.observe_header=glink_header;
    }
    if (point==NaAdspGlinkDeferred && d->watchdog.pdc.va) {
        // Same existing observer, one read after fresh authentication. The
        // direct watchdog pin's CFG remains diagnostic, never programmed.
        auto *pdc=reinterpret_cast<volatile uint32_t *>(d->watchdog.pdc.va);
        printf("NEZHA AUDIO27 WDOG PDC READONLY: after-auth pin=6 cfg=%#x\n",
            pdc[(0x110+4*6)/4]);
    }
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
    uint32_t v[79];bool valid=false;
    for (unsigned attempt=0;attempt<4;++attempt) {
        uint32_t seq=__atomic_load_n(&audioProbeSequence,__ATOMIC_ACQUIRE);if (seq&1) continue;
        for (unsigned i=0;i<79;++i) v[i]=__atomic_load_n(&audioProbeValues[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq==__atomic_load_n(&audioProbeSequence,__ATOMIC_RELAXED)) {valid=true;break;}
    }
    *bytes=0;*truncated=0;if (!valid) return QUD_BUSY;
    int n=snprintf(out,capacity,"{\"v\":2,\"mode\":\"%s\",\"phase\":%u,\"error\":%d,"
        "\"adsp_stage\":%u,\"slave\":%u,\"handover\":%u,\"gpr_online\":%u,\"smmu\":%u,"
        "\"fault\":%u,\"tas_phase\":%u,\"tas_error\":%d,\"tas_cleanup\":%d,\"revision\":[%u,%u],"
        "\"owner_error\":%u,\"proxy_held\":%u,\"proxy_possible\":%u,\"proxy_stage\":%u,\"proxy_resource\":%u,"
        "\"tcs_irq\":%u,\"tcs_status\":%u,\"tcs_command\":%u,\"fault_address\":\"0x%llx\","
        "\"fault_syndrome\":%u,\"tcs_irq_enable\":%u,\"pas_queries\":%u,\"stop_mask\":%u,"
        "\"stop_main\":%d,\"stop_dtb\":%d,\"load_pas\":%u,\"load_point\":%u,\"load_segment\":%u,"
        "\"load_offset\":%llu,\"load_bytes\":%llu,\"load_pa\":\"0x%llx\","
        "\"retain_proxy\":%u,\"handover_seen\":%u,\"adsp_proxy_possible\":%u,\"post_point\":%u,\"post_result\":%d,"
        "\"glink_attempts\":%u,\"glink_cmd\":%u,\"glink_p1\":%u,\"glink_p2\":%u,\"glink_result\":%d,"
        "\"glink_rx\":%u,\"glink_tx\":%u,\"glink_version\":%u,\"glink_rx_bound\":%u,"
        "\"ipcc_unknown\":%u,\"ipcc_unknown_id\":%u,\"qrtr_up\":%u,\"qrtr_hello\":%u,"
        "\"qrtr_rx\":%u,\"qrtr_tx_queued\":%u,\"qrtr_tx_sent\":%u,\"qrtr_bad\":%u,"
        "\"qrtr_refused\":%u,\"qrtr_remote_services\":%u,\"locator_requests\":%u,\"locator_answers\":%u,"
        "\"locator_txn\":%u,\"qrtr_error\":%d,\"gpr_initiator\":%u,\"owned_fsr\":%u,\"owned_smmu_error\":%d,"
        "\"glink_delay_ms\":%u,\"glink_delay_left_ms\":%u,"
        "\"wdog\":%u,\"wdog_base\":%u,\"wdog_irq\":%u,\"wdog_stops\":%u,\"wdog_main\":%d,\"wdog_dtb\":%d,"
        "\"glink_observe_ms\":%u,\"glink_observe_left_ms\":%u,"
        "\"control_only\":true,\"external_dma\":false,\"owned_smmu_observed\":false,"
        "\"PCM\":false,\"HAL\":false}",
#ifdef NEZHA_ALS_CONTROL_ONLY
        "als-control",
#else
        nativeAudioProbe.mode==4 ? "tas-identity":"dsp-tas-identity",
#endif
        v[0],(int32_t)v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],(int32_t)v[9],(int32_t)v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17],v[18],v[19],v[20],
        (unsigned long long)(((uint64_t)v[22]<<32)|v[21]),v[23],v[24],v[25],v[26],(int32_t)v[27],(int32_t)v[28],v[29],v[30],v[31],
        (unsigned long long)(((uint64_t)v[33]<<32)|v[32]),(unsigned long long)(((uint64_t)v[35]<<32)|v[34]),
        (unsigned long long)(((uint64_t)v[37]<<32)|v[36]),
        v[38],v[39],v[40],v[41],(int32_t)v[42],v[43],v[44],v[45],v[46],(int32_t)v[47],v[48],v[49],v[50],v[51],v[52],v[53],
        v[54],v[55],v[56],v[57],v[58],v[59],v[60],v[61],v[62],v[63],v[64],(int32_t)v[65],v[66],v[67],(int32_t)v[68],v[69],v[70],
        v[71],v[72],v[73],v[74],(int32_t)v[75],(int32_t)v[76],v[77],v[78]);
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
void watchdog_irq(void *v) {na_adsp_watchdog_event(static_cast<NaAdsp *>(v));}
int configure_watchdog(NaNativeAdspWatchdog *w,NaResourceOwner *owner,NaAdsp *adsp,uint64_t lease) {
    // Restore Audio024's control-only lifecycle. Audio025/026 never loaded
    // firmware because pin6 CFG writes read back zero, including a separate
    // masked type write with 200us observation. Do not repeat that behavior
    // or claim the direct pin's CFG register proves interrupt delivery.
    auto *common=owner->common;
    if (!common || !common->initialized || !common->io.lock || !common->io.unlock)
        return NA_OWNERSHIP;
    IOReturn irq=armcpu_spi_attach(783,true,watchdog_irq,adsp,&w->irq,&w->router);
    printf("NEZHA AUDIO27 WDOG IRQ: intid=783 result=%d GIC=%#x route=%#llx\n",
        irq,w->irq,(unsigned long long)w->router);
    if (irq!=kIOReturnSuccess || (w->irq&7)!=7) return NA_IO;
    int r=na_native_map(&w->pdc,{0x0b220000,0x2000},lease);
    if (r) return r;
    r=na_native_map(&w->spi,{0x17960000,0x1000},lease);
    if (r) return r;
    auto *pdc=reinterpret_cast<volatile uint32_t *>(w->pdc.va);
    w->pdcVersion=pdc[0x1000/4];w->pdcParameters=pdc[0x100c/4];
    w->pdcBefore=pdc[(0x110+4*6)/4];
    printf("NEZHA AUDIO27 WDOG PDC READONLY: version=%#x parameters=%#x pin=6 cfg=%#x\n",
        w->pdcVersion,w->pdcParameters,w->pdcBefore);
    // Only the actual nezha v5 controller observed in025/026 is targeted.
    // Parameters are raw diagnostics; no unestablished count-field decode.
    if (w->pdcVersion!=0x50000) return NA_MISSING;
    // Current stock DTS and matching qcom-pdc.ko additionally configure
    // SPI751 in region179600f0/f4: bank23,bit15 selects edge when clear.
    // Audio026 actually read fffffff8 (level). Only this owned bit changes.
    // No qcom,scm-spi-cfg in stock DT; do not guess a secure-I/O call.
    auto *spi=reinterpret_cast<volatile uint32_t *>(w->spi.va)+(0xf0+4*23)/4;
    auto &io=common->io;
    if (io.lock(io.context)) return NA_OWNERSHIP;
    w->spiBefore=*spi;const uint32_t edge=w->spiBefore&~0x8000u;
    if (edge!=w->spiBefore) {*spi=edge;na_native_publish_writes();}
    w->spiAfter=*spi;
    io.unlock(io.context);
    printf("NEZHA AUDIO27 WDOG SPI: address=0x1796014c before=%#x edge=%u\n",
        w->spiBefore,(w->spiBefore&0x8000u)==0);
    printf("NEZHA AUDIO27 WDOG SPI: after=%#x edge=%u other-bits-preserved=%u\n",
        w->spiAfter,(w->spiAfter&0x8000u)==0,w->spiAfter==edge);
    return w->spiAfter==edge ? NA_OK:NA_IO;
}
int control_buffers_ready(void *v) {
    auto *d=static_cast<AudioDspProbe *>(v);
    // This diagnostic sends GLINK/QRTR control packets and reads reset-muted
    // TAS identity. It never publishes a PCM/topology address to the DSP.
    // The same-SoC pandora control-only path adds the audio DMA domain only
    // when its speaker plan is enabled. Leave inherited
    // mappings/IRQs untouched here instead of installing an unused domain.
    if (!d->adsp.masters_stopped || !d->plan.adsp_lease || d->plan.smmu.lease ||
        d->dma.descriptor || d->tables.descriptor || d->plan.tables.cpu ||
        d->plan.tables.physical.bytes) return NA_OWNERSHIP;
    for (const auto &stream:d->plan.stream)
        if (stream.graph || stream.dma.cpu || stream.dma.physical.bytes || stream.iova || stream.port)
            return NA_OWNERSHIP;
    capture_crash_baseline(d);
    return na_native_adsp_watchdog_prepare(&d->watchdog,&d->input.resources,&d->adsp,d->plan.adsp_lease);
}
int poll_shared(void *v) {
    auto *d=static_cast<AudioDspProbe *>(v);
    return na_resource_owner_dispatch(&d->input.resources);
}
int prepare(AudioDspProbe *d) {
    int r=na_native_prepare_adsp_inputs(&d->input,&d->plan);if (r) return r;
    d->input.progress_context=d;d->input.progress=loader_progress;
    uint64_t lease=d->plan.adsp_lease;
    r=na_native_map(&d->smem,{0x81d00000,0x200000},lease);if (r) return r;
    r=na_native_map(&d->qmp,{0xc300000,0x1000},lease);if (r) return r;
    r=na_native_map(&d->cmdDb,{0x81c60000,0x20000},lease);if (r) return r;
    const NaRange region[]={na_nezha_facts.firmware,na_nezha_facts.dtb};
    for (unsigned i=0;i<2;++i) {
        auto &m=d->input.metadata[i];
        r=na_pas_begin(&d->loader[i],&d->input.firmware[i],i ? 36:1,region[i],d->plan.firmware_map[i],
            m.va,m.physical,m.bytes,&d->plan.pas,d->now);if (r) return r;
    }
    NaProxyOps power=na_resource_owner_proxy(&d->input.resources);
    r=na_proxy_init(&d->power,reinterpret_cast<volatile uint32_t *>(d->qmp.va),0x400,
        const_cast<const uint8_t *>(d->cmdDb.va),0x20000,1,&power);if (r) return r;
    NaAdspOps ops={d,smem_lock,smem_unlock,doorbell,proxy,control_buffers_ready,poll_shared,post_ready};
    r=na_adsp_init(&d->adsp,d->smem.va,0x200000,&d->loader[0],&d->loader[1],&d->rx,&ops);if (r) return r;
#ifdef NEZHA_ALS_NATIVE_OWNER
    // One boot lifetime created by this fresh owner; never reuse after loss.
    r=nezha_als_native_install(&d->adsp,d,1,200000);if (r) return r;
#endif
    // Android GLINK's create_local and pandora_adsp_gpr=2 both initiate OPEN
    // when no remote adsp_apps OPEN exists. Audio018 never observed that name;
    // this candidate exercises that supported path, not an assumed DSP fix.
#ifndef NEZHA_ALS_CONTROL_ONLY
    d->adsp.gpr_initiator=1;
#endif
    // Separate authenticated firmware/SMP2P/handover from our first GLINK
    // FIFO writes and replies. Full playback retains the zero-delay default.
    d->adsp.glink_delay_ms=20000;
    // Audio022/023 lost USB immediately after the 10s/20s window without
    // preserving the first setup marker. Only this diagnostic yields 100ms
    // between existing setup/RX/poll observations. Each poll still checks
    // watchdog/SMP2P and completes handover; ordinary playback leaves zero.
    d->adsp.glink_observe_ms=100;
    // Audio016 still reset with all startup votes held. Restore the ordinary
    // handover/release policy; its original diagnostic image stays frozen.
    return na_resource_owner_ipcc(&d->input.resources,&d->adsp);
}
#ifndef NEZHA_ALS_CONTROL_ONLY
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
        r=na_geni_board_init_identity(&d->tasBoard,&board,&d->tasOps);if (r) return r;
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
int poll_tas_only(AudioDspProbe *d) {
    if (d->phase==1) {
        // Reuse the same late common-resource boundary as ADSP preparation,
        // without parsing firmware, acquiring PAS/DMA or touching GLINK.
        if (__atomic_load_n(&nezha_touch_stage,__ATOMIC_ACQUIRE)!=7 ||
            !sm8850_usb_domain_ready()) return NA_PENDING;
        if (!na_xpu_trace_before("common-resources",0,0)) return NA_IO;
        d->input.resources.common=sm8850_native_resources();
        na_xpu_trace_after("common-resources",0,d->input.resources.common ? NA_OK:NA_IO,
                           0,(uintptr_t)d->input.resources.common);
        if (!d->input.resources.common) return NA_IO;
        d->phase=3;d->deadline=d->now+5000;
    }
    if (d->phase==3) {
        int r=identify_tas(d);
        if (r<0) d->tasError=r;
        else if (!r) d->phase=4;
        return r;
    }
    return d->phase==4 ? NA_OK:NA_OWNERSHIP;
}
#endif
bool read_crash_message(AudioDspProbe *d,char *message) {
    memset(message,0,161);
    QcomSmemPartition global;uint32_t at=0,bytes=0;
    int r=qcom_smem_partition(&d->adsp.smem,QCOM_SMEM_HOST_GLOBAL,QCOM_SMEM_HOST_GLOBAL,&global);
    if (!r) r=qcom_smem_find(&d->adsp.smem,&global,423,&at,&bytes);
    if (r || !bytes || at>d->adsp.smem.size || bytes>d->adsp.smem.size-at) return false;
    unsigned n=0,limit=bytes<160 ? bytes:160;
    while (n<limit) {uint8_t c=d->adsp.smem.base[at+n];if (!c) break;message[n++]=c>=32 && c<=126 ? (char)c:'.';}
    return true;
}
void capture_crash_baseline(AudioDspProbe *d) {
    d->crashBaselinePresent=read_crash_message(d,d->crashBaseline);
    memcpy(d->crashLast,d->crashBaseline,sizeof d->crashLast);
    d->crashLastPresent=d->crashBaselinePresent;d->crashNext=d->now+20;
    printf("NEZHA AUDIO27 SMEM423 BASELINE: present=%u before authentication\n",d->crashBaselinePresent);
}
void observe_crash_message(AudioDspProbe *d) {
    if (d->loader[0].stage!=NaPasReady || d->now<d->crashNext) return;
    d->crashNext=d->now+20;char message[161];bool present=read_crash_message(d,message);
    if (present && message[0] && (!d->crashLastPresent || strcmp(message,d->crashLast))) {
        if (d->crashLogs<4)
            printf("NEZHA AUDIO27 SMEM423 UPDATE: after-auth same-baseline=%u (bounded): %s\n",
                d->crashBaselinePresent && !strcmp(message,d->crashBaseline),message);
        ++d->crashLogs;
    }
    memcpy(d->crashLast,message,sizeof d->crashLast);d->crashLastPresent=present;
    // Text changes are evidence only. They never synthesize a watchdog, stop
    // the DSP, clear firmware memory or introduce another GLINK consumer.
}
void crash_reason(AudioDspProbe *d) {
    const bool fatal=(d->adsp.slave&1) &&
        __atomic_load_n(&d->adsp.doorbells,__ATOMIC_ACQUIRE)!=d->adsp.auth_doorbells;
    if (d->adsp.error!=NA_REMOTE || (!fatal && !d->adsp.watchdog_fault)) return;
    char message[161];
    if (!read_crash_message(d,message)) {
        printf("NEZHA AUDIO27 DSP: post-auth %s, bounded SMEM crash reason unavailable\n",
            d->adsp.watchdog_fault ? "watchdog":"fatal");return;
    }
    printf("NEZHA AUDIO27 DSP: SMEM423 after fresh post-auth %s (bounded snapshot): %s\n",
        d->adsp.watchdog_fault ? "watchdog":"fatal",message);
}
void stop_faulted_adsp(AudioDspProbe *d) {
    if (d->watchdogStopAttempted) return;
    d->watchdogStopAttempted=1;publish(d);
    d->watchdogMain=d->loader[0].ops.pas(d->loader[0].ops.context,6,1,0,0);
    d->watchdogStops|=1;publish(d);
    d->watchdogDtb=d->loader[1].ops.pas(d->loader[1].ops.context,6,36,0,0);
    d->watchdogStops|=2;
    if (!d->watchdogMain && !d->watchdogDtb) d->adsp.masters_stopped=1;
    publish(d);
    printf("NEZHA AUDIO27 WDOG STOP: returned=%u main=%d dtb=%d\n",
        d->watchdogStops,d->watchdogMain,d->watchdogDtb);
    // These fixed PAS calls contain this fresh watchdog once. Every lease,
    // buffer and mapping stays retained; no retry or system reset follows.
}
void run_probe(void *v,wait_result_t) {
    auto *d=static_cast<AudioDspProbe *>(v);d->deadline=na_native_now_us(nullptr)/1000+120000;
    unsigned last=UINT32_MAX,lastPhase=UINT32_MAX,lastSlave=UINT32_MAX,lastGpr=UINT32_MAX,lastHeld=UINT32_MAX;int lastError=0;
    uint32_t lastQrtrUp=UINT32_MAX,lastQrtrHello=UINT32_MAX,lastQrtrRx=UINT32_MAX,lastQrtrTx=UINT32_MAX;
    int lastQrtrError=0;
    uint32_t lastServices=UINT32_MAX;
    for (;;) {
        d->now=na_native_now_us(nullptr)/1000;int r=NA_PENDING;
#ifndef NEZHA_ALS_CONTROL_ONLY
        if (d->mode==4) r=poll_tas_only(d);
        else
#endif
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
#ifdef NEZHA_ALS_CONTROL_ONLY
            if (!r && (d->adsp.handed || (d->adsp.retain_proxy && d->adsp.handover_seen)) &&
                d->adsp.mapper.online && d->adsp.mapper.qrtr.hello_received && d->phase==2) d->phase=3;
            if (r>=0 && d->phase==3) d->phase=4;
#else
            if (!r && (d->adsp.handed || (d->adsp.retain_proxy && d->adsp.handover_seen)) &&
                d->adsp.transport.online && d->phase==2) d->phase=3;
            if (r>=0 && d->phase==3) {
                int tas=identify_tas(d);
                if (tas<0) {d->tasError=tas;tas_cleanup(d);d->phase=4;}
                else if (!tas) d->phase=4;
            }
#endif
        }
        if (d->mode!=4 && d->phase>=2 && d->phase<=4) observe_crash_message(d);
        if (r<0 || (d->phase<4 && d->now>=d->deadline)) {
            d->error=r<0 ? r:NA_TIMEOUT;d->phase=5;
            if (d->mode!=4 && d->adsp.watchdog_fault) {crash_reason(d);stop_faulted_adsp(d);}
#ifndef NEZHA_ALS_CONTROL_ONLY
            tas_cleanup(d);
#endif
            if (d->mode!=4 && !d->adsp.watchdog_fault) crash_reason(d);
        }
        publish(d);
        if (last!=d->adsp.stage || lastPhase!=d->phase || lastError!=d->error || lastSlave!=d->adsp.slave || lastGpr!=d->adsp.transport.online || lastHeld!=d->power.held) {
            if (d->mode==4)
                printf("NEZHA AUDIO27 TAS IDENTITY ONLY: phase=%u error=%d TAS=%#x/%#x tas-error=%d cleanup=%d; no ADSP/PAS/GLINK/PCM\n",
                    d->phase,d->error,d->revision[0],d->revision[1],d->tasError,d->tasCleanup);
            else printf("NEZHA AUDIO27 DSP: phase=%u error=%d ADSP=%u slave=%x handover=%u GPR=%u TAS=%u/%u error=%d seen=%u retain=%u held=%x GLINK=%u/%u unknown=\"%s\" IPCC=%u/%x\n",
                d->phase,d->error,d->adsp.stage,d->adsp.slave,d->adsp.handed,d->adsp.transport.online,d->revision[0],d->revision[1],d->tasError,d->adsp.handover_seen,d->adsp.retain_proxy,d->power.held,
                d->adsp.glink.rx_messages,d->adsp.glink.tx_messages,d->adsp.glink.unknown_names,
                d->input.resources.common ? d->input.resources.common->ipcc_unknown_count:0u,
                d->input.resources.common ? d->input.resources.common->ipcc_last_unknown:0u);
            last=d->adsp.stage;lastPhase=d->phase;lastError=d->error;lastSlave=d->adsp.slave;lastGpr=d->adsp.transport.online;lastHeld=d->power.held;
        }
        const auto &router=d->adsp.mapper;
        if (d->mode!=4 && (lastQrtrUp!=router.online || lastQrtrHello!=router.qrtr.hello_received ||
            lastQrtrRx!=router.qrtr.rx_packets || lastQrtrTx!=router.tx_sent || lastQrtrError!=router.error)) {
            // Only the DSP diagnostic owns this transport.
            printf("NEZHA AUDIO27 QRTR: up=%u hello=%u rx=%u tx=%u/%u bad=%u refused=%u services=%u locator=%u/%u txn=%u name=\"%s\" error=%d\n",
                router.online,router.qrtr.hello_received,router.qrtr.rx_packets,router.tx_sent,router.qrtr.tx_packets,
                router.qrtr.rx_bad,router.qrtr.tx_refused,router.qrtr.remote_count,
                router.requests,router.answers,router.last_transaction,router.last_service,router.error);
            lastQrtrUp=router.online;lastQrtrHello=router.qrtr.hello_received;lastQrtrRx=router.qrtr.rx_packets;
            lastQrtrTx=router.tx_sent;lastQrtrError=router.error;
        }
        if (d->mode!=4 && lastServices!=router.qrtr.remote_count) {
            for (unsigned i=0;i<router.qrtr.remote_count;++i) {
                const auto &service=router.qrtr.remote[i];
                printf("NEZHA AUDIO27 QRTR SERVICE: id=%x instance=%x node=%u port=%u\n",
                    service.service,service.instance,service.node,service.port);
            }
            lastServices=router.qrtr.remote_count;
        }
        if (d->phase==5 || (d->mode==4 && d->phase==4)) break;
        assert_wait_timeout((event_t)d,THREAD_UNINT,1,kMillisecondScale);thread_block(THREAD_CONTINUE_NULL);
    }
    // Static state, firmware mappings and PAS/control leases remain retained. The
    // existing USB path continues; no automatic reset or PAS replay follows.
    thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
}
extern "C" int na_native_adsp_watchdog_prepare(NaNativeAdspWatchdog *w,NaResourceOwner *owner,NaAdsp *adsp,uint64_t lease) {
    if (!w || !owner || !adsp || !lease || !adsp->masters_stopped || adsp->stage!=NaAdspDma) return NA_OWNERSHIP;
    if (w->attempted) return w->error;
    w->attempted=1;w->error=configure_watchdog(w,owner,adsp,lease);
    return w->error;
}
static void start_probe(unsigned mode) {
    auto *d=&nativeAudioProbe;
    if (__atomic_exchange_n(&d->started,1,__ATOMIC_ACQ_REL)) return;
    d->mode=mode;
    d->phase=1;publish(d);
    if (!qcom_usb_diag_register(&dspProvider)) {d->phase=5;d->error=NA_BUSY;publish(d);return;}
    thread_t thread=THREAD_NULL;
    if (kernel_thread_start(run_probe,d,&thread)!=KERN_SUCCESS) {d->phase=5;d->error=NA_IO;publish(d);return;}
    thread_deallocate(thread);
}
extern "C" void nezha_audio27_dsp_probe_start(void) {start_probe(2);}
#ifndef NEZHA_ALS_CONTROL_ONLY
extern "C" void nezha_audio27_tas_probe_start(void) {start_probe(4);}
#endif
