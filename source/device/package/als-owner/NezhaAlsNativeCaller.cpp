// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board caller on the existing fresh ADSP control worker. No new transport.
#ifdef NEZHA_ALS_CALLER_HOST
#include "caller-host-shim.h"
#else
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/sched_prim.h>
#include "os/NezhaAudioNative.h"
#endif
#include "NezhaAlsNativeCaller.h"
#include "NezhaAlsQrtrBackend.h"

namespace {
struct Caller {
    NaAdsp *owner;
    void *event;
    IOService *provider;
    IOLock *queueLock;
    uint64_t epoch;
    uint32_t interval;
    nezha_als_qrtr_backend backend;
    nezha_als_ssc_client client;
    bool attempted, prepared, selected, failed, linkSeen;
    uint32_t lastResponses,lastReports,lastPhase,lastUid;
    int error;
};
// All callback storage is retained for this owner's boot lifetime, on failure too.
Caller caller;
void queueLock(void *v) { IOLockLock(static_cast<Caller *>(v)->queueLock); }
void queueUnlock(void *v) { IOLockUnlock(static_cast<Caller *>(v)->queueLock); }
void wake(void *v) { thread_wakeup((event_t)static_cast<Caller *>(v)->event); }
bool online(void *v) {
    auto *c=static_cast<Caller *>(v);
    return c->owner->stage==NaAdspOnline && c->owner->mapper.online &&
           !c->owner->error && !c->owner->mapper.error && !c->owner->glink.broken;
}
uint64_t now(void *v) { return na_native_now_us(static_cast<Caller *>(v)->owner->ops.context); }
uint64_t receipt(void *v) { return na_native_host_time(static_cast<Caller *>(v)->owner->ops.context); }
int room(void *v,uint32_t bytes) {
    auto *c=static_cast<Caller *>(v);auto *m=&c->owner->mapper;
    if (!online(v) || !bytes || bytes>NA_PD_PACKET_BYTES) return NezhaAlsClientTransport;
    // Only the existing owner worker mutates this TX queue. Check before its
    // queue_packet callback, whose full case is a terminal mapper error.
    return m->head-m->tail<NA_PD_TX_SLOTS ? NezhaAlsClientOkay:NezhaAlsClientAgain;
}
void lost(Caller *c,int error) {
    if (c->failed) return;
    c->failed=true;c->error=error<0 ? error:NezhaAlsClientTransport;
    if (c->prepared) nezha_als_qrtr_lost(&c->backend,c->error);
    printf("NEZHA ALS OWNER: lost rc=%d epoch=%llu\n",c->error,(unsigned long long)c->epoch);
}
uint32_t freePort(const QcomQrtr *q) {
    if (q->local_count>=QCOM_QRTR_MAX_LOCAL) return 0;
    // Stock Android QRTR assigns unnamed clients in 0x4000..0x7fff. Use
    // that ordinary client range and retain the locator's explicit port1.
    const uint32_t firstPort=0x4000;
    for (uint32_t port=firstPort;port<firstPort+QCOM_QRTR_MAX_LOCAL;port++) {
        bool used=false;
        for (uint32_t i=0;i<q->local_count;i++) if (q->local[i].port==port) used=true;
        if (!used) return port;
    }
    return 0;
}
}

