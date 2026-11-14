/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Reuses sheng's native UART.SK worker/IOService path. Pairing, security,
 * profiles and persistent bonds remain with the selected iOS system stack. */
#include "NezhaBluetoothPlatform.h"
#include "NezhaWcnPower.h"
#include "NezhaBluetoothDiagnostic.h"
#include "qbt_transport.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
extern "C" { volatile uint64_t native_bluetooth_stats[32]; }
extern "C" int nezha_wifi_platform_ready(void);
extern "C" int net_init_add(void (*)(void));
extern "C" void nezha_bluetooth_probe(void);
namespace {
struct Runtime {
    nezha_bt_inputs inputs;
    nezha_bt_board board;
    IOService *radio;
    uint32_t started, pending, request, stopping, stopped, boardReady;
    thread_t workerIdentity;
    int stopError;
    bool service;
} runtime;
static uint64_t nowNs() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns; }
static void wake(void *) {
    __atomic_store_n(&runtime.pending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&runtime.pending);
}
static void waitUntil(void *, uint64_t deadline) {
    if (__atomic_exchange_n(&runtime.pending, 0u, __ATOMIC_ACQ_REL)) return;
    if (deadline == UINT64_MAX) assert_wait((event_t)&runtime.pending, THREAD_UNINT);
    else { uint64_t absolute; nanoseconds_to_absolutetime(deadline, &absolute); assert_wait_deadline((event_t)&runtime.pending, THREAD_UNINT, absolute); }
    if (__atomic_exchange_n(&runtime.pending, 0u, __ATOMIC_ACQ_REL)) clear_wait(current_thread(), THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
}
static size_t receive(void *, const uint8_t *p, size_t n) { return qbt_transport_receive(p, (uint32_t)n); }
static int request(void *, bool on) {
    if (__atomic_load_n(&runtime.stopping, __ATOMIC_ACQUIRE)) return ESHUTDOWN;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&runtime.request, &expected, on ? 1u : 2u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return EBUSY;
    if (!on && __atomic_load_n(&runtime.boardReady, __ATOMIC_ACQUIRE)) nezha_bt_board_cancel(&runtime.board);
    wake(nullptr); return 0;
}
static int publish() {
    IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane);
    OSData *encoding = node ? OSDynamicCast(OSData, node->getProperty("transport-encoding")) : nullptr;
    OSData *speed = node ? OSDynamicCast(OSData, node->getProperty("transport-speed")) : nullptr;
    uint32_t wire = 0, baud = 0;
    if (encoding && encoding->getLength() == 4) memcpy(&wire, encoding->getBytesNoCopy(), 4);
    if (speed && speed->getLength() == 4) memcpy(&baud, speed->getBytesNoCopy(), 4);
    bool ok = node && wire == 1 && baud == runtime.inputs.boot.operating_baud;
    if (ok) ok = node->setProperty("local-mac-address", runtime.inputs.address, 6);
    IOService *radio = ok ? new IOService : nullptr;
    if (!radio || !radio->init()) ok = false;
    if (ok) {
        radio->setName("bluetooth");
        ok = radio->setProperty("device_type", "bluetooth") && radio->setProperty("transport-encoding", encoding) &&
             radio->setProperty("transport-speed", speed) && radio->setProperty("local-mac-address", runtime.inputs.address, 6);
        IOService *parent = IOService::getPlatform();
        if (ok && parent && radio->attach(parent)) { runtime.radio = radio; radio->registerService(); }
        else ok = false;
    }
    if (node) node->release();
    if (!ok && radio) radio->release();
    return ok ? 0 : -ENOENT;
}
static int unpublish() {
    if (!runtime.radio) return 0;
    if (!runtime.radio->terminate()) return -EBUSY;
    /* IOService owns asynchronous client termination. Drop only our reference;
     * never force-detach a service from underneath an outstanding client. */
    runtime.radio->release(); runtime.radio = nullptr; return 0;
}
static void publishDiagnostic() {
    uint64_t state[32];qbt_stats stats={};
    for(unsigned i=0;i<32;i++)state[i]=native_bluetooth_stats[i];
    if(runtime.board.core)qbt_get_stats(runtime.board.core,&stats);
    nezha_bt_diagnostic_update(state,&stats,nowNs(),__atomic_load_n(&runtime.request,__ATOMIC_ACQUIRE),
        __atomic_load_n(&runtime.stopping,__ATOMIC_ACQUIRE),__atomic_load_n(&runtime.stopped,__ATOMIC_ACQUIRE));
}
static void snapshot() {
    if (!runtime.board.core) return;
    qbt_info info; qbt_stats stats;
    qbt_get_info(runtime.board.core, &info); qbt_get_stats(runtime.board.core, &stats);
    native_bluetooth_stats[2] = info.baud; native_bluetooth_stats[3] = stats.tx_bytes; native_bluetooth_stats[4] = stats.rx_bytes;
    native_bluetooth_stats[5] = runtime.board.geni.stats.interrupts; native_bluetooth_stats[6] = stats.timeouts;
    native_bluetooth_stats[7] = stats.clock_sleeps; native_bluetooth_stats[10] = runtime.board.phase;
    native_bluetooth_stats[14] = runtime.board.phase; native_bluetooth_stats[15] = runtime.board.geni.stats.line_errors;
    native_bluetooth_stats[16] = runtime.board.geni.stats.dma_errors; native_bluetooth_stats[17] = info.product;
    native_bluetooth_stats[18] = info.soc; native_bluetooth_stats[19] = (uint32_t)info.rom << 16 | info.patch;
    native_bluetooth_stats[20] = runtime.board.powered | ((unsigned)runtime.board.clocked << 2);
    native_bluetooth_stats[21] = nezha_wcn_power_owners();
    for (unsigned i = 0; i < 4; i++) native_bluetooth_stats[22 + i] = runtime.board.geni.fault_irq[i];
    native_bluetooth_stats[26] = stats.malformed; native_bluetooth_stats[27] = stats.unmatched_events;
    native_bluetooth_stats[28] = info.board_valid ? info.board : UINT32_MAX; native_bluetooth_stats[29] = stats.tlv_acks;
    publishDiagnostic();
}
static int result(bool on, int e) {
    qbt_info info = {}; if (runtime.board.core) qbt_get_info(runtime.board.core, &info);
    qbt_transport_power_result(on, e < 0 ? -e : e, info.firmware, info.baud);
    qbt_transport_stats pipe; qbt_transport_get_stats(&pipe);
    if (on && !e && pipe.boot) e = pipe.boot > 0 ? -pipe.boot : -EIO;
    native_bluetooth_stats[0] = e ? 3 : on ? 2 : 0; native_bluetooth_stats[1] = (uint64_t)(int64_t)e;
    snapshot();
    if(!runtime.board.core){
        native_bluetooth_stats[14]=runtime.board.phase;
        native_bluetooth_stats[20]=runtime.board.powered|((unsigned)runtime.board.clocked<<2);
        publishDiagnostic();
    }
    printf("[nezha bluetooth] power=%u result=%d phase=%u product=%x soc=%x rom=%x patch=%x board=%x valid=%u\n",
           on, e, runtime.board.phase, info.product, info.soc, info.rom, info.patch, info.board, info.board_valid);
    return e;
}
static void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "nezha-bluetooth");
    __atomic_store_n(&runtime.workerIdentity, current_thread(), __ATOMIC_RELEASE);
    nezha_bt_board_ops ops;
    int e = nezha_bt_platform_init(&ops, &runtime.inputs, &runtime, wake, waitUntil, receive);
    if (!e) e = nezha_bt_board_init(&runtime.board, &ops, &runtime.inputs.boot, runtime.inputs.address);
    if (!e) __atomic_store_n(&runtime.boardReady, 1u, __ATOMIC_RELEASE);
    qbt_transport_ops transport = {&runtime, wake, request,
        (uint32_t)(nezha_bt_board_boot_budget_ns(&runtime.inputs.boot)/1000000000ULL)+5u};
    if (!e) e = qbt_transport_init(&transport); /* XNU interface uses positive errno */
    if (e) goto done;
    {
        uint64_t deadline = nowNs() + 300000000000ULL;
        while (!nezha_wifi_platform_ready()) {
            if (__atomic_load_n(&runtime.stopping, __ATOMIC_ACQUIRE)) { e = -ECANCELED; break; }
            if (nowNs() >= deadline) { e = -ETIMEDOUT; break; }
            waitUntil(nullptr, nowNs() + 10000000ULL);
        }
        if (e) goto done;
    }
    /* Wait for the existing addon via a real file read, not a GPU progress
     * counter. Only absent/not-yet-mounted inputs are retried, for 30 seconds. */
    {
        uint64_t deadline = nowNs() + 30000000000ULL;
        /* Read the patch to prove the addon is present. NVM is selected only
         * after the controller returns its real board ID during startup. */
        const char *names[1] = {runtime.inputs.patch};
        for (unsigned index = 0; index < 1; index++) {
          for (;;) {
            const uint8_t *bytes = nullptr; size_t length = 0;
            char patch[64]; snprintf(patch, sizeof(patch), "qca/%s", names[index]);
            e = nezha_bt_firmware_read(&runtime.inputs, patch, &bytes, &length);
            if (!e) { nezha_bt_firmware_release(nullptr, bytes, length); break; }
            if ((e != -ENOENT && e != -EAGAIN) || nowNs() >= deadline || __atomic_load_n(&runtime.stopping, __ATOMIC_ACQUIRE)) break;
            waitUntil(nullptr, nowNs() + 10000000ULL);
          }
          if (e) break;
        }
    }
    if (!e && !__atomic_load_n(&runtime.stopping, __ATOMIC_ACQUIRE)) e = nezha_bt_board_power_on(&runtime.board);
    else if (!e) e = -ECANCELED;
    e = result(true, e);
    printf("[nezha bluetooth] diagnostic provider10 registered=%d\n",nezha_bt_diagnostic_init());
    if (e && runtime.board.powered) { (void)nezha_bt_board_power_off(&runtime.board); (void)result(false, e); }
    {
        uint64_t health = nowNs() + 1000000000ULL, closeAt = 0;
        while (!__atomic_load_n(&runtime.stopping, __ATOMIC_ACQUIRE)) {
            uint32_t command = __atomic_exchange_n(&runtime.request, 0u, __ATOMIC_ACQ_REL);
            if (command) {
                e = command == 1 ? nezha_bt_board_power_on(&runtime.board) : nezha_bt_board_power_off(&runtime.board);
                runtime.service = false; closeAt = 0; e = result(command == 1, e);
                if (e && command == 1 && runtime.board.powered) { (void)nezha_bt_board_power_off(&runtime.board); (void)result(false, e); }
            }
            if (runtime.board.powered) {
                bool active = qbt_transport_active();
                if (active != runtime.service) {
                    qbt_set_service(runtime.board.core, active); runtime.service = active;
                    closeAt = active ? 0 : nowNs() + 500000000ULL;
                }
                uint8_t tx[2048]; size_t room = qbt_transmit_capacity(runtime.board.core);
                if (room > sizeof(tx)) room = sizeof(tx);
                uint32_t n = room ? qbt_transport_tx_take(tx, (uint32_t)room) : 0;
                if (n && qbt_transmit(runtime.board.core, tx, n)) qbt_fail(runtime.board.core, -EIO);
                nezha_bt_board_step(&runtime.board);
                qbt_info info; qbt_get_info(runtime.board.core, &info);
                if (info.state == 3 || (closeAt && nowNs() >= closeAt)) {
                    e = nezha_bt_board_power_off(&runtime.board); runtime.service = false; closeAt = 0;
                    result(false, info.state == 3 ? info.error : e);
                }
            } else runtime.service = false;
            uint64_t now = nowNs();
            if (now >= health) { snapshot(); health = now + 1000000000ULL; }
            uint64_t deadline = nezha_bt_board_deadline(&runtime.board);
            if (health < deadline) deadline = health;
            if (closeAt && closeAt < deadline) deadline = closeAt;
            if (!deadline) { thread_yield_to_preemption(); continue; }
            waitUntil(nullptr, deadline);
        }
    }
