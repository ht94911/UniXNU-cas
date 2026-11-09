/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_XPU_TRACE_H
#define NEZHA_AUDIO_XPU_TRACE_H
#include "NezhaAudioNative.h"
#include <IOKit/IOLib.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <QcomXnuUsbDebug.h>

/* Explicit audio diagnostic. No firmware/reset command or new USB owner.
 * BEFORE must leave the existing CPU/transport queues before the access.
 * Status sampling is CPU-only; the existing master worker services DWC3.
 * Three drained samples allow the collector to read a completed IN packet.
 * This is a bounded diagnostic wait, not the normal shutdown quiet policy.
 */
static inline bool na_xpu_trace_enabled(void) {
    int enabled=0;
    return PE_parse_boot_argn("na-xpu-trace",&enabled,sizeof(enabled)) && enabled==1;
}
static inline uint64_t na_xpu_trace_mpidr(void) {
    uint64_t value;__asm__ volatile("mrs %0,mpidr_el1":"=r"(value));return value;
}
static inline int na_xpu_trace_before(const char *op,uint64_t pa,uint64_t span,
                                      uint32_t mask=0,uint32_t value=0) {
    if (!na_xpu_trace_enabled()) return 1;
    if (!ml_get_interrupts_enabled()) return 0;
    struct qcom_usb_debug_status status;
    qcom_usb_debug_get_status(&status);
    if (status.phase!=QCOM_USB_DEBUG_ACTIVE) return 0;
    char record[320];
    int bytes=snprintf(record,sizeof(record),
        "\nNEZHA AUDIO27 XPU BEFORE: op=%s pa=%#llx span=%#llx mask=%#x value=%#x mpidr=%#llx caller=%#llx ticks=%#llx\n",
        op,(unsigned long long)pa,(unsigned long long)span,mask,value,
        (unsigned long long)na_xpu_trace_mpidr(),
        (unsigned long long)(uintptr_t)__builtin_return_address(0),
        (unsigned long long)na_native_host_time(nullptr));
    if (bytes<=0 || (size_t)bytes>=sizeof(record) ||
        qcom_usb_debug_write(record,(size_t)bytes)!=(size_t)bytes) return 0;
    unsigned quiet=0;
    for (unsigned attempt=0;attempt<16;++attempt) {
        qcom_usb_debug_get_status(&status);
        if (status.phase!=QCOM_USB_DEBUG_ACTIVE) return 0;
        if (status.error!=QCOM_USB_DEBUG_STATUS_BUSY &&
            !status.buffer_pending && !status.transport_pending) {
            if (++quiet==3) return 1;
        } else quiet=0;
        IOSleep(1);
    }
    /* The access is declined rather than performing an unobserved step. */
    return 0;
}
static inline void na_xpu_trace_after(const char *op,uint64_t pa,int result,
                                     uint32_t value=0,uint64_t auxiliary=0) {
    if (!na_xpu_trace_enabled() || !ml_get_interrupts_enabled()) return;
    char record[256];
    int bytes=snprintf(record,sizeof(record),
        "\nNEZHA AUDIO27 XPU AFTER: op=%s pa=%#llx result=%d value=%#x auxiliary=%#llx ticks=%#llx\n",
        op,(unsigned long long)pa,result,value,(unsigned long long)auxiliary,
        (unsigned long long)na_native_host_time(nullptr));
    if (bytes>0 && (size_t)bytes<sizeof(record)) (void)qcom_usb_debug_write(record,(size_t)bytes);
}
#endif
