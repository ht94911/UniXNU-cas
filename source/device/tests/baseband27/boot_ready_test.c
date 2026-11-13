/* SPDX-License-Identifier: BSD-2-Clause */
#include "../../package/baseband27/bridge/NbBoot.h"
#include <assert.h>
#include <stdio.h>
static NbBaseband state;
static NbPas loaders[3];
static void online(void)
{
    state = (NbBaseband){0};
    for (unsigned i = 0; i < 3; ++i) loaders[i] = (NbPas){ .stage = NbPasReady };
    state.c.modem = &loaders[0];
    state.c.modem_dtb = &loaders[1];
    state.c.ipa_fws = &loaders[2];
    state.stage = NbBasebandModem;
    state.reported_online = state.link_up = 1;
    state.mss.stage = NbMssOnline;
    state.mss.slave = NB_MSS_BIT_READY;
    state.qrtr.up = state.qrtr.hello_received = 1;
    const uint32_t services[] = {1, 2, 3, 11};
    for (unsigned i = 0; i < 4; ++i) {
        state.qrtr.remote[i].service = services[i];
        state.qrtr.remote[i].instance = 1;
        state.qrtr.remote[i].port = 0x100 + i;
    }
    state.qrtr.remote_count = 4;
}
int main(void)
{
    assert(!nb_boot_ready(NULL, 0, 1));
    for (unsigned stage = NbMssCold; stage <= NbMssInitialStopDtb; ++stage) {
        online();
        state.mss.stage = stage;
        assert(nb_boot_ready(&state, NB_OK, 1) == (stage == NbMssOnline));
    }
    online(); assert(nb_boot_ready(&state, NB_OK, 1));
    assert(!nb_boot_ready(&state, NB_PENDING, 1));
    assert(!nb_boot_ready(&state, NB_REMOTE, 1));
    assert(!nb_boot_ready(&state, NB_OK, 0));
    state.mss.slave |= NB_MSS_BIT_FATAL; assert(!nb_boot_ready(&state, NB_OK, 1));
    online(); state.mss.wdog_events = 1; assert(!nb_boot_ready(&state, NB_OK, 1));
    state.mss.auth_wdog_events = 1; assert(nb_boot_ready(&state, NB_OK, 1));
    for (unsigned i = 0; i < 3; ++i) {
        online(); loaders[i].stage = NbPasFault; assert(!nb_boot_ready(&state, NB_OK, 1));
    }
    for (unsigned i = 0; i < 4; ++i) {
        online(); state.qrtr.remote[i].service = 0; assert(!nb_boot_ready(&state, NB_OK, 1));
    }
    online(); state.qrtr.up = 0; assert(!nb_boot_ready(&state, NB_OK, 1));
    online(); state.link_up = 0; assert(!nb_boot_ready(&state, NB_OK, 1));
    online(); state.error = NB_REMOTE; assert(!nb_boot_ready(&state, NB_OK, 1));
    puts("PASS boot ready: real PAS/MSS/IPCRTR/service/IPA proof and failure revocation");
}
