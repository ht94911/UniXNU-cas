/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_XPU_TRACE_H
#define NEZHA_AUDIO_XPU_TRACE_H
#include "NezhaAudioNative.h"
#include <IOKit/IOLib.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <QcomXnuUsbDebug.h>

/* Explicit audio diagnostic. No firmware/reset command or new USB owner.
 * BEFORE must complete on the existing bulk-IN FIFO before the access.
 * accepted/replayed count log bytes; queued/completed count all IN bytes.
 * Fix an accepted watermark after enqueue, then a transport watermark once
 * that log prefix is replayed. Later logs/control frames cannot move either
 * target. Status and record admission are CPU-only; the master services DWC3.
 * This bounded diagnostic wait is separate from normal shutdown admission.
 */
static inline bool na_xpu_trace_enabled(void) {
    int enabled=0;
    return PE_parse_boot_argn("na-xpu-trace",&enabled,sizeof(enabled)) && enabled==1;
}
static inline uint64_t na_xpu_trace_mpidr(void) {
    uint64_t value;__asm__ volatile("mrs %0,mpidr_el1":"=r"(value));return value;
}
static inline int na_xpu_trace_blocked(const char *op,uint64_t pa,const char *reason,
                                      const qcom_usb_debug_status &status,
                                      size_t enqueued,unsigned attempts,unsigned busy,
                                      uint64_t accepted_target,uint64_t transport_target) {
    char record[512];
    int bytes=snprintf(record,sizeof(record),
        "\nNEZHA AUDIO27 XPU BLOCKED: op=%s pa=%#llx reason=%s phase=%u error=%u enqueued=%llu attempts=%u busy=%u accepted-target=%llu transport-target=%llu accepted=%llu replayed=%llu queued=%llu completed=%llu bp=%llu tp=%llu\n",
        op,(unsigned long long)pa,reason,status.phase,status.error,
        (unsigned long long)enqueued,attempts,busy,
        (unsigned long long)accepted_target,(unsigned long long)transport_target,
        (unsigned long long)status.buffer_accepted,(unsigned long long)status.buffer_replayed,
        (unsigned long long)status.tx_queued_total,(unsigned long long)status.tx_completed,
        (unsigned long long)status.buffer_pending,(unsigned long long)status.transport_pending);
    if (bytes>0 && (size_t)bytes<sizeof(record))
        (void)qcom_usb_debug_try_record(record,(size_t)bytes);
    return 0;
}
static inline int na_xpu_trace_before(const char *op,uint64_t pa,uint64_t span,
                                      uint32_t mask=0,uint32_t value=0) {
    if (!na_xpu_trace_enabled()) return 1;
    struct qcom_usb_debug_status status={};
    if (!ml_get_interrupts_enabled())
        return na_xpu_trace_blocked(op,pa,"interrupts-disabled",status,0,0,0,0,0);
    qcom_usb_debug_get_status(&status);
    if (status.phase!=QCOM_USB_DEBUG_ACTIVE)
        return na_xpu_trace_blocked(op,pa,"controller-inactive",status,0,0,0,0,0);
    char record[320];
    int bytes=snprintf(record,sizeof(record),
        "\nNEZHA AUDIO27 XPU BEFORE: op=%s pa=%#llx span=%#llx mask=%#x value=%#x mpidr=%#llx caller=%#llx ticks=%#llx\n",
        op,(unsigned long long)pa,(unsigned long long)span,mask,value,
        (unsigned long long)na_xpu_trace_mpidr(),
        (unsigned long long)(uintptr_t)__builtin_return_address(0),
        (unsigned long long)na_native_host_time(nullptr));
    if (bytes<=0 || (size_t)bytes>=sizeof(record))
        return na_xpu_trace_blocked(op,pa,"record-size",status,0,0,0,0,0);
    size_t enqueued=qcom_usb_debug_try_record(record,(size_t)bytes);
    if (enqueued!=(size_t)bytes)
        return na_xpu_trace_blocked(op,pa,"record-admission",status,enqueued,0,0,0,0);
    uint64_t accepted_target=0,transport_target=0;
    bool have_accepted=false,have_transport=false;
    unsigned busy=0;
    for (unsigned attempt=0;attempt<128;++attempt) {
        qcom_usb_debug_get_status(&status);
        if (status.phase!=QCOM_USB_DEBUG_ACTIVE)
            return na_xpu_trace_blocked(op,pa,"controller-inactive",status,enqueued,
                                         attempt+1,busy,accepted_target,transport_target);
        if (status.error==QCOM_USB_DEBUG_STATUS_BUSY) {
            ++busy; /* Its zero counters are not a completed transfer. */
        } else if (status.error || status.buffer_replayed>status.buffer_accepted ||
                   status.tx_queued_total<status.buffer_replayed ||
                   status.tx_completed>status.tx_queued_total) {
            return na_xpu_trace_blocked(op,pa,"status-snapshot",status,enqueued,
                                         attempt+1,busy,accepted_target,transport_target);
        } else {
            if (!have_accepted && status.buffer_accepted>=(uint64_t)bytes) {
                accepted_target=status.buffer_accepted;have_accepted=true;
            }
            if (have_accepted && !have_transport && status.buffer_replayed>=accepted_target) {
                transport_target=status.tx_queued_total;have_transport=true;
            }
            if (have_transport && status.tx_completed>=transport_target) return 1;
        }
        IOSleep(1);
    }
    return na_xpu_trace_blocked(op,pa,!have_accepted ? "status-busy":
                                  !have_transport ? "cpu-queue":"transport",
                                  status,enqueued,128,busy,accepted_target,transport_target);
}
static inline void na_xpu_trace_after(const char *op,uint64_t pa,int result,
                                     uint32_t value=0,uint64_t auxiliary=0) {
    if (!na_xpu_trace_enabled() || !ml_get_interrupts_enabled()) return;
    char record[256];
    int bytes=snprintf(record,sizeof(record),
        "\nNEZHA AUDIO27 XPU AFTER: op=%s pa=%#llx result=%d value=%#x auxiliary=%#llx ticks=%#llx\n",
        op,(unsigned long long)pa,result,value,(unsigned long long)auxiliary,
        (unsigned long long)na_native_host_time(nullptr));
    if (bytes>0 && (size_t)bytes<sizeof(record)) (void)qcom_usb_debug_try_record(record,(size_t)bytes);
}
#endif
