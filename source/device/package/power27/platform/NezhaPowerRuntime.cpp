// SPDX-License-Identifier: BSD-2-Clause-Patent
// A single worker owns the request state and all hardware callbacks. Consumers
// copy one completed generation under a mutex; they never read half-written data.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <pexpert/pexpert.h>
#include "NezhaPowerRuntime.h"

extern "C" IOService *np_battery_service_create(void);
namespace {
NpPowerOps io;
NpMca mca;
IOLock *snapshotLock;
NpGauge stagingGauge, gauge;
NpCharger stagingCharger, charger;
IOService *source;
uint32_t bound, started, stopRequested, workerExited;
uint32_t suspendRequested;
int lifecycle;
uint64_t generation, refreshAt, queuedAt, transportEpoch;
unsigned cursor;
bool cycle, outgoing, refreshRequested;
uint8_t request[NP_MCA_REQUEST];
uint32_t props[4];

void commit(uint64_t now) {
    stagingCharger.end_ms = now;
    if (refreshRequested) { stagingCharger.valid = 0; stagingCharger.fault |= NP_FAULT_STALE; }
    IOLockLock(snapshotLock);
    gauge = stagingGauge; charger = stagingCharger;
    IOLockUnlock(snapshotLock);
    IOLog("NEZHA POWER SAMPLE: gen=%llu gauge=%x soc=%d mv=%d ma=%d centiC=%d remaining=%d full=%d design=%d cycle=%d charger=%x faults=%x link=%u replies_timeout=%u ignored=%u\n",
          (unsigned long long)stagingGauge.generation,stagingGauge.valid,stagingGauge.soc,
          stagingGauge.voltage_mv,stagingGauge.current_ma,stagingGauge.temperature_centi_c,
          stagingGauge.remaining_mah,stagingGauge.full_mah,stagingGauge.design_mah,stagingGauge.cycles,
          stagingCharger.valid,stagingCharger.fault,(unsigned)mca.up,mca.timeouts,mca.ignored);
    cycle = outgoing = false; refreshAt = now + (refreshRequested ? 250 : 2000);
    refreshRequested = false;
}
void next(uint64_t now) {
    while (cursor < 4 && !props[cursor]) ++cursor; // unverified fields stay absent
    if (cursor == 4) { commit(now); return; }
    if (np_mca_begin(&mca, props[cursor], NP_MCA_READ, nullptr, 0, 4, request) != NP_MCA_REQUEST) {
        stagingCharger.fault |= NP_FAULT_TRANSPORT;
        commit(now); return;
    }
    queuedAt = now; outgoing = true;
}
void begin(uint64_t now) {
    if (generation == UINT64_MAX) { __atomic_store_n(&stopRequested, 1u, __ATOMIC_RELEASE); return; }
    const uint64_t gen = ++generation;
    uint8_t bytes[NP_GAUGE_WORDS][2] = {};
    uint32_t valid = 0;
    const int gaugeError = io.read_gauge(io.context, bytes, &valid);
    uint64_t end = io.now_ms(io.context);
    np_decode_gauge(&stagingGauge, bytes, valid, now, end, gen);
    bzero(&stagingCharger, sizeof(stagingCharger));
    stagingCharger.begin_ms = now; stagingCharger.epoch = mca.epoch;
    stagingCharger.generation = gen;
    if (gaugeError) stagingCharger.fault |= NP_FAULT_TRANSPORT;
    cycle = true; cursor = 0;
    if (!mca.up || !io.schema_verified) { commit(end); return; }
    next(end);
}
void step() {
    uint64_t now = io.now_ms(io.context);
    // Time out before dispatching inbound messages; a delayed work-loop tick
    // cannot turn a response after its deadline into a successful command.
    if (cycle && np_mca_timeout(&mca, now)) {
        stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);
    }
    if (io.poll_link(io.context)) np_power27_link(0, transportEpoch);
    now = io.now_ms(io.context); // callbacks may have queued the NEXT request
    if (outgoing) {
        if (now < queuedAt || now - queuedAt >= 250) {
            np_mca_cancel(&mca); outgoing = false;
            stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);
        } else {
            const int r = io.send_mca(io.context, request, sizeof(request));
            if (!r) {
                outgoing = false;
                if (np_mca_sent(&mca, io.now_ms(io.context), io.mca_reply_timeout_ms)) {
                    stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(io.now_ms(io.context));
                }
            }
            else if (r != -1) {
                np_mca_cancel(&mca); outgoing = false;
                stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);
            }
        }
    }
    if (!cycle && now >= refreshAt) begin(io.now_ms(io.context));
}
void invalidate(uint32_t fault) {
    np_mca_link(&mca, 0); cycle = outgoing = false;
    IOLockLock(snapshotLock);
    charger.valid = gauge.valid = 0; charger.fault = fault;
    IOLockUnlock(snapshotLock);
}
void worker(void *, wait_result_t) {
    __atomic_store_n(&lifecycle, 1, __ATOMIC_RELEASE);
    source = np_battery_service_create();
    bool suspended = false;
    int error = source ? 0 : -1;
    while (!error && !__atomic_load_n(&stopRequested, __ATOMIC_ACQUIRE)) {
        const bool requested = __atomic_load_n(&suspendRequested, __ATOMIC_ACQUIRE) != 0;
        if (requested && !suspended) {
            invalidate(NP_FAULT_STALE);
            error = io.quiesce(io.context);
            if (!error) { suspended = true; __atomic_store_n(&lifecycle, 3, __ATOMIC_RELEASE); }
        } else if (!requested && suspended) {
            error = io.resume(io.context);
            if (!error) {
                suspended = false; refreshAt = 0;
                __atomic_store_n(&lifecycle, 1, __ATOMIC_RELEASE);
            }
        }
        if (!error && !suspended) step();
        IOSleep(50);
    }
    invalidate(NP_FAULT_TRANSPORT);
    // Do not repeat a failed quiesce or free quarantined buffers. The PM caller
    // must distinguish lifecycle -1 from a completed transition.
    if (!error && !suspended) error = io.quiesce(io.context);
    __atomic_store_n(&lifecycle, error ? -1 : 5, __ATOMIC_RELEASE);
    IOLog("NEZHA POWER: worker exit, error=%d; mappings retained\n", error);
    __atomic_store_n(&workerExited, 1u, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
}
}
extern "C" int np_power27_bind(const NpPowerOps *ops) {
    if (!ops || !ops->now_ms || !ops->poll_link || !ops->send_mca || !ops->read_gauge || !ops->quiesce || !ops->resume ||
        __atomic_load_n(&started, __ATOMIC_ACQUIRE) || bound) return -1;
    if (ops->schema_verified && (ops->mca_reply_timeout_ms < 250 || ops->mca_reply_timeout_ms > 1000)) return -1;
    if (ops->schema_verified && ops->property_status &&
        (!ops->status_valid_mask || (ops->status_charging_mask & ~ops->status_valid_mask))) return -1;
    snapshotLock = IOLockAlloc();
    if (!snapshotLock) return -1;
    io = *ops; props[0] = io.property_online; props[1] = io.property_status;
    props[2] = io.property_input_uv; props[3] = io.property_input_ua;
    np_mca_init(&mca); bound = 1; return 0;
}
extern "C" void nezha_power27_start(void) {
    int mode = 0;
    (void)PE_parse_boot_argn("nezha-power", &mode, sizeof(mode));
    if (!mode) return;
    if (mode != 1 || !bound) { IOLog("NEZHA POWER: binding unavailable or unsupported mode\n"); return; }
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) return;
    thread_t t;
    if (kernel_thread_start(worker, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
    else {
        __atomic_store_n(&lifecycle, -1, __ATOMIC_RELEASE);
        __atomic_store_n(&workerExited, 1u, __ATOMIC_RELEASE);
    }
}
extern "C" void nezha_power27_stop(void) {
    __atomic_store_n(&stopRequested, 1u, __ATOMIC_RELEASE);
}
extern "C" void nezha_power27_suspend(void) {
    __atomic_store_n(&suspendRequested, 1u, __ATOMIC_RELEASE);
}
extern "C" void nezha_power27_resume(void) {
    __atomic_store_n(&suspendRequested, 0u, __ATOMIC_RELEASE);
}
extern "C" int nezha_power27_lifecycle(void) {
    const int s = __atomic_load_n(&lifecycle, __ATOMIC_ACQUIRE);
    if (s == 1 && __atomic_load_n(&stopRequested, __ATOMIC_ACQUIRE)) return 4;
    if (s == 1 && __atomic_load_n(&suspendRequested, __ATOMIC_ACQUIRE)) return 2;
    return s;
}
extern "C" int np_snapshot_read(NpSnapshot *out) {
    if (!out || !bound || !snapshotLock) return 0;
    IOLockLock(snapshotLock);
    np_snapshot(out, &gauge, &charger, io.now_ms(io.context), gauge.generation);
    IOLockUnlock(snapshotLock); return 1;
}
extern "C" void np_power27_link(int up, uint64_t epoch) {
    if (!epoch || epoch < transportEpoch || (mca.up == !!up && epoch == transportEpoch)) return;
    transportEpoch = epoch;
    IOLog("NEZHA POWER SOCCP: up=%d epoch=%llu\n",up,(unsigned long long)epoch);
    np_mca_link(&mca, up); cycle = outgoing = false;
    IOLockLock(snapshotLock);
    charger.valid = 0; charger.fault = up ? NP_FAULT_STALE : NP_FAULT_REMOTE_RESET;
    IOLockUnlock(snapshotLock); refreshAt = 0;
}
extern "C" void np_power27_receive(uint64_t epoch, const uint8_t *p, uint32_t n) {
    if (!epoch || epoch != transportEpoch || __atomic_load_n(&suspendRequested, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&stopRequested, __ATOMIC_ACQUIRE)) return;
    uint32_t note = 0, value = 0;
    if (cycle && np_mca_timeout(&mca, io.now_ms(io.context))) {
        stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(io.now_ms(io.context));
    }
    const int r = np_mca_receive(&mca, mca.epoch, io.now_ms(io.context), p, n, &note, &value);
    if (r == NP_MCA_NOTE && io.schema_verified && note < 32 && (io.notification_mask & (1u << note))) {
        // Trigger a fresh cohort, without treating notification0's firmware-
        // dependent payload as a universally valid USB boolean.
        // Coalesce events into a new cohort. A notification during this
        // cohort invalidates its charger part; do not starve the request queue
        // by cancelling it repeatedly on a notification storm.
        if (cycle) refreshRequested = true;
        else refreshAt = 0;
        IOLockLock(snapshotLock); charger.valid = 0; IOLockUnlock(snapshotLock);
        return;
    }
    if (!cycle || (r != NP_MCA_REPLY && r != NP_MCA_ERROR)) return;
    if (r == NP_MCA_REPLY) {
        const uint32_t raw = np_le32(mca.reply);
        switch (cursor) {
        case 0:
            if (raw <= 1) { stagingCharger.external = (int32_t)raw; stagingCharger.valid |= NP_BIT(NP_EXTERNAL); }
            break;
        case 1:
            if (raw < 32 && (io.status_valid_mask & (1u << raw))) {
                stagingCharger.charging = !!(io.status_charging_mask & (1u << raw));
                stagingCharger.done = raw == io.status_done;
                stagingCharger.valid |= NP_BIT(NP_CHARGING) | NP_BIT(NP_CHARGE_DONE);
            }
            break;
        case 2:
            if (raw <= 25000000) { stagingCharger.usb_uv = (int32_t)raw; stagingCharger.valid |= NP_BIT(NP_USB_VOLTAGE); }
            break;
        case 3:
            if (raw <= 10000000) { stagingCharger.usb_ua = (int32_t)raw; stagingCharger.valid |= NP_BIT(NP_USB_CURRENT); }
            break;
        }
    } else stagingCharger.fault |= NP_FAULT_CHARGER;
    ++cursor; next(io.now_ms(io.context));
}
