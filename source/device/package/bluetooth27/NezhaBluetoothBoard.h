/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NEZHA_BLUETOOTH_BOARD_H
#define NEZHA_BLUETOOTH_BOARD_H
#include "qbt.h"
#include "qbt_geni.h"
#ifdef __cplusplus
extern "C" {
#endif
/* N0 live DT + Qualcomm canoe-{peach,wcn786x}-bt.dtsi. UART18 is
 * /aliases/hsuart0, QUP3 SE5. SPI18 is disabled; it is not the HCI path. */
#define NEZHA_BT_UART_BASE UINT64_C(0x01994000)
#define NEZHA_BT_QUP_BASE UINT64_C(0x019c0000)
#define NEZHA_BT_UART_INTID 619u
#define NEZHA_BT_QUP_SID 0x4c3u
#define NEZHA_BT_RX_GPIO 27u
#define NEZHA_BT_POWER_OWNER 2u
struct nezha_bt_dma {
    void *cookie;
    uint64_t uart_base, qup_base;
    uint32_t intid, sid;
    struct qbt_dma_buffer tx, rx[QBT_GENI_RX_COUNT];
};
/* Binding to the selected package's existing resource owner. All operations
 * return negative errno. The owner checks the live allocation and common
 * SMMU/GCC/RPMh lock; it must not introduce a second raw register owner.
 * resource_acquire maps registers and allocates buffers, without changing a
 * live DMA route. resource_dma_map runs only after serial_retire proves that
 * both inherited sequencers and DMA engines are idle.
 * Failed acquire may return a cookie/lease if rollback is indeterminate.
 * Failed release keeps that cookie/lease valid and all DMA memory allocated.
 * irq_detach drains callbacks before resource_release can free any buffer.
 * reset exclusively owns PMH0104 GPIO5 (shared BT/UWB), never WLAN GPIO16.
 * firmware_get returns bytes verified against this package's private manifest;
 * no generic or other-board NVM fallback is allowed. */
struct nezha_bt_board_ops {
    void *context;
    struct qbt_geni_ops geni;
    void *(*allocate)(void *,size_t);
    void (*release)(void *,void *,size_t);
    uint64_t (*now_ns)(void *);
    void (*wait_until)(void *,uint64_t);
    void (*wake)(void *);
    int (*resource_acquire)(void *,struct nezha_bt_dma *);
    int (*resource_dma_map)(void *,struct nezha_bt_dma *);
    int (*resource_release)(void *,struct nezha_bt_dma *);
    int (*power_acquire)(void *,uint32_t owner,uint64_t *lease);
    int (*power_release)(void *,uint32_t owner,uint64_t lease);
    int (*reset_enable)(void *,uint64_t lease,bool enable);
    int (*pins_default)(void *); /* captures original state, DT GPIO/RTS-low reset state */
    int (*pins_active)(void *,bool);
    int (*serial_clock)(void *,bool);
    int (*serial_retire)(void *); /* masks source, stops both engines, verifies DMA idle */
    int (*serial_source)(void *,uint32_t baud,uint32_t *source_hz);
    int (*irq_attach)(void *,uint32_t,void (*)(void *),void *);
    int (*irq_detach)(void *,uint32_t,void *);
    int (*firmware_get)(void *,const char *,const uint8_t **,size_t *);
    void (*firmware_put)(void *,const uint8_t *,size_t);
    size_t (*receive)(void *,const uint8_t *,size_t);
    void (*log)(void *,int,const char *);
    void (*bootstrap_rx)(void *,const uint8_t *,size_t); /* bounded startup-only observation */
    void (*bootstrap_tick)(void *);
};
enum nezha_bt_phase { NEZHA_BT_OFF, NEZHA_BT_RESOURCES, NEZHA_BT_POWER,
    NEZHA_BT_UART, NEZHA_BT_FIRMWARE, NEZHA_BT_READY, NEZHA_BT_FAILED,
    NEZHA_BT_QUARANTINED, NEZHA_BT_STOPPING };
/* One executor owns protocol and lifecycle operations. The object must live
 * until irq_detach succeeds. Hard IRQ only masks its line and wakes this
 * executor; socket/USB producers use their existing queues. */
struct nezha_bt_board {
    struct nezha_bt_board_ops ops;
    struct nezha_bt_dma dma;
    struct qbt_geni geni;
    struct qbt *core;
    struct qbt_boot_options boot;
    char patch_name[64],nvm_name[64];
    uint8_t address[6];
    uint64_t power_lease,boot_deadline;
    enum nezha_bt_phase phase;
    int error,cleanup_error;
    uint32_t cancelled,closing;
    bool clocked,irq_attached,pins_set,reset_set,powered;
};
/* The caller supplies a zero-initialized object. Reinitialization with a live
 * core, IRQ callback, DMA allocation or power lease is rejected. */
int nezha_bt_board_init(struct nezha_bt_board *,const struct nezha_bt_board_ops *,
    const struct qbt_boot_options *,const uint8_t address[6]);
int nezha_bt_board_power_on(struct nezha_bt_board *);
int nezha_bt_board_power_off(struct nezha_bt_board *);
int nezha_bt_board_destroy(struct nezha_bt_board *);
void nezha_bt_board_cancel(struct nezha_bt_board *); /* producer-safe; wakes worker */
void nezha_bt_board_stop(struct nezha_bt_board *); /* permanent shutdown, producer-safe */
void nezha_bt_board_step(struct nezha_bt_board *);
uint64_t nezha_bt_board_deadline(const struct nezha_bt_board *);
#ifdef __cplusplus
}
#endif
#endif
