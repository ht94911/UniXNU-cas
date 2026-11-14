/* SPDX-License-Identifier: BSD-2-Clause */
/* Connects the QcomBaseband bridge (bridge/, derived for iOS 27) to the nezha baseband worker, in the roles Coachz's Wi-Fi thread
 * plays (platform/coachz/README.md, steps 3 and 5): QRTR sends and DEL_CLIENT go through the worker's router, NEW/DEL_SERVER and
 * the bridge's client-port data come from it, and the data adapter is the IPA board layer. Everything runs on the worker thread. */
#include "NbBaseband.h"
#include "../bridge/QcomBaseband.h"
#include "../bridge/NbBoot.h"

static NbBaseband *nb_glue;   /* the bridge's C entry points carry no context; one baseband per boot */
static uint32_t nb_boot_online;
static uint32_t nb_boot_watchdog;

static void bridge_send(uint32_t local_port, uint32_t node, uint32_t port, const void *body, uint32_t length)
{
    if (nb_glue) nb_qrtr_send_data(nb_baseband_qrtr(nb_glue), local_port, node, port, (const uint8_t *)body, length);
}

/* NAS shared-port transactions must see actual queue refusal. The existing
 * per-client bridge callback and all non-NAS routes retain their original ABI. */
int coachz_modem_qmi_send(uint32_t local_port, uint32_t node, uint32_t port, const void *body, uint32_t length)
{
    return nb_glue ? nb_qrtr_send_data(nb_baseband_qrtr(nb_glue), local_port, node, port,
                                      (const uint8_t *)body, length) : NbQrtrDown;
}

static void bridge_del_client(uint32_t port)
{
    if (nb_glue) nb_qrtr_del_client(nb_baseband_qrtr(nb_glue), port);
}

static int bridge_unrouted(void *context, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context;
    return coachz_baseband_qrtr(port, node, from, data, length);
}

static void bridge_server_new(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    (void)context;
    coachz_baseband_new_server(service, instance, node, port);
}

static void bridge_server_del(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    (void)context; (void)instance;
    coachz_baseband_del_server(service, node, port);
}

static void bridge_online(void *context, int online)
{
    (void)context;
    if (online) coachz_baseband_configure(bridge_send, bridge_del_client);
    else coachz_baseband_down();
}

/* The bridge hooks for NbBasebandConfig.bridge. Call once, before nb_baseband_init. */
NbBasebandBridge nb_bridge_hooks(NbBaseband *b)
{
    nb_glue = b;
    const NbBasebandBridge hooks = { 0, bridge_unrouted, bridge_server_new, bridge_server_del, bridge_online };
    return hooks;
}

/* The bridge's poll and report, from the worker after nb_baseband_poll. */
void nb_bridge_state(int result)
{
    if (nb_glue) __atomic_store_n(&nb_boot_watchdog, nb_glue->mss.auth_wdog_events,
                                  __ATOMIC_RELEASE);
    __atomic_store_n(&nb_boot_online, nb_boot_ready(nb_glue, result, nb_ipa_data_ready()),
                     __ATOMIC_RELEASE);
}
void nb_bridge_invalidate(void) { __atomic_store_n(&nb_boot_online, 0, __ATOMIC_RELEASE); }
int coachz_modem_boot_ready(void)
{
    return __atomic_load_n(&nb_boot_online, __ATOMIC_ACQUIRE) && nb_glue &&
        __atomic_load_n(&nb_glue->mss.wdog_events, __ATOMIC_ACQUIRE) ==
        __atomic_load_n(&nb_boot_watchdog, __ATOMIC_ACQUIRE);
}
void nb_bridge_poll(void) { coachz_baseband_poll(); }
void nb_bridge_report(unsigned second) { coachz_baseband_report(second); nb_ipa_report(second); }

/* The data adapter the bridge's PDP layer calls. */
int coachz_modem_data_send(uint32_t mux, const uint8_t *ip, uint32_t length) { return nb_ipa_data_send(mux, ip, length); }
void coachz_modem_data_receiver(void (*receiver)(uint32_t, const uint8_t *, uint32_t, uint32_t)) { nb_ipa_data_receiver(receiver); }
