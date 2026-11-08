/* SPDX-License-Identifier: GPL-2.0-only */
/* IOKit binding of the reused GENI driver and the N0 SM8850 contracts.
 * Shared owner hooks are integrated with other consumers only in the approved
 * package. No raw address, regulator name or HCI payload is accepted from USB. */
#include "NezhaBluetoothPlatform.h"
#include "NezhaWcnPower.h"
#include "Sm8850Bt.h"
extern "C" {
#include "Sm8850Rpmh.h"
}
#include "qbt_geni_regs.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <sys/systm.h>
#include <sys/errno.h>
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
extern "C" int armcpu_spi_mask(uint32_t, bool);
extern "C" void nezha_wifi_domain_lock(void);
extern "C" void nezha_wifi_domain_unlock(void);
namespace {
struct Region { IOMemoryMap *map; volatile uint32_t *va; uint32_t bytes; };
static void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
static uint64_t nowNs() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns; }
static int map(Region &r, uint64_t pa, uint32_t bytes) {
    if (r.va) return r.bytes == bytes ? 0 : -EINVAL;
    IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
    r.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (m) m->release();
    if (!r.map) return -ENOMEM;
    r.va = reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()); r.bytes = bytes;
    return r.va ? 0 : -ENOMEM;
}
static void unmap(Region &r) { if (r.map) r.map->release(); r = {}; }
struct Shared {
    Region window[7], rsc, db, sw;
    sm8850_bt_pmic pmic;
    Sm8850Rpmh rpmh;
    uint64_t lease[3], generation;
    uint32_t owners, rails;
    bool mapped, failed;
} shared;
/* Wi-Fi023's existing normal-thread apps-domain mutex also serializes this
 * RSC/GCC foundation. USB and touch finish their fixed claims before either
 * radio starts. Maps and the single RSC scheduler survive radio toggles. */