done:
    (void)nezha_bt_diagnostic_init();
    __atomic_store_n(&runtime.stopping, 1u, __ATOMIC_RELEASE);
    uint32_t abandoned = __atomic_exchange_n(&runtime.request, 0u, __ATOMIC_ACQ_REL);
    if (abandoned) result(abandoned == 1, -ESHUTDOWN);
    int off = 0;
    if (runtime.board.core) {
        off = nezha_bt_board_destroy(&runtime.board);
        result(false, off);
    }
    int serviceError = unpublish(); if (!off) off = serviceError;
    runtime.stopError = off;
    printf("[nezha bluetooth] worker exit initialization=%d cleanup=%d\n", e, off);
    native_bluetooth_stats[30] = 1; native_bluetooth_stats[31] = (uint64_t)(int64_t)off;
    __atomic_store_n(&runtime.workerIdentity, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&runtime.stopped, 1u, __ATOMIC_RELEASE); wake(nullptr);
    publishDiagnostic();
}
}
extern "C" void nezha_bluetooth_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("nezha-bluetooth", &enabled, sizeof(enabled)) || enabled != 1) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&runtime.started, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    native_bluetooth_stats[0] = 1;
    native_bluetooth_stats[8] = UINT32_MAX;
    native_bluetooth_stats[9] = UINT32_MAX;
    int e = nezha_bt_inputs_read(&runtime.inputs); if (!e) e = publish();
    thread_t thread = nullptr;
    if (!e && kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) { thread_deallocate(thread); return; }
    if (!e) e = -ENOMEM;
    /* No UART worker or hardware lease exists on this failure path. Keep
     * the initialization error in stats; only failed service cleanup may
     * block the later normal USB shutdown. */
    runtime.stopError = unpublish();
    printf("[nezha bluetooth] start failed initialization=%d cleanup=%d\n", e, runtime.stopError);
    native_bluetooth_stats[0] = 3; native_bluetooth_stats[1] = (uint64_t)(int64_t)e;
    __atomic_store_n(&runtime.stopped, 1u, __ATOMIC_RELEASE);
}
extern "C" void nezha_bluetooth_register(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("nezha-bluetooth", &enabled, sizeof(enabled)) || enabled != 1) return;
    static uint32_t registered;
    if (__atomic_exchange_n(&registered, 1u, __ATOMIC_ACQ_REL)) return;
    int e = net_init_add(nezha_bluetooth_probe);
    if (e == EALREADY) nezha_bluetooth_probe();
    else if (e) printf("[nezha bluetooth] net_init_add failed %d; worker not started\n", e);
}
extern "C" void nezha_bluetooth_stop(void) {
    if (!__atomic_load_n(&runtime.started, __ATOMIC_ACQUIRE)) return;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&runtime.stopping, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&runtime.boardReady, __ATOMIC_ACQUIRE)) nezha_bt_board_stop(&runtime.board);
        wake(nullptr);
    }
}
extern "C" int nezha_bluetooth_stop_and_wait(uint32_t ms) {
    if (!ms || ms > 30000) return -EINVAL;
    if (__atomic_load_n(&runtime.workerIdentity, __ATOMIC_ACQUIRE) == current_thread()) return -EDEADLK;
    nezha_bluetooth_stop();
    uint64_t end = nowNs() + (uint64_t)ms * 1000000ULL;
    while (__atomic_load_n(&runtime.started, __ATOMIC_ACQUIRE) && !__atomic_load_n(&runtime.stopped, __ATOMIC_ACQUIRE)) {
        if (nowNs() >= end) return -ETIMEDOUT;
        /* A different normal shutdown thread; never shares the UART worker's
         * event wait, and never runs from interrupt context. */
        IOSleep(1);
    }
    return runtime.stopError;
}
