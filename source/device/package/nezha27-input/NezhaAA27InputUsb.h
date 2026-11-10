/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "NezhaAA27Input.h"
extern "C" {
#include <QcomUsbDiagnostic.h>
}
/* Static state only. USB callbacks copy/queue under a single non-blocking
 * lock, and never call HID, allocate, wait, log or perform device I/O. */
static aa27_input_state aa27Input={};
static unsigned aa27InputLock,aa27InputClaimed;
static uint64_t aa27InputFrequency;
static bool aa27InputTryLock() {
    unsigned expected=0;
    return __atomic_compare_exchange_n(&aa27InputLock,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
}
static void aa27InputUnlock() {__atomic_store_n(&aa27InputLock,0,__ATOMIC_RELEASE);}
static int aa27InputSnapshot(void *,char *out,size_t cap,size_t *bytes,unsigned *trunc) {
    *bytes=0;*trunc=0;if(!aa27InputTryLock())return QUD_BUSY;
    aa27_input_state s=aa27Input;aa27InputUnlock();
    int n=snprintf(out,cap,"{\"v\":1,\"ready\":%u,\"phase\":%u,\"action\":%llu,\"kind\":%u,\"step\":%u,\"last\":%u,\"commits\":%u,\"dispatches\":%u,\"result\":%u,\"width\":%u,\"height\":%u,\"ui_accepted\":false}",
        s.ready,s.phase,(unsigned long long)s.action,s.kind,s.step,s.last,s.commits,s.dispatches,s.result,NEZHA_PANEL_W,NEZHA_PANEL_H);
    if(n<0 || (size_t)n>=cap){*trunc=1;return QUD_FAILED;}*bytes=(size_t)n;return QUD_OK;
}
static int aa27InputSubmit(void *,const qud_diag_request *r,char *out,size_t cap,size_t *bytes,unsigned *trunc) {
    *bytes=0;*trunc=0;if(r->op!=QUD_SUBMIT)return QUD_UNSUPPORTED;
    if(cap<128)return QUD_FAILED;
    if(!aa27InputTryLock())return QUD_BUSY;
    unsigned result=aa27_input_submit(&aa27Input,r->arg0,r->arg1,r->cursor,r->experiment,
        qcom_usb_debug_ticks()+aa27InputFrequency*3);
    aa27InputUnlock();
    if(result)return (int)result;
    int n=snprintf(out,cap,"{\"v\":1,\"queued\":true,\"action\":%llu,\"input_dispatched\":false}",(unsigned long long)r->experiment);
    if(n<0 || (size_t)n>=cap){*trunc=1;return QUD_FAILED;}*bytes=(size_t)n;return QUD_OK;
}
static const qud_diag_provider aa27InputProvider={27,1,(1u<<QUD_SNAPSHOT)|(1u<<QUD_SUBMIT),
    2,UINT32_MAX,"aa27input",nullptr,aa27InputSnapshot,aa27InputSubmit};
static bool aa27InputRegister() {
    unsigned enabled=0,expected=0;
    if(!PE_parse_boot_argn("aa27-input",&enabled,sizeof(enabled)) || enabled!=1)return false;
    if(!__atomic_compare_exchange_n(&aa27InputClaimed,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return false;
    __asm__ volatile("mrs %0, cntfrq_el0":"=r"(aa27InputFrequency));
    aa27Input.sequence=0x80000000u;
    bool registered=aa27InputFrequency && qcom_usb_diag_register(&aa27InputProvider);
    printf("AA27 INPUT: provider27 registered=%u; original HID owner paced\n",registered?1u:0u);
    return registered;
}
/* These functions are called only by the selected touch owner's timer. An
 * acknowledgement may wait for the next tick if USB is copying its snapshot;
 * that retries bookkeeping, never a HID report. */
static bool aa27InputOwnerState(bool ready,bool idle,aa27_input_state *copy) {
    if(!aa27InputTryLock())return false;
    aa27Input.ready=ready;
    aa27_input_begin(&aa27Input,qcom_usb_debug_ticks(),idle);
    *copy=aa27Input;aa27InputUnlock();return true;
}
static bool aa27InputOwnerAck(uint32_t sequence,uint32_t result,bool dispatched) {
    if(!aa27InputTryLock())return false;
    bool accepted=aa27_input_ack(&aa27Input,sequence,result,dispatched);
    if(accepted && result)aa27Input.ready=0;
    aa27InputUnlock();return accepted;
}