struct Guard {
    int error;
    Guard() : error(nezha_wcn_resource_lock()) {}
    ~Guard() { if (!error) nezha_wcn_resource_unlock(); }
};
static uint32_t readShared(void *, unsigned w, uint32_t off) { return shared.window[w].va[off / 4]; }
static void writeShared(void *, unsigned w, uint32_t off, uint32_t v) { shared.window[w].va[off / 4] = v; barrier(); }
static void delayShared(void *, unsigned us) { IODelay(us); }
static const sm8850_bt_io io = {nullptr, readShared, writeShared, delayShared};
static int rscRead(void *, unsigned w, uint32_t off, uint32_t *v) {
    Region &r = w == Sm8850RpmhRsc ? shared.rsc : shared.db;
    if (!v || !r.va || off > r.bytes - 4 || (off & 3)) return -EINVAL;
    *v = r.va[off / 4]; barrier(); return 0;
}
static int rscWrite(void *, unsigned w, uint32_t off, uint32_t v) {
    if (w != Sm8850RpmhRsc || !shared.rsc.va || off > shared.rsc.bytes - 4 || (off & 3)) return -EINVAL;
    shared.rsc.va[off / 4] = v; barrier(); return 0;
}
static uint64_t rscNow(void *) { return nowNs() / 1000; }
static int prepareShared() {
    if (shared.failed) return -EIO;
    if (shared.mapped) return 0;
    static const struct { uint64_t pa; uint32_t bytes; } resources[] = {
        {SM8850_BT_GCC_BASE, SM8850_BT_GCC_BYTES}, {SM8850_BT_SMMU_BASE, 0x100000},
        {SM8850_BT_PMIC_CORE_BASE, 0x1000}, {SM8850_BT_PMIC_MAP_BASE, 0x8000},
        {SM8850_BT_PMIC_OWNER_BASE, 0x8000}, {SM8850_BT_PMIC_OBSERVER_BASE, 0x40000},
        {SM8850_BT_PMIC_WRITE_BASE, 0x400000}
    };
    int e = 0;
    for (unsigned w = 0; !e && w < 7; w++) e = map(shared.window[w], resources[w].pa, resources[w].bytes);
    if (!e) e = map(shared.rsc, 0x18920000, 0x10000);
    if (!e) e = map(shared.db, 0x81c60000, 0x20000);
    if (!e) e = map(shared.sw, 0x0f112000, 0x1000); /* GPIO18, wcn_sw_ctrl */
    if (e) {
        for (unsigned w = 0; w < 7; w++) unmap(shared.window[w]);
        unmap(shared.rsc); unmap(shared.db); unmap(shared.sw); return e;
    }
    shared.rpmh = {nullptr, rscRead, rscWrite, rscNow, delayShared, shared.db.bytes, shared.rsc.bytes, 0};
    shared.mapped = true; return 0;
}
static int rpmhError(int e) {
    switch (e) {
    case 0: return 0;
    case -1: return -EINVAL;
    case -2: return -EIO;
    case -3: return -EBUSY;
    case -4: return -ETIMEDOUT;
    case -5: return -ENOENT;
    default: return -EIO;
    }
}
static int vote(const char *name, uint32_t offset, uint32_t value) {
    return rpmhError(Sm8850RpmhResource(&shared.rpmh, name, offset, value));
}
struct Platform {
    Region uart, qup, pins;
    IOBufferMemoryDescriptor *memory;
    sm8850_bt_clock clock;
    sm8850_bt_dma_route route;
    const nezha_bt_inputs *inputs;
    void *executor;
    void (*wake)(void *);
    void (*wait)(void *, uint64_t);
    size_t (*receive)(void *, const uint8_t *, size_t);
    void (*irq)(void *);
    void *irqContext;
    uint32_t irqBusy, savedPin[4];
    bool initialized, claimed, registeredIrq, pinsSaved, retired;
} platform;
static void irqTrampoline(void *) {
    (void)__atomic_add_fetch(&platform.irqBusy, 1u, __ATOMIC_ACQ_REL);
    (void)armcpu_spi_mask(NEZHA_BT_UART_INTID, true);
    void (*handler)(void *) = __atomic_load_n(&platform.irq, __ATOMIC_ACQUIRE);
    if (handler) handler(platform.irqContext);
    (void)__atomic_sub_fetch(&platform.irqBusy, 1u, __ATOMIC_RELEASE);
}
static uint32_t uartRead(void *, uint32_t r) { return platform.uart.va[r / 4]; }
static void uartWrite(void *, uint32_t r, uint32_t v) {
    if (r == G_S_CMD || r == G_M_CMD || r == G_TX_DMA_LEN || r == G_RX_DMA_LEN) platform.retired = false;
    platform.uart.va[r / 4] = v; barrier();
}
static void dmaBarrier(void *) { barrier(); }
static void dmaSync(void *, const qbt_dma_buffer *d, size_t bytes, bool device, bool tx) {
    if (bytes && (device || !tx)) {
        uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        size_t line = 4u << ((ctr >> 16) & 15);
        uintptr_t at = reinterpret_cast<uintptr_t>(d->bytes) & ~(line - 1), end = reinterpret_cast<uintptr_t>(d->bytes) + bytes;
        for (; at < end; at += line) {
            if (tx) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
            else if (device) __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
            else __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
        }
    }
    barrier();
}
static void irqMask(void *, bool mask) { if (platform.registeredIrq) (void)armcpu_spi_mask(NEZHA_BT_UART_INTID, mask); }
static void wake(void *) { platform.wake(platform.executor); }
static void wait(void *, uint64_t t) { platform.wait(platform.executor, t); }
static uint64_t time(void *) { return nowNs(); }
static void *allocate(void *, size_t n) { return IOMallocData(n); }
static void release(void *, void *p, size_t n) { IOFreeData(p, n); }
static size_t receive(void *, const uint8_t *p, size_t n) { return platform.receive(platform.executor, p, n); }
static void log(void *, int level, const char *s) { if (level <= 6) printf("[nezha bluetooth] %s\n", s); }
static int resources(void *, nezha_bt_dma *d) {
    Guard guard; if (guard.error) return guard.error;
    if (platform.claimed) return -EBUSY;
    platform.claimed = true; d->cookie = &platform;
    int e = prepareShared();
    if (!e) e = map(platform.uart, NEZHA_BT_UART_BASE, 0x4000);
    if (!e) e = map(platform.qup, NEZHA_BT_QUP_BASE, 0x2000);
    if (!e) e = map(platform.pins, 0x0f118000, 0x4000); /* TLMM tile +0x100000, GPIO24..27 */
    if (e) return e;
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    if ((4u << ((ctr >> 16) & 15)) > 64) return -ENOTSUP;
    const IOOptionBits flags = static_cast<IOOptionBits>(kIODirectionInOut) | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone;
    platform.memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, flags, 32768, 4096);
    if (!platform.memory) return -ENOMEM;
    IOByteCount contiguous = 0;
    uint64_t pa = platform.memory->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    uint8_t *bytes = static_cast<uint8_t *>(platform.memory->getBytesNoCopy());
    if (!bytes || !pa || contiguous < 32768 || pa >= (UINT64_C(1) << 36) || 32768 > (UINT64_C(1) << 36) - pa) return -ERANGE;
    d->uart_base = NEZHA_BT_UART_BASE; d->qup_base = NEZHA_BT_QUP_BASE; d->intid = NEZHA_BT_UART_INTID; d->sid = NEZHA_BT_QUP_SID;
    d->tx = {bytes, pa, QBT_GENI_TX_SIZE};
    for (unsigned i = 0; i < QBT_GENI_RX_COUNT; i++) d->rx[i] = {bytes + 4096 + i * QBT_GENI_RX_SIZE, pa + 4096 + i * QBT_GENI_RX_SIZE, QBT_GENI_RX_SIZE};
    platform.retired = false; return 0;
}
static int mapDma(void *, nezha_bt_dma *d) {
    Guard guard; if (guard.error) return guard.error;
    if (d->cookie != &platform || !platform.retired) return -EBUSY;
    int e = sm8850_bt_dma_get(&io, &platform.route, d->tx.address, 32768);
    if (!e) { native_bluetooth_stats[8] = platform.route.slot; native_bluetooth_stats[9] = platform.route.route & 255u; }
    return e;
}
static int releaseResources(void *, nezha_bt_dma *d) {
    Guard guard; if (guard.error) return guard.error;
    if (!d->cookie) return 0;
    if (d->cookie != &platform || __atomic_load_n(&platform.irq, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&platform.irqBusy, __ATOMIC_ACQUIRE) || (platform.route.acquired && !platform.retired)) return -EBUSY;
    int e = sm8850_bt_dma_put(&io, &platform.route); if (e) return e;
    if (platform.memory) platform.memory->release(); platform.memory = nullptr;
    unmap(platform.uart); unmap(platform.qup); unmap(platform.pins);
    platform.claimed = platform.pinsSaved = platform.retired = false; d->cookie = nullptr; return 0;
}
static int serialClock(void *, bool on) {
    Guard guard; if (guard.error) return guard.error;
    return on ? sm8850_bt_clock_get(&io, &platform.clock) : sm8850_bt_clock_put(&io, &platform.clock);
}
static int serialSource(void *, uint32_t baud, uint32_t *hz) {
    Guard guard; if (guard.error) return guard.error;
    if (!platform.uart.va || !platform.clock.acquired || platform.clock.failed) return -ENXIO;
    if ((uartRead(nullptr, G_STATUS) & (G_M_ACTIVE | G_S_ACTIVE)) || (uartRead(nullptr, 0xe40) & 3) ||
        uartRead(nullptr, G_TX_FIFO_STATUS)) return -EBUSY;
    platform.retired = true;
    return sm8850_bt_clock_source(&io, &platform.clock, baud, hz);
}
static int serialRetire(void *) {
    if (!platform.uart.va || !platform.clock.acquired) return -ENXIO;
    irqMask(nullptr, true); uartWrite(nullptr, G_IRQ_EN, 0);
    native_bluetooth_stats[11] = uartRead(nullptr, G_FW_REV); native_bluetooth_stats[12] = platform.qup.va[1];
    native_bluetooth_stats[13] = shared.window[SM8850_BT_GCC].va[0x52018 / 4];
    if (((uartRead(nullptr, G_FW_REV) >> 8) & 255u) != 2) return -ENXIO;
    if ((uartRead(nullptr, G_STATUS) & (G_M_ACTIVE | G_S_ACTIVE)) || (uartRead(nullptr, 0xe40) & 3)) {
        uartWrite(nullptr, G_M_CTRL, G_ABORT); uartWrite(nullptr, G_S_CTRL, G_ABORT);
        unsigned n;
        for (n = 0; n < 1000 && (uartRead(nullptr, G_STATUS) & (G_M_ACTIVE | G_S_ACTIVE)); n++) IODelay(1);
        if (n == 1000) return -ETIMEDOUT;
        uartWrite(nullptr, 0xc58, 1); uartWrite(nullptr, G_RX_RESET, 1);
        for (n = 0; n < 1000 && (uartRead(nullptr, 0xe40) & 3); n++) IODelay(1);
        if (n == 1000) return -ETIMEDOUT;
    }
    uartWrite(nullptr, G_M_CLEAR, UINT32_MAX); uartWrite(nullptr, G_S_CLEAR, UINT32_MAX);
    uartWrite(nullptr, G_TX_CLEAR, UINT32_MAX); uartWrite(nullptr, G_RX_CLEAR, UINT32_MAX);
    platform.retired = true; return 0;
}
static int defaultPins(void *) {
    if (!platform.pins.va) return -ENXIO;
    if (platform.pinsSaved) return -EBUSY;
    for (unsigned i = 0; i < 4; i++) platform.savedPin[i] = platform.pins.va[i * 1024];
    platform.pinsSaved = true;
    static const uint32_t pulls[4] = {0, 1, 3, 1}; /* default CTS/RTS/TX/RX from live DT */
    for (unsigned i = 0; i < 4; i++) {
        uint32_t cfg = (platform.savedPin[i] & ~0x3ffu) | pulls[i];
        platform.pins.va[i * 1024] = cfg; barrier();
        if (platform.pins.va[i * 1024] != cfg) return -EIO;
    }
    return 0;
}
static int pins(void *, bool active) {
    if (!platform.pins.va) return -ENXIO;
    if (!platform.pinsSaved) {
        if (!active) return 0;
        for (unsigned i = 0; i < 4; i++) platform.savedPin[i] = platform.pins.va[i * 1024];
        platform.pinsSaved = true;
    }
    static const uint32_t pulls[4] = {0, 1, 3, 0};
    for (unsigned i = 0; i < 4; i++) {
        uint32_t old = platform.pins.va[i * 1024];
        uint32_t v = active ? ((old & ~0x3ffu) | (1u << 2) | pulls[i]) : platform.savedPin[i];
        platform.pins.va[i * 1024] = v; barrier();
        if (platform.pins.va[i * 1024] != v) return -EIO;
    }
    if (!active) platform.pinsSaved = false;
    return 0;
}
static int attachIrq(void *, uint32_t id, void (*handler)(void *), void *ctx) {
    if (id != NEZHA_BT_UART_INTID || !handler || __atomic_load_n(&platform.irq, __ATOMIC_ACQUIRE)) return -EINVAL;
    if (!platform.registeredIrq) {
        int e = armcpu_spi_attach(id, false, irqTrampoline, nullptr, nullptr, nullptr);
        if (e) return -EIO;
        platform.registeredIrq = true;
    }
    irqMask(nullptr, true); platform.irqContext = ctx;
    __atomic_store_n(&platform.irq, handler, __ATOMIC_RELEASE); return 0;
}
static int detachIrq(void *, uint32_t id, void *ctx) {
    if (id != NEZHA_BT_UART_INTID) return -EINVAL;
    if (platform.irqContext && platform.irqContext != ctx) return -EBUSY;
    irqMask(nullptr, true); __atomic_store_n(&platform.irq, nullptr, __ATOMIC_RELEASE);
    for (unsigned n = 0; n < 1000; n++) {
        if (!__atomic_load_n(&platform.irqBusy, __ATOMIC_ACQUIRE)) { platform.irqContext = nullptr; return 0; }
        IODelay(100);
    }
    return -ETIMEDOUT;
}
static int powerGet(void *, uint32_t owner, uint64_t *lease) { return nezha_wcn_power_get(owner, lease); }
static int powerPut(void *, uint32_t owner, uint64_t lease) { return nezha_wcn_power_put(owner, lease); }
static int enable(void *, uint64_t lease, bool active) { return nezha_wcn_bt_enable(lease, active); }
static int firmware(void *, const char *name, const uint8_t **data, size_t *bytes) { return nezha_bt_firmware_read((void *)platform.inputs, name, data, bytes); }
static void firmwarePut(void *, const uint8_t *p, size_t bytes) { nezha_bt_firmware_release(nullptr, p, bytes); }
}
extern "C" int nezha_wcn_resource_lock(void) {
    nezha_wifi_domain_lock(); return 0;
}
extern "C" void nezha_wcn_resource_unlock(void) { nezha_wifi_domain_unlock(); }
extern "C" int nezha_wcn_rpmh_resource(const char *name, uint32_t offset, uint32_t value) {
    Guard guard; if (guard.error) return guard.error;
    int e = prepareShared(); return e ? e : vote(name, offset, value);
}
extern "C" int nezha_wcn_rpmh_arc(const char *name, uint32_t level) {
    Guard guard; if (guard.error) return guard.error;
    int e = prepareShared(); return e ? e : rpmhError(Sm8850RpmhArc(&shared.rpmh, name, level));
}
extern "C" int nezha_wcn_power_get(unsigned owner, uint64_t *lease) {
    if (!lease || owner < NEZHA_WCN_WLAN || owner > NEZHA_WCN_UWB) return -EINVAL;
    Guard guard; if (guard.error) return guard.error;
    int e = prepareShared(); if (e) return e;
    if (shared.lease[owner - 1]) { *lease = shared.lease[owner - 1]; return 0; }
    /* Voltage values match N0 and the qualified Wi-Fi path. S2J occurs once,
     * despite three logical BT supplies. No FMD/PDC/NVM programming. */
    static const struct { const char *name; uint16_t mv; uint8_t mode; } rails[] = {
        {"L3G_E0", 1200, 5}, {"L2G_E0", 1800, 5}, {"S2J_E1", 892, 0},
        {"S8F_E0", 1876, 0}, {"S7F_E0", 1328, 7}, {"L6K_E1", 1800, 0},
        {"L3K_E1", 1200, 7}, {"S1J_E1", 892, 0}
    };
    unsigned wanted = owner == NEZHA_WCN_WLAN ? 0xdfu : 0x3fu;
    *lease = shared.lease[owner - 1] = ++shared.generation;
    if (!*lease) { shared.failed = true; return -EOVERFLOW; }
    shared.owners |= 1u << owner;
    static const unsigned wlanOrder[8] = {0, 6, 1, 2, 3, 4, 7, 5};
    static const unsigned btOrder[8] = {1, 0, 5, 2, 3, 4, 6, 7};
    for (unsigned step = 0; step < 8; step++) {
        unsigned i = owner == NEZHA_WCN_WLAN ? wlanOrder[step] : btOrder[step];
        if (!(wanted & (1u << i)) || (shared.rails & (1u << i))) continue;
        e = vote(rails[i].name, 0, rails[i].mv);
        if (!e && rails[i].mode) e = vote(rails[i].name, 8, rails[i].mode);
        if (!e) e = vote(rails[i].name, 4, 1);
        if (e) { shared.failed = true; return e; }
        shared.rails |= 1u << i;
    }
    e = vote("C1A_E0", 4, 1);
    if (!e) {
        uint32_t cfg = (shared.sw.va[0] & ~0x3cu) | (1u << 2);
        shared.sw.va[0] = cfg; barrier();
        if (shared.sw.va[0] != cfg) e = -EIO;
    }
    if (e) shared.failed = true;
    return e;
}
extern "C" int nezha_wcn_power_put(unsigned owner, uint64_t lease) {
    if (owner < NEZHA_WCN_WLAN || owner > NEZHA_WCN_UWB || !lease) return -EINVAL;
    Guard guard; if (guard.error) return guard.error;
    if (shared.lease[owner - 1] != lease) return -EINVAL;
    if (shared.failed) return -EIO;
    /* Firmware and other platform consumers retain a baseline vote. Release
     * this radio's logical lease; do not infer that all shared consumers are
     * off. Full system sleep/power removal is a separate unvalidated stage. */
    shared.lease[owner - 1] = 0; shared.owners &= ~(1u << owner); return 0;
}
extern "C" int nezha_wcn_bt_enable(uint64_t lease, bool enabled) {
    Guard guard; if (guard.error) return guard.error;
    if (!lease || shared.lease[NEZHA_WCN_BLUETOOTH - 1] != lease) return -EINVAL;
    if (shared.owners & (1u << NEZHA_WCN_UWB)) return -EBUSY;
    if (shared.failed) return -EIO;
    return sm8850_bt_pmic_enable(&io, &shared.pmic, enabled);
}
extern "C" unsigned nezha_wcn_power_owners(void) {
    Guard guard; return guard.error ? UINT32_MAX : shared.owners;
}
extern "C" int nezha_bt_platform_init(nezha_bt_board_ops *o, const nezha_bt_inputs *inputs,
        void *executor, void (*wakeFn)(void *), void (*waitFn)(void *, uint64_t), size_t (*rx)(void *, const uint8_t *, size_t)) {
    if (!o || !inputs || !wakeFn || !waitFn || !rx) return -EINVAL;
    if (platform.initialized) return -EALREADY;
    platform.inputs = inputs; platform.executor = executor; platform.wake = wakeFn; platform.wait = waitFn; platform.receive = rx;
    *o = {};
    o->geni = {nullptr, uartRead, uartWrite, dmaBarrier, dmaSync, time, irqMask, wake, nullptr};
    o->allocate = allocate; o->release = release; o->now_ns = time; o->wait_until = wait; o->wake = wake;
    o->resource_acquire = resources; o->resource_dma_map = mapDma; o->resource_release = releaseResources;
    o->power_acquire = powerGet; o->power_release = powerPut; o->reset_enable = enable;
    o->pins_default = defaultPins; o->pins_active = pins; o->serial_clock = serialClock; o->serial_retire = serialRetire; o->serial_source = serialSource;
    o->irq_attach = attachIrq; o->irq_detach = detachIrq; o->firmware_get = firmware; o->firmware_put = firmwarePut;
    o->receive = receive; o->log = log; platform.initialized = true; return 0;
}
