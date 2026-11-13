/* SPDX-License-Identifier: BSD-2-Clause */
/* The native boot controller asks this board-local property through the
 * existing AppleBaseband user client. One means the real SoC modem and IPA
 * have completed their existing startup; it is not an activation or SIM
 * registration result. The worker publishes this value after every poll. */
#ifndef NB_BOOT_H
#define NB_BOOT_H
#include "../platform/NbBaseband.h"
#define NB_BOOT_READY_PROPERTY 0x4e42u
static inline int nb_boot_ready(const NbBaseband *b, int poll_result, int ipa_ready)
{
    if (!b || poll_result != NB_OK || !ipa_ready ||
        b->stage != NbBasebandModem || !b->reported_online || !b->link_up ||
        b->error || b->mss.stage != NbMssOnline || b->mss.error ||
        !(b->mss.slave & NB_MSS_BIT_READY) || (b->mss.slave & NB_MSS_BIT_FATAL) ||
        __atomic_load_n(&b->mss.wdog_events, __ATOMIC_ACQUIRE) != b->mss.auth_wdog_events ||
        !b->c.modem || !b->c.modem_dtb || !b->c.ipa_fws ||
        b->c.modem->stage != NbPasReady || b->c.modem_dtb->stage != NbPasReady ||
        b->c.ipa_fws->stage != NbPasReady || !b->qrtr.up ||
        !b->qrtr.hello_received) return 0;
    const uint32_t services[] = { 1, 2, 3, 11 }; /* WDS, DMS, NAS, UIM */
    for (unsigned i = 0; i < sizeof services / sizeof services[0]; ++i)
        if (nb_qrtr_find_remote(&b->qrtr, services[i], ~0u) < 0) return 0;
    return 1;
}
#endif
