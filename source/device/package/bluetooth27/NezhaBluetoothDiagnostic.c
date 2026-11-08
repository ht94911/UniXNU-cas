/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaBluetoothDiagnostic.h"
#include <QcomUsbDiagnostic.h>
#ifdef KERNEL
#include <sys/systm.h>
#else
#include <stdio.h>
#include <string.h>
#endif
struct cached_state { uint64_t state[32], ns; struct qbt_stats stats; uint32_t pending, stopping, stopped; };
static struct cached_state latest;
static uint32_t busy, ready, registered, skipped;
static int take(void) { uint32_t zero=0;return __atomic_compare_exchange_n(&busy,&zero,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); }
static void release(void) { __atomic_store_n(&busy,0,__ATOMIC_RELEASE); }
void nezha_bt_diagnostic_update(const uint64_t state[32],const struct qbt_stats *stats,
        uint64_t ns,uint32_t pending,uint32_t stopping,uint32_t stopped) {
    if(!state || !stats)return;
    if(!take()){__atomic_fetch_add(&skipped,1,__ATOMIC_RELAXED);return;}
    memcpy(latest.state,state,sizeof(latest.state));latest.stats=*stats;latest.ns=ns;
    latest.pending=pending;latest.stopping=stopping;latest.stopped=stopped;
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);release();
}
static int snapshot(void *cookie,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    (void)cookie;
    if(!out || !capacity || !bytes || !truncated)return QUD_INVALID;
    *bytes=0;*truncated=0;
    if(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE) || !take())return QUD_BUSY;
    struct cached_state s=latest;release();
    int n=snprintf(out,capacity,"{\"version\":1,\"cached\":true,\"ns\":%llu,\"pending\":%u,"
        "\"stopping\":%u,\"stopped\":%u,\"skipped\":%u,\"state\":[",
        (unsigned long long)s.ns,s.pending,s.stopping,s.stopped,__atomic_load_n(&skipped,__ATOMIC_RELAXED));
    if(n<0 || (size_t)n>=capacity){*truncated=1;return QUD_FAILED;}
    size_t used=(size_t)n;
    for(unsigned i=0;i<32;i++){
        n=snprintf(out+used,capacity-used,"%s%llu",i?",":"",(unsigned long long)s.state[i]);
        if(n<0 || (size_t)n>=capacity-used){*truncated=1;return QUD_FAILED;}used+=(size_t)n;
    }
    n=snprintf(out+used,capacity-used,"],\"credits\":%llu,\"credit_events\":%llu,"
        "\"completed_packets\":%llu,\"hardware_errors\":%llu,\"backpressure\":%llu}",
        (unsigned long long)s.stats.command_credits,(unsigned long long)s.stats.command_credit_events,
        (unsigned long long)s.stats.completed_packets,(unsigned long long)s.stats.hardware_errors,
        (unsigned long long)s.stats.backpressure);
    if(n<0 || (size_t)n>=capacity-used){*truncated=1;return QUD_FAILED;}
    *bytes=used+(size_t)n;return QUD_OK;
}
static const struct qud_diag_provider provider={.id=10,.version=1,.operations=1u<<QUD_SNAPSHOT,
    .max_arg0=0,.max_arg1=0,.name="nezha-bluetooth",.cookie=NULL,.snapshot=snapshot,.submit=NULL};
int nezha_bt_diagnostic_init(void) {
    if(registered)return 1;
    registered=qcom_usb_diag_register(&provider);return registered;
}
