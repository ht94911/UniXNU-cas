/* SPDX-License-Identifier: APSL-2.0 */
#ifndef NEZHA_GAUGE_BUS_H
#define NEZHA_GAUGE_BUS_H
#include "NezhaPowerGpi.h"
#include "NezhaPower.h"
#include "NezhaCharge.h"
#include "NezhaBoardThermal.h"
#ifdef KERNEL
typedef struct {
    NpGpi gpi;
    PgpiI2cClock clock;
    void *context;
    /* Shared QUP1/GPI ownership, clock votes and the verified firmware-managed
     * DMA mapping must be established together by the selected board owner.
     * acquire returns zero only after this client's resources are usable. */
    int (*acquire)(void *);
    void (*release)(void *, int dma_stopped);
    uint32_t gpii;
    uint8_t pump_registers_owned; /* exclusive SC8585 event/register lease; default0 */
    uint8_t pump_adc_owned,saved_adc15,saved_adc16;
    uint8_t pump_initialized,pump_init_failed,pump_initial_enable;
    uint8_t pump_usb_ovp_owned,pump_usb_ovp_failed,saved_usb_ovp;
    uint8_t pump_watchdog_owned,pump_watchdog_before,pump_watchdog_expected;
    uint8_t pump_watchdog_restore_attempted;
    uint8_t pump_watchdog_feed_failed;
    uint8_t pump_mode_attempted,pump_mode_failed;
    uint16_t pump_mode_owned,pump_mode_restore_attempted;
    uint8_t pump_mode_before[11],pump_mode_expected[11];
    uint8_t pump_enable_attempted,pump_enable_owned;
    uint8_t pump_disable_attempted,pump_disable_failed;
    uint8_t pump_watchdog_expired,pump_adc_resume_attempted;
    void (*pause_us)(void *,uint32_t);
    uint8_t auth_qualified;
    void (*auth_wait_ms)(void *,uint32_t);
    uint64_t (*now_ms)(void *);
} NpGaugeBus;
int np_gauge_bus_read(void *, uint8_t out[NP_GAUGE_WORDS][2], uint32_t *valid);
int np_gauge_bus_original_temperature(NpGaugeBus *,NpBoardTemperatureReading *);
int np_gauge_bus_auth_digest(NpGaugeBus *,const uint8_t [32],uint8_t [32]);
int np_gauge_bus_identity(NpGaugeBus *,NpGaugeIdentity *);
int np_pump_bus_read(NpGaugeBus *, NpPump *);
int np_pump_bus_restore(NpGaugeBus *);
int np_pump_bus_disable(NpGaugeBus *);
int np_pump_bus_usb_ovp(NpGaugeBus *,uint8_t expected,uint8_t value,
                       uint8_t *observed,uint8_t *attempted);
/* Configure a stopped, known SC8585's1s/5s watchdog only. The sole event
 * owner preserves the complete original0d byte and reads back every write.
 * This proves configuration only; feeding/autonomous shutdown is separate. */
int np_pump_bus_watchdog(NpGaugeBus *,unsigned timeout_ms,
                         uint8_t *observed,uint8_t *attempted);
int np_pump_bus_watchdog_restore(NpGaugeBus *);
/* Refresh only this owner's exact watchdog byte and still-valid ADC scan.
 * Running pumps additionally require owned mode1 and a confirmed enable.
 * An uncertain feed is never sent again. */
int np_pump_bus_watchdog_feed(NpGaugeBus *,uint8_t *observed,uint8_t *attempted);
/* After confirmed stop, restore the owned watchdog. Resume its ADC only if
 * the real watchdog timeout was observed, to measure low-voltage exit. */
int np_pump_bus_prepare_exit(NpGaugeBus *);
/* Actual OS4 forward mode1 primary protections, while stopped and on measured
 * low input. Unavailable6c/6d advisory alarms are preserved, not configured.
 * Original configuration is restored before ADC. */
int np_pump_bus_mode1(NpGaugeBus *,NpPumpModeReading *);
int np_pump_bus_mode_restore(NpGaugeBus *);
/* One enable after owned stockmode1/watchdog and live ADC voltage checks.
 * The caller supplies the already-confirmed adapter voltage. */
int np_pump_bus_enable(NpGaugeBus *,uint32_t expected_vbus_mv,NpPump *);
#endif
#endif
