/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef QBT_TRANSPORT_H
#define QBT_TRANSPORT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct qbt_transport_ops {
    void *context;
    void (*wake)(void *);
    /* Queue power transition on the UART worker; never call back inline. */
    int (*power)(void *, bool);
};
extern volatile uint32_t qbt_transport_activity[4];
struct qbt_transport_stats {
    uint32_t version, rx_buffered, tx_buffered, flags;
    uint64_t rx_dropped, tx_kicks, rx_kicks;
    int32_t boot, power_result;
};
void qbt_transport_get_stats(struct qbt_transport_stats *);
int qbt_transport_init(const struct qbt_transport_ops *);
void qbt_transport_boot_result(int error, const char *firmware, uint32_t baud);
void qbt_transport_power_result(bool enabled, int error, const char *firmware, uint32_t baud);
uint32_t qbt_transport_receive(const uint8_t *, uint32_t);
uint32_t qbt_transport_tx_pending(void);
uint32_t qbt_transport_tx_take(uint8_t *, uint32_t);
bool qbt_transport_active(void);
#ifdef __cplusplus
}
#endif
#endif
