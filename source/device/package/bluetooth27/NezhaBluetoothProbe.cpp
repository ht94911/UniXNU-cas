/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Reuses sheng's native UART.SK worker/IOService path. Pairing, security,
 * profiles and persistent bonds remain with the selected iOS system stack. */
#include "NezhaBluetoothPlatform.h"
#include "NezhaWcnPower.h"
#include "qbt_transport.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/errno.h>
extern "C" {
volatile uint64_t native_bluetooth_stats[32];
uint8_t native_bluetooth_boot_rx[512],native_bluetooth_boot_tail[256];
volatile uint32_t native_bluetooth_boot_rx_length,native_bluetooth_boot_tail_length,native_bluetooth_boot_sequence;
volatile uint64_t native_bluetooth_command_state[2];
void nezha_bluetooth_diagnostic_register(void);
}
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
static void snapshot();
static void progress(void *) {
    static uint64_t next;
    uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    if(ns<next)return;next=ns+100000000ULL;snapshot();
}
static void resetBootstrap() {
    __atomic_add_fetch(&native_bluetooth_boot_sequence,1u,__ATOMIC_ACQ_REL);
    native_bluetooth_boot_rx_length=native_bluetooth_boot_tail_length=0;
    memset(native_bluetooth_boot_rx,0,sizeof(native_bluetooth_boot_rx));
    memset(native_bluetooth_boot_tail,0,sizeof(native_bluetooth_boot_tail));
    __atomic_add_fetch(&native_bluetooth_boot_sequence,1u,__ATOMIC_RELEASE);
}
static bool enabled() {
    unsigned value=0;
    if(PE_parse_boot_argn("nezha-bluetooth",&value,sizeof(value)))return value==1;
    DTEntry chosen;const void *data;unsigned bytes=0;
    if(SecureDTLookupEntry(nullptr,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"nezha-bt-enable",&data,&bytes)!=kSuccess || bytes!=4)return false;
    memcpy(&value,data,4);return value==1;
}
static void bootstrapRx(void *,const uint8_t *data,size_t bytes) {
    __atomic_add_fetch(&native_bluetooth_boot_sequence,1u,__ATOMIC_ACQ_REL);
    unsigned prefix=native_bluetooth_boot_rx_length;
    size_t copy=sizeof(native_bluetooth_boot_rx)-prefix;
    if(copy>bytes)copy=bytes;
    if(copy)memcpy(native_bluetooth_boot_rx+prefix,data,copy);
    native_bluetooth_boot_rx_length=prefix+(unsigned)copy;
    unsigned tail=native_bluetooth_boot_tail_length;
    if(bytes>=sizeof(native_bluetooth_boot_tail)) {
        memcpy(native_bluetooth_boot_tail,data+bytes-sizeof(native_bluetooth_boot_tail),sizeof(native_bluetooth_boot_tail));
        native_bluetooth_boot_tail_length=sizeof(native_bluetooth_boot_tail);
    } else {
        size_t keep=tail;
        if(keep>sizeof(native_bluetooth_boot_tail)-bytes)keep=sizeof(native_bluetooth_boot_tail)-bytes;
        memmove(native_bluetooth_boot_tail,native_bluetooth_boot_tail+tail-keep,keep);
        memcpy(native_bluetooth_boot_tail+keep,data,bytes);
        native_bluetooth_boot_tail_length=(unsigned)(keep+bytes);
    }
    __atomic_add_fetch(&native_bluetooth_boot_sequence,1u,__ATOMIC_RELEASE);
}
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
    native_bluetooth_command_state[0]=stats.last_opcode;native_bluetooth_command_state[1]=stats.last_status;
}
static int result(bool on, int e) {
    qbt_info info = {}; if (runtime.board.core) qbt_get_info(runtime.board.core, &info);
    qbt_transport_power_result(on, e < 0 ? -e : e, info.firmware, info.baud);
    qbt_transport_stats pipe; qbt_transport_get_stats(&pipe);
    if (on && !e && pipe.boot) e = pipe.boot > 0 ? -pipe.boot : -EIO;
    native_bluetooth_stats[0] = e ? 3 : on ? 2 : 0; native_bluetooth_stats[1] = (uint64_t)(int64_t)e;
    snapshot();
    printf("[nezha bluetooth] power=%u result=%d phase=%u product=%x soc=%x rom=%x patch=%x board=%x valid=%u\n",
           on, e, runtime.board.phase, info.product, info.soc, info.rom, info.patch, info.board, info.board_valid);
    return e;
}
static void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "nezha-bluetooth");
    __atomic_store_n(&runtime.workerIdentity, current_thread(), __ATOMIC_RELEASE);
    nezha_bt_board_ops ops;
    int e = nezha_bt_platform_init(&ops, &runtime.inputs, &runtime, wake, waitUntil, receive);
    if (!e) { ops.bootstrap_rx=bootstrapRx;ops.bootstrap_tick=progress; e = nezha_bt_board_init(&runtime.board, &ops, &runtime.inputs.boot, runtime.inputs.address); }
    if (!e) __atomic_store_n(&runtime.boardReady, 1u, __ATOMIC_RELEASE);
    qbt_transport_ops transport = {&runtime, wake, request};
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
    if (!e && !__atomic_load_n(&runtime.stopping, __ATOMIC_ACQUIRE)) { resetBootstrap();e = nezha_bt_board_power_on(&runtime.board); }
    else if (!e) e = -ECANCELED;
    e = result(true, e);
    if (e && runtime.board.powered) { (void)nezha_bt_board_power_off(&runtime.board); (void)result(false, e); }
    {
        uint64_t health = nowNs() + 1000000000ULL, closeAt = 0;
        while (!__atomic_load_n(&runtime.stopping, __ATOMIC_ACQUIRE)) {
            uint32_t command = __atomic_exchange_n(&runtime.request, 0u, __ATOMIC_ACQ_REL);
            if (command) {
                if(command==1 && !runtime.board.powered)resetBootstrap();
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
}
}
extern "C" void nezha_bluetooth_probe(void) {
    if(!enabled())return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&runtime.started, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    nezha_bluetooth_diagnostic_register();
    native_bluetooth_stats[0] = 1; native_bluetooth_stats[8] = native_bluetooth_stats[9] = UINT32_MAX;
    int e = nezha_bt_inputs_read(&runtime.inputs); if (!e) e = publish();
    thread_t thread = nullptr;
    if (!e && kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) { thread_deallocate(thread); return; }
    if (!e) e = -ENOMEM;
    (void)unpublish(); runtime.stopError = e;
    native_bluetooth_stats[0] = 3; native_bluetooth_stats[1] = (uint64_t)(int64_t)e;
    __atomic_store_n(&runtime.stopped, 1u, __ATOMIC_RELEASE);
}
extern "C" void nezha_bluetooth_register(void) {
    if(!enabled())return;
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
