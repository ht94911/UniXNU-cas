/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER27_H
#define NEZHA_POWER27_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* All times are monotonic milliseconds, including time asleep. No wall clock. */
#define NP_GAUGE_WORDS 10u
#define NP_GAUGE_TTL_MS 15000u
#define NP_CHARGER_TTL_MS 5000u
#define NP_MAX_SKEW_MS 2000u
enum np_field {
    NP_PRESENT, NP_SOC, NP_VOLTAGE, NP_CURRENT, NP_TEMPERATURE,
    NP_REMAINING, NP_FULL_CAPACITY, NP_DESIGN, NP_CYCLES, NP_FULL_FLAG,
    NP_EXTERNAL, NP_CHARGING, NP_CHARGE_DONE, NP_USB_VOLTAGE, NP_USB_CURRENT,
    NP_FAULT, NP_FIELDS
};
#define NP_BIT(f) (UINT32_C(1) << (f))
#define NP_GAUGE_REQUIRED (NP_BIT(NP_PRESENT) | NP_BIT(NP_SOC) | NP_BIT(NP_VOLTAGE))
enum np_fault {
    NP_FAULT_NONE = 0, NP_FAULT_TRANSPORT = 1, NP_FAULT_SAMPLE = 2,
    NP_FAULT_STALE = 4, NP_FAULT_REMOTE_RESET = 8, NP_FAULT_CHARGER = 16,
    NP_FAULT_PUMP = 32
};
typedef struct {
    uint32_t valid;
    int32_t present, soc, voltage_mv, current_ma, temperature_centi_c;
    int32_t remaining_mah, full_mah, design_mah, cycles, full_flag;
    uint16_t raw[NP_GAUGE_WORDS];
    uint64_t begin_ms, end_ms;
    uint64_t generation;
} NpGauge;
enum np_input_current_scope {
    NP_INPUT_CURRENT_UNKNOWN = 0,
    NP_INPUT_CURRENT_MAIN_BUCK = 1,
    NP_INPUT_CURRENT_WHOLE_PORT = 2
};
typedef struct {
    uint32_t valid, fault;
    int32_t external, charging, done, usb_uv, usb_ua;
    uint64_t begin_ms, end_ms, epoch, generation;
    uint8_t input_pair_coherent; /* firmware latched pair, not two independent GETs */
    /* MCA0x20004 is main-buck bus current. It can read0 with the charge pump
     * supplying the battery; coherence alone does not make it whole-port input. */
    uint8_t input_current_scope;
} NpCharger;
typedef struct {
    NpGauge gauge;
    NpCharger charger;
    uint32_t valid, faults;
    int32_t external, charging, fully_charged;
    uint64_t generation, at_ms;
    int64_t usb_power_uw; /* simultaneous input sample only; not adapter rating */
    uint8_t usb_power_valid;
} NpSnapshot;

/* Read-only identity from the gauge's volatile MAC reply. The stock device
 * name is the bounded substring between '@' and '#', not the full raw block. */
typedef struct {
    uint32_t valid, error;
    uint8_t cell_code;
    char device_name[9];
} NpGaugeIdentity;
int np_mac_info_decode(uint16_t command, const uint8_t reply[36],
                       uint8_t *data, size_t bytes);
int np_gauge_identity_decode(NpGaugeIdentity *, const uint8_t name[16],
                             const uint8_t chemistry[4]);

/* The physical package exposes ONE bq27z561 at 0x55. Do not double mAh, voltage
 * or current based on a different board's two-cell/gauge implementation.
 * words: 08,0c,06,2c,10,12,3c,2a,2e,0a (little-endian standard commands).
 * This word reader sends no Control(), MAC(), unseal, flash or calibration
 * writes. Separate auth-enabled code uses only the verified volatile MAC0
 * challenge/response; it does not change this standard-word read path. */
extern const uint8_t np_gauge_commands[NP_GAUGE_WORDS];
int np_decode_gauge(NpGauge *, const uint8_t bytes[NP_GAUGE_WORDS][2],
                    uint32_t word_valid, uint64_t begin_ms, uint64_t end_ms,
                    uint64_t generation);
void np_snapshot(NpSnapshot *, const NpGauge *, const NpCharger *,
                 uint64_t now_ms, uint64_t generation);
int np_snapshot_read(NpSnapshot *); /* platform-owned mutex, defined in Runtime */
void nezha_power27_start(void);    /* called once from the selected late caller */
void nezha_power27_stop(void);     /* quiesce; no remote reset or power cut */
/* Before the first normal VFS unmount, stop the existing worker and await its
 * real terminal completion. 0 also permits an inactive or failed/exited
 * worker; it does not assert successful log/CRC completion. ETIMEDOUT means
 * the worker is still active, so its dependent unmount must not proceed. */
int nezha_power27_prepare_shutdown(void);
void nezha_power27_suspend(void);  /* asynchronous; poll lifecycle before PM ack */
void nezha_power27_resume(void);
/* 0 inactive, 1 running, 2 suspend pending, 3 suspended, 4 stop pending,
 * 5 stopped with quiesce confirmed, 6 resume pending, -1 failed/quarantined. */
int nezha_power27_lifecycle(void);
/* Terminal worker completion permits normal system shutdown to continue even
 * after a diagnosed error; it does not acknowledge a sleep/resume success. */
int nezha_power27_worker_exited(void);

#ifdef __cplusplus
}
#endif
#endif