extern "C" int nezha_als_native_install(NaAdsp *a,void *event,uint64_t epoch,uint32_t interval) {
    if (!a || !event || !epoch || !interval || caller.owner ||
        a->stage!=NaAdspPower || a->glink_initialized) return NezhaAlsClientArgument;
    caller.owner=a;caller.event=event;caller.epoch=epoch;caller.interval=interval;
    return NezhaAlsClientOkay;
}
extern "C" int nezha_als_native_prepared(NaAdsp *a) {
    if (a!=caller.owner || !caller.owner || caller.attempted ||
        a->stage!=NaAdspOnline || !a->dma_ready || !a->glink_initialized ||
        !a->mapper.channel || !a->mapper.qrtr.send) return NezhaAlsClientArgument;
    caller.attempted=true;
    // The fresh-ready worker reaches this after the platform exists. This is
    // the same legitimate provider used by the existing audio/battery services.
    caller.provider=IOService::getPlatform();
    if (!caller.provider) { lost(&caller,NezhaAlsClientFrontend);return caller.error; }
    caller.provider->retain();
    caller.queueLock=IOLockAlloc();
    if (!caller.queueLock) { lost(&caller,NezhaAlsClientFrontend);return caller.error; }
    uint32_t port=freePort(&a->mapper.qrtr);
    if (!port) { lost(&caller,NezhaAlsClientArgument);return caller.error; }
    const nezha_als_qrtr_ops ops={&caller,queueLock,queueUnlock,wake,online,now,receipt,room,
                                 kIOReturnNotReady,kIOReturnNoResources};
    int r=nezha_als_qrtr_prepare(&caller.backend,caller.provider,&a->mapper.qrtr,port,
                                &ops,caller.epoch,caller.interval);
    if (r) { lost(&caller,r);return r; }
    caller.prepared=true;
    printf("NEZHA ALS OWNER: prepared fresh-adsp epoch=%llu local=%u interval-us=%u\n",
           (unsigned long long)caller.epoch,port,caller.interval);
    return NezhaAlsClientOkay;
}
extern "C" void nezha_als_native_after_receive(NaAdsp *a) {
    if (a!=caller.owner || !caller.prepared || caller.failed) return;
    if (a->stage!=NaAdspOnline || a->error || a->mapper.error || a->glink.broken) {
        lost(&caller,NezhaAlsClientTransport);return;
    }
    // A newly registered channel is initially down while OPEN/ACK progresses.
    // Once it has actually been up, losing it invalidates the sample session.
    if (!a->mapper.online) {
        if (caller.linkSeen) lost(&caller,NezhaAlsClientTransport);
        return;
    }
    caller.linkSeen=true;
    const QcomQrtr *q=&a->mapper.qrtr;
    if (!q->hello_received) {
        if (caller.selected) lost(&caller,NezhaAlsClientTransport);
        return;
    }
    if (!caller.selected) {
        unsigned count=0;nezha_als_endpoint endpoint={};
        for (uint32_t i=0;i<q->remote_count;i++) {
            const auto &r=q->remote[i];
            if (r.service!=TCS3760_SNS_SERVICE_ID || (r.instance&255)!=TCS3760_SNS_IDL_MAJOR) continue;
            count++;endpoint={r.node,r.port,r.instance};
        }
        if (!count) return;
        if (count!=1) { lost(&caller,NezhaAlsClientIdentity);return; }
        int r=nezha_als_qrtr_select(&caller.backend,&caller.client,endpoint,caller.epoch);
        if (r) { lost(&caller,r);return; }
        caller.selected=true;
        printf("NEZHA ALS OWNER: same-edge service=400 instance=%u node=%u port=%u epoch=%llu\n",
               endpoint.instance,endpoint.node,endpoint.port,(unsigned long long)caller.epoch);
    }
    const unsigned phase=caller.client.phase,tx=caller.client.tx_state;
    int r=nezha_als_qrtr_poll(&caller.backend);
    if (caller.client.responses!=caller.lastResponses || caller.client.reports!=caller.lastReports ||
        caller.client.phase!=caller.lastPhase || caller.client.match.sensor_uid_valid!=caller.lastUid) {
        caller.lastResponses=caller.client.responses;caller.lastReports=caller.client.reports;
        caller.lastPhase=caller.client.phase;caller.lastUid=caller.client.match.sensor_uid_valid;
        printf("NEZHA ALS OWNER: state phase=%u tx=%u replies=%u reports=%u suid=%u samples=%u stale=%u invalid=%u rc=%d\n",
               caller.client.phase,caller.client.tx_state,caller.client.responses,caller.client.reports,
               caller.lastUid,caller.client.delivered,caller.client.stale,caller.client.invalid_samples,r);
    }
    if (!r && tx==NezhaAlsTxQueued && caller.client.tx_state==NezhaAlsTxAwaiting) {
        const auto &client=caller.client;
        const unsigned qmiId=client.tx[3] | (unsigned)client.tx[4]<<8;
        printf("NEZHA ALS OWNER: queued request phase=%u txn=%u local=%u dst=%u/%u qmi=%u bytes=%u router-tx=%u fifo-sent=%u\n",
               client.phase,(unsigned)client.transaction,caller.backend.local_port,
               client.endpoint.node,client.endpoint.port,qmiId,(unsigned)client.tx_bytes,
               a->mapper.qrtr.tx_packets,a->mapper.tx_sent);
    }
    if (r<0) {
        printf("NEZHA ALS OWNER: query failed rc=%d phase=%u tx=%u txn=%u replies=%u reports=%u stale=%u rx-data=%u unrouted=%u pending=%u fifo-sent=%u\n",
               r,phase,tx,(unsigned)caller.client.transaction,caller.client.responses,
               caller.client.reports,caller.client.stale,q->rx_data,q->rx_unrouted,
               a->mapper.head-a->mapper.tail,a->mapper.tx_sent);
        lost(&caller,r);
    }
}
extern "C" void nezha_als_native_lost(NaAdsp *a,int error) {
    if (a==caller.owner && caller.owner) lost(&caller,error);
}
