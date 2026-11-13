/* SPDX-License-Identifier: MIT */
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>

#include "NezhaCameraCamcc.h"
#include "NezhaCameraOvx10500u.h"
#include "NezhaCameraRpmh.h"
#include "NezhaCameraProfile.h"
#include "NezhaCameraJournal.h"
#include "NezhaInputTelemetry.h"
#include <QcomUsbDiagnostic.h>

/* Stock nezha FDT: canoe-camcc, canoe-cambistmclkcc and CCI0. Map only
 * the pages of the recorded registers. Offsets checked against u0er
 * ed8978ac0087 and Qualcomm's Kaanapali camera clock/CCI source. */
enum {
    CameraClockPage = 0x958e000,
    MainMclkPage = 0x1764000,
    CameraCci0Page = 0x941b000,
    CameraTlmm = 0xf000000,
    CameraTlmmFirst = 89 * 0x1000,
    CameraTlmmBytes = (116 - 89 + 1) * 0x1000,
    AppsRscDrv2 = 0x18920000,
    AppsRscBytes = 0x10000,
    AopCommandDb = 0x81c60000,
    AopCommandDbBytes = 0x20000,
    RegisterPageBytes = 0x1000,
    CameraTopGdsc = 0x334,
    CameraTopStatus = 0x338,
    CameraCciCommand = 0x250,
    CameraCciConfig = 0x254,
    CameraCciBranch = 0x268,
    CameraCciVersion = 0,
    CameraCciIrqMask = 0xc04,
    CameraCciIrqStatus = 0xc0c,
    CameraCciMaster1Irqs = 0x60ee6000 | (1 << 12) | (1 << 16) | (1 << 20) | (1 << 24) | (1 << 26),
    CameraTopPowerUp = 1 << 16,
};
static const uint32_t clock_off = UINT32_C(1) << 31;

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern volatile uint32_t native_source_storage_boot_state;
extern volatile unsigned nezha27_usb_reset_owner_ready;
extern volatile int nezha_touch_stage;
extern void thread_terminate_self(void);

static unsigned requested_mode;

enum CameraDiagnosticPhase {
    CameraWaiting = 1, CameraMapped, CameraTop, CameraRate, CameraCci,
    CameraResources, CameraPower, CameraCciPrepared, CameraIdentity,
    CameraSensorRestored, CameraClockRestored, CameraFinished,
    CameraDependencyTimeout, CameraWorkerFailure,
};

static unsigned journal_token, usb_gated, probe_request;
enum CameraRequestState { CameraDisarmed, CameraArmed, CameraQueued, CameraRunning, CameraDone };

enum CameraHardwareMark {
    CameraBeforeMap = 20, CameraBeforeCamccRead, CameraBeforeMclkRead,
    CameraBeforeTop, CameraBeforeRate, CameraBeforeGate, CameraBeforeCciRead,
    CameraBeforePower, CameraBeforeCciPrepare, CameraBeforeSensorRead,
    CameraBeforeRestore,
};

enum CameraDiagnosticWord {
    DiagMode, DiagPhase, DiagStatus, DiagGdsc, DiagGdscStatus, DiagCoreAhb, DiagCpasAhb,
    DiagCciCommand, DiagCciConfig, DiagCciBranch, DiagCciVersion, DiagCciIrq,
    DiagCciQueue0, DiagCciQueue1, DiagCciFifo, DiagMclkCommand, DiagMclkConfig, DiagMclkBranch,
    DiagFailedStatus, DiagPowerOn, DiagPowerVotes, DiagRpmhStatus, DiagIdStatus, DiagIdWord,
    DiagIdBytes, DiagIdPhase, DiagIdIrq, DiagIdFifoWord, DiagCciQuiet, DiagPinsRestored,
    DiagClockRestore, DiagFailedPhase,
};

/* The camera worker is the sole producer. USB copies the existing HID
 * snapshot representation and performs no camera access in its callback. */
static uint32_t diagnostic_words[NZI_WORDS];
static uint32_t diagnostic_physical[NZI_WORDS];
static volatile struct nzi_frame camera_diagnostic;
static volatile struct nzi_frame camera_physical_diagnostic;

static void record_diagnostic(enum CameraDiagnosticPhase phase, unsigned status)
{
    diagnostic_words[DiagMode] = requested_mode;
    diagnostic_words[DiagPhase] = (unsigned)phase;
    diagnostic_words[DiagStatus] = status;
    if (status != 0 && diagnostic_words[DiagFailedPhase] == 0) {
        diagnostic_words[DiagFailedPhase] = (unsigned)phase;
        diagnostic_words[DiagFailedStatus] = status;
    }
    const uint32_t sequence = nzi_begin(&camera_diagnostic);
    const uint32_t physical_sequence = nzi_begin(&camera_physical_diagnostic);
    for (unsigned i = 0; i < NZI_WORDS; ++i) {
        nzi_put(&camera_diagnostic, i, diagnostic_words[i]);
        nzi_put(&camera_physical_diagnostic, i, diagnostic_physical[i]);
    }
    nzi_end(&camera_physical_diagnostic, physical_sequence);
    nzi_end(&camera_diagnostic, sequence);
}

static int camera_snapshot(void *cookie, char *output, size_t capacity, size_t *bytes, unsigned *truncated)
{
    (void)cookie;
    *bytes = 0;
    *truncated = 0;
    uint32_t words[NZI_WORDS];
    uint32_t physical[NZI_WORDS];
    const uint32_t sequence = __atomic_load_n(&camera_diagnostic.sequence, __ATOMIC_ACQUIRE);
    if (!nzi_copy(&camera_diagnostic, words) || !nzi_copy(&camera_physical_diagnostic, physical) ||
        sequence != __atomic_load_n(&camera_diagnostic.sequence, __ATOMIC_ACQUIRE) ||
        sequence != __atomic_load_n(&camera_physical_diagnostic.sequence, __ATOMIC_ACQUIRE)) {
        return QUD_BUSY;
    }
    const int size = snprintf(output, capacity,
        "{\"v\":1,\"mode\":%u,\"phase\":%u,\"status\":%u,\"first_failed_phase\":%u,\"first_failed_status\":%u,"
        "\"request\":%u,"
        "\"camcc\":[%u,%u,%u,%u],\"cci\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"mclk\":[%u,%u,%u],\"power\":[%u,%u,%u],\"identity\":[%u,%u,%u,%u,%u,%u],"
        "\"cleanup\":[%u,%u,%u],\"gpio\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"vrm\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],\"rsc\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
        words[DiagMode], words[DiagPhase], words[DiagStatus], words[DiagFailedPhase], words[DiagFailedStatus],
        __atomic_load_n(&probe_request, __ATOMIC_ACQUIRE),
        words[DiagGdsc], words[DiagGdscStatus], words[DiagCoreAhb], words[DiagCpasAhb],
        words[DiagCciCommand], words[DiagCciConfig], words[DiagCciBranch], words[DiagCciVersion],
        words[DiagCciIrq], words[DiagCciQueue0], words[DiagCciQueue1], words[DiagCciFifo],
        words[DiagMclkCommand], words[DiagMclkConfig], words[DiagMclkBranch],
        words[DiagPowerOn], words[DiagPowerVotes], words[DiagRpmhStatus],
        words[DiagIdStatus], words[DiagIdWord], words[DiagIdBytes], words[DiagIdPhase],
        words[DiagIdIrq], words[DiagIdFifoWord], words[DiagCciQuiet], words[DiagPinsRestored], words[DiagClockRestore],
        physical[0], physical[1], physical[2], physical[3], physical[4], physical[5],
        physical[6], physical[7], physical[8], physical[9], physical[10], physical[11],
        physical[12], physical[13], physical[14], physical[15], physical[16], physical[17],
        physical[18], physical[19], physical[20], physical[21], physical[22], physical[23],
        physical[24], physical[25], physical[26], physical[27], physical[28], physical[29], physical[30], physical[31]);
    if (size < 0 || (size_t)size >= capacity) {
        *truncated = 1;
        return QUD_FAILED;
    }
    *bytes = (size_t)size;
    return QUD_OK;
}

/* The existing USB worker only submits a fixed one-shot request. The
 * camera thread owns all file I/O, MMIO, delays and resource cleanup. */
static int camera_submit(void *cookie, const struct qud_diag_request *request,
    char *output, size_t capacity, size_t *bytes, unsigned *truncated)
{
    (void)cookie;
    *bytes = 0;
    *truncated = 0;
    if (usb_gated == 0 || journal_token == 0 || requested_mode != 3) {
        return QUD_UNSUPPORTED;
    }
    if (request == NULL || request->op != QUD_SUBMIT || request->provider != 28 ||
        request->arg0 != 1 || request->arg1 != journal_token || request->experiment || request->cursor) {
        return QUD_INVALID;
    }
    static const char reply[] = "{\"queued\":1}";
    if (capacity < sizeof(reply)) {
        return QUD_FAILED;
    }
    unsigned idle = CameraArmed;
    if (!__atomic_compare_exchange_n(&probe_request, &idle, CameraQueued, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return QUD_BUSY;
    }
    memcpy(output, reply, sizeof(reply));
    *bytes = sizeof(reply) - 1;
    return QUD_OK;
}

static const struct qud_diag_provider camera_provider = {
    28, 1, (1u << QUD_SNAPSHOT) | (1u << QUD_SUBMIT), 1, UINT32_MAX,
    "nezhacamera", NULL, camera_snapshot, camera_submit,
};

static bool journal_ready;

static bool hardware_mark(unsigned phase, unsigned status, unsigned a, unsigned b, unsigned c)
{
    if (journal_token == 0) {
        return true;
    }
    if (!journal_ready) {
        return false;
    }
    const int error = nezha_camera_journal_mark(phase, status, a, b, c);
    if (error != 0) {
        journal_ready = false;
        printf("NEZHA CAMERA: journal stopped phase=%u error=%d\n", phase, error);
        return false;
    }
    return true;
}

static vm_offset_t map_registers(vm_offset_t physical, vm_size_t bytes)
{
    return ml_io_map_unmappable(physical, bytes, 7 /* VM_WIMG_IO */);
}

static uint32_t read32(vm_offset_t page, uint32_t offset)
{
    const uint32_t value = *(volatile const uint32_t *)(page + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static vm_offset_t map_clocked_cci(vm_offset_t camcc)
{
    const uint32_t power = read32(camcc, CameraTopStatus);
    const uint32_t branch = read32(camcc, CameraCciBranch);
    const uint32_t root = read32(camcc, CameraCciCommand);
    if ((power & CameraTopPowerUp) == 0 || (branch & clock_off) != 0 ||
        (root & clock_off) != 0) {
        record_diagnostic(CameraCci, 2);
        printf("NEZHA CAMERA: CCI0 not clocked top=%#x root=%#x branch=%#x\n",
               power, root, branch);
        return 0;
    }

    const vm_offset_t cci = map_registers(CameraCci0Page, RegisterPageBytes);
    if (cci == 0) {
        record_diagnostic(CameraCci, 1);
        printf("NEZHA CAMERA: CCI0 map failed\n");
        return 0;
    }
    if (!hardware_mark(CameraBeforeCciRead, 0, CameraCci0Page, root, branch)) {
        ml_io_unmap(cci, RegisterPageBytes);
        return 0;
    }
    printf("NEZHA CAMERA: CCI0 hw=%#x irq-mask=%#x irq=%#x m1q0=%u m1q1=%u fifo=%u\n",
           read32(cci, CameraCciVersion), read32(cci, CameraCciIrqMask),
           read32(cci, CameraCciIrqStatus), read32(cci, 0x504), read32(cci, 0x604),
           read32(cci, 0x21c));
    diagnostic_words[DiagCciVersion] = read32(cci, CameraCciVersion);
    diagnostic_words[DiagCciIrq] = read32(cci, CameraCciIrqStatus);
    diagnostic_words[DiagCciQueue0] = read32(cci, 0x504);
    diagnostic_words[DiagCciQueue1] = read32(cci, 0x604);
    diagnostic_words[DiagCciFifo] = read32(cci, 0x21c);
    record_diagnostic(CameraCci, 0);
    (void)hardware_mark(CameraCci, 0, diagnostic_words[DiagCciVersion],
        diagnostic_words[DiagCciIrq], diagnostic_words[DiagCciFifo]);
    return cci;
}

static uint32_t read_page(void *cookie, uint32_t offset)
{
    return read32((vm_offset_t)cookie, offset);
}

static void write_page(void *cookie, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)((vm_offset_t)cookie + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}

static void wait_ms(void *cookie, unsigned milliseconds)
{
    (void)cookie;
    IOSleep(milliseconds);
}

static bool cci_idle(vm_offset_t cci)
{
    return read32(cci, 0x304) == 0 && read32(cci, 0x404) == 0 &&
        read32(cci, 0x504) == 0 && read32(cci, 0x604) == 0 &&
        read32(cci, 0x11c) == 0 && read32(cci, 0x21c) == 0;
}

static void record_main_physical(vm_offset_t gpio_base, const struct NezhaCameraRpmh *rpmh)
{
    /* Keep the hardware observation that produced the first error while
     * the primary record continues to publish the cleanup result. */
    if (diagnostic_words[DiagFailedPhase] != 0) {
        return;
    }
    const unsigned pins[] = {106, 107, 89, 111, 112, 116};
    for (unsigned i = 0; i < 6; ++i) {
        diagnostic_physical[2 * i] = read32(gpio_base, pins[i] * 0x1000);
        diagnostic_physical[2 * i + 1] = read32(gpio_base, pins[i] * 0x1000 + 4);
    }
    for (unsigned i = 0; i < 3; ++i) {
        const struct NezhaCameraVrmState *rail = &rpmh->rail[i];
        diagnostic_physical[12 + 4 * i] = rail->original_mv;
        diagnostic_physical[13 + 4 * i] = (unsigned)rail->original_on;
        diagnostic_physical[14 + 4 * i] = rail->observed_mv;
        diagnostic_physical[15 + 4 * i] = (unsigned)rail->observed_on;
    }
    diagnostic_physical[24] = rpmh->last_address;
    diagnostic_physical[25] = rpmh->last_value;
    diagnostic_physical[26] = rpmh->last_irq;
    diagnostic_physical[27] = rpmh->last_tcs_status;
    diagnostic_physical[28] = rpmh->last_command_status;
    diagnostic_physical[29] = rpmh->last_reply;
    diagnostic_physical[30] = (unsigned)rpmh->unsettled;
    diagnostic_physical[31] = (unsigned)rpmh->status;
}

static bool read_main_identity(vm_offset_t cci, unsigned cci_hz)
{
    if (!hardware_mark(CameraBeforeCciPrepare, 0, CameraCci0Page, cci_hz, 1)) {
        return true;
    }
    const struct NezhaCameraRegisterIo bus = {(void *)cci, read_page, write_page, wait_ms};
    const uint32_t mask = read32(cci, CameraCciIrqMask);
    uint32_t timing[5];
    if (mask == UINT32_MAX) {
        record_diagnostic(CameraCciPrepared, NezhaCciIoError);
        printf("NEZHA CAMERA: main CCI0 IRQ mask is unavailable\n");
        return true;
    }
    for (unsigned i = 0; i < 5; ++i) {
        timing[i] = read32(cci, 0x200 + i * 4);
    }
    write_page((void *)cci, CameraCciIrqMask, mask | CameraCciMaster1Irqs);
    if (read32(cci, CameraCciIrqMask) != (mask | CameraCciMaster1Irqs)) {
        printf("NEZHA CAMERA: main CCI0 IRQ mask did not latch\n");
        write_page((void *)cci, CameraCciIrqMask, mask);
        return read32(cci, CameraCciIrqMask) == mask;
    }

    struct NezhaCameraCciObservation prep;
    const enum NezhaCameraCciStatus prepared = nezha_camera_cci_recover_master(&bus, 1, cci_hz, &prep);
    record_diagnostic(CameraCciPrepared, (unsigned)prepared);
    (void)hardware_mark(CameraCciPrepared, (unsigned)prepared, (unsigned)prep.phase,
        prep.irq, prep.fifo_words);
    printf("NEZHA CAMERA: main CCI0 preparation=%d phase=%d irq=%#x fifo=%u\n",
           prepared, prep.phase, prep.irq, prep.fifo_words);
    if (prepared != NezhaCciOk) {
        write_page((void *)cci, CameraCciIrqMask, mask);
        return false;
    }

    struct NezhaCameraOvxIdentity id = {0};
    const enum NezhaCameraCciStatus status = hardware_mark(CameraBeforeSensorRead, 0, 0x10, 0x300b, 2) ?
        nezha_camera_ovx10500u_read_identity(&bus, &id) : NezhaCciIoError;
    const unsigned word = (unsigned)id.bytes[0] << 8 | id.bytes[1];
    diagnostic_words[DiagIdStatus] = (unsigned)status;
    diagnostic_words[DiagIdWord] = word;
    diagnostic_words[DiagIdBytes] = id.bytes_read;
    diagnostic_words[DiagIdPhase] = (unsigned)id.transfer.phase;
    diagnostic_words[DiagIdIrq] = id.transfer.irq;
    diagnostic_words[DiagIdFifoWord] = id.transfer.fifo_word;
    record_diagnostic(CameraIdentity, status != NezhaCciOk ? (unsigned)status :
                      word == NezhaCameraOvx10500uId ? 0 : NezhaCciInvalid);
    (void)hardware_mark(CameraIdentity, diagnostic_words[DiagStatus], word, id.bytes_read, id.transfer.irq);
    printf("NEZHA CAMERA: OVX10500U CCI0/M1 reg=0x300b bytes=%u id=%#x expected=%#x match=%u status=%d phase=%d irq=%#x fifo=%#x\n",
           id.bytes_read, word, (unsigned)NezhaCameraOvx10500uId,
           (unsigned)(status == NezhaCciOk && word == NezhaCameraOvx10500uId),
           status, id.transfer.phase, id.transfer.irq, id.transfer.fifo_word);
    bool quiet = cci_idle(cci);
    if (id.transfer.needs_recovery) {
        const enum NezhaCameraCciStatus recovered = nezha_camera_cci_recover_master(&bus, 1, cci_hz, &prep);
        printf("NEZHA CAMERA: main CCI0 recovery=%d phase=%d irq=%#x fifo=%u\n",
               recovered, prep.phase, prep.irq, prep.fifo_words);
        quiet = recovered == NezhaCciOk && cci_idle(cci);
    }
    if (quiet) {
        for (unsigned i = 0; i < 5; ++i) {
            write_page((void *)cci, 0x200 + i * 4, timing[i]);
        }
        for (unsigned i = 0; i < 5; ++i) {
            quiet = quiet && read32(cci, 0x200 + i * 4) == timing[i];
        }
    }
    write_page((void *)cci, CameraCciIrqMask, mask);
    return quiet && read32(cci, CameraCciIrqMask) == mask;
}

static bool main_identity_probe(vm_offset_t cci, vm_offset_t mclk, unsigned cci_hz)
{
    const uint32_t version = read32(cci, CameraCciVersion);
    if (mclk == 0 || version == 0 || version == UINT32_MAX || !cci_idle(cci)) {
        record_diagnostic(CameraResources, 3);
        printf("NEZHA CAMERA: main probe prerequisites missing CCI0=%#x MCLK0=%u\n",
               version, (unsigned)(mclk != 0));
        return true;
    }
    if (!hardware_mark(CameraBeforePower, 0, version, AppsRscDrv2, MainMclkPage)) {
        return true;
    }
    const vm_offset_t tlmm = map_registers(CameraTlmm + CameraTlmmFirst, CameraTlmmBytes);
    const vm_offset_t rsc = map_registers(AppsRscDrv2, AppsRscBytes);
    const vm_offset_t db = map_registers(AopCommandDb, AopCommandDbBytes);
    bool released = true;
    if (tlmm != 0 && rsc != 0 && db != 0) {
        const struct NezhaCameraRegisterIo gpio = {(void *)(tlmm - CameraTlmmFirst), read_page, write_page, wait_ms};
        const struct NezhaCameraRegisterIo clock = {(void *)mclk, read_page, write_page, wait_ms};
        const struct NezhaCameraRegisterIo apps = {(void *)rsc, read_page, write_page, wait_ms};
        struct NezhaCameraRpmh rpmh;
        if (nezha_camera_rpmh_init(&rpmh, &apps, (const uint8_t *)db, AopCommandDbBytes)) {
            record_diagnostic(CameraResources, 0);
            const struct NezhaCameraMainRailIo rails = {&rpmh, nezha_camera_rpmh_acquire, nezha_camera_rpmh_release};
            struct NezhaCameraMainPins pins = {0};
            const bool held = nezha_camera_main_hold_reset(&gpio, &pins);
            const bool powered = held && nezha_camera_main_start_pins(&gpio, &clock, &rails, &pins);
            diagnostic_words[DiagPowerOn] = (unsigned)powered;
            diagnostic_words[DiagPowerVotes] = pins.rails_requested;
            diagnostic_words[DiagRpmhStatus] = (unsigned)rpmh.status;
            diagnostic_words[DiagMclkCommand] = read32(mclk, 0);
            diagnostic_words[DiagMclkConfig] = read32(mclk, 4);
            diagnostic_words[DiagMclkBranch] = read32(mclk, 0x18);
            record_main_physical(tlmm - CameraTlmmFirst, &rpmh);
            record_diagnostic(CameraPower, powered ? 0 : 1);
            (void)hardware_mark(CameraPower, powered ? 0 : 1, pins.rails_requested,
                (unsigned)rpmh.status, rpmh.last_address);
            printf("NEZHA CAMERA: main power held=%u on=%u votes=%#x rpmh=%d pending=%u\n",
                   (unsigned)held, (unsigned)powered, pins.rails_requested, rpmh.status, (unsigned)rpmh.unsettled);
            const bool quiet = !powered || read_main_identity(cci, cci_hz);
            released = quiet && (!pins.reset_held || nezha_camera_main_restore_pins(&gpio, &clock, &rails, &pins));
            diagnostic_words[DiagCciQuiet] = (unsigned)quiet;
            diagnostic_words[DiagPinsRestored] = (unsigned)released;
            diagnostic_words[DiagRpmhStatus] = (unsigned)rpmh.status;
            record_main_physical(tlmm - CameraTlmmFirst, &rpmh);
            record_diagnostic(CameraSensorRestored, released ? 0 : 1);
            (void)hardware_mark(CameraSensorRestored, released ? 0 : 1, (unsigned)quiet,
                (unsigned)rpmh.status, pins.rails_requested);
            for (unsigned i = 0; i < 3; ++i) {
                const struct NezhaCameraVrmState *rail = &rpmh.rail[i];
                printf("NEZHA CAMERA: VRM[%u] addr=%#x start=%u/%u final=%u/%u owned=%u/%u\n",
                       i, rail->address, rail->original_mv, (unsigned)rail->original_on,
                       rail->observed_mv, (unsigned)rail->observed_on,
                       (unsigned)rail->voltage_changed, (unsigned)rail->enable_changed);
            }
            printf("NEZHA CAMERA: main release cci-idle=%u pins-and-rails=%u rpmh=%d\n",
                   (unsigned)quiet, (unsigned)released, rpmh.status);
        } else {
            record_diagnostic(CameraResources, 1);
            printf("NEZHA CAMERA: main RPMh configuration or resources did not match\n");
        }
    } else {
        record_diagnostic(CameraResources, 2);
        printf("NEZHA CAMERA: main TLMM, RSC or cmd-db map failed\n");
    }
    if (db != 0) {
        ml_io_unmap(db, AopCommandDbBytes);
    }
    if (rsc != 0) {
        ml_io_unmap(rsc, AppsRscBytes);
    }
    if (tlmm != 0) {
        ml_io_unmap(tlmm, CameraTlmmBytes);
    }
    return released;
}

static void camera_probe(void)
{
    if (!hardware_mark(CameraBeforeMap, 0, CameraClockPage, MainMclkPage, CameraCci0Page)) {
        return;
    }
    const vm_offset_t camcc = map_registers(CameraClockPage, RegisterPageBytes);
    if (camcc == 0) {
        record_diagnostic(CameraMapped, 1);
        printf("NEZHA CAMERA: CAMCC map failed\n");
        return;
    }
    if (!hardware_mark(CameraBeforeCamccRead, 0, CameraClockPage, CameraTopStatus, 0)) {
        ml_io_unmap(camcc, RegisterPageBytes);
        return;
    }
    const uint32_t top = read32(camcc, CameraTopStatus);
    record_diagnostic(CameraMapped, 0);
    printf("NEZHA CAMERA: mode=%u CAMCC gdsc=%#x status=%#x cci0 cmd=%#x cfg=%#x branch=%#x\n",
           requested_mode, read32(camcc, CameraTopGdsc), top,
           read32(camcc, CameraCciCommand), read32(camcc, CameraCciConfig),
           read32(camcc, CameraCciBranch));

    const vm_offset_t mclk = map_registers(MainMclkPage, RegisterPageBytes);
    if (mclk != 0 && hardware_mark(CameraBeforeMclkRead, 0, MainMclkPage, 0, 0)) {
        diagnostic_words[DiagMclkCommand] = read32(mclk, 0);
        diagnostic_words[DiagMclkConfig] = read32(mclk, 4);
        diagnostic_words[DiagMclkBranch] = read32(mclk, 0x18);
        printf("NEZHA CAMERA: MCLK0 cmd=%#x cfg=%#x branch=%#x\n",
               read32(mclk, 0), read32(mclk, 4), read32(mclk, 0x18));
    }

    if (requested_mode >= 2 && top != UINT32_MAX &&
        hardware_mark(CameraBeforeTop, 0, top, read32(camcc, CameraTopGdsc), 0)) {
        const struct NezhaCameraRegisterIo io = {(void *)(camcc - 0x21000u), read_page, write_page, wait_ms};
        struct NezhaCameraCamccLease lease = {0};
        const int acquire = nezha_camera_camcc_acquire_top(&io, &lease);
        diagnostic_words[DiagGdsc] = read32(camcc, CameraTopGdsc);
        diagnostic_words[DiagGdscStatus] = read32(camcc, CameraTopStatus);
        diagnostic_words[DiagCoreAhb] = read32(camcc, 0x348);
        diagnostic_words[DiagCpasAhb] = read32(camcc, 0x37c);
        record_diagnostic(CameraTop, (unsigned)acquire);
        (void)hardware_mark(CameraTop, (unsigned)acquire, diagnostic_words[DiagGdscStatus],
            diagnostic_words[DiagCoreAhb], diagnostic_words[DiagCpasAhb]);
        printf("NEZHA CAMERA: top result=%d gdsc=%#x status=%#x core=%#x cpas=%#x owned=%#x\n",
               acquire, read32(camcc, CameraTopGdsc), read32(camcc, CameraTopStatus),
               read32(camcc, 0x348), read32(camcc, 0x37c), lease.enabled_by_us);
        int rate = NezhaCameraCamccInvalid;
        int gate = NezhaCameraCamccInvalid;
        bool quiet = true;
        if (acquire == NezhaCameraCamccOk &&
            hardware_mark(CameraBeforeRate, 0, diagnostic_words[DiagCciCommand], 19200000, 0)) {
            rate = nezha_camera_camcc_select_cci0_rate(&io, &lease, 19200000);
            diagnostic_words[DiagCciCommand] = read32(camcc, CameraCciCommand);
            diagnostic_words[DiagCciConfig] = read32(camcc, CameraCciConfig);
            diagnostic_words[DiagCciBranch] = read32(camcc, CameraCciBranch);
            record_diagnostic(CameraRate, (unsigned)rate);
            (void)hardware_mark(CameraRate, (unsigned)rate, diagnostic_words[DiagCciCommand],
                diagnostic_words[DiagCciConfig], diagnostic_words[DiagCciBranch]);
            printf("NEZHA CAMERA: CCI0 rate result=%d cmd=%#x cfg=%#x branch=%#x\n",
                   rate, read32(camcc, CameraCciCommand), read32(camcc, CameraCciConfig),
                   read32(camcc, CameraCciBranch));
            if (rate == NezhaCameraCamccOk &&
                hardware_mark(CameraBeforeGate, 0, diagnostic_words[DiagCciBranch], 0, 0)) {
                gate = nezha_camera_camcc_enable_cci0(&io, &lease);
                diagnostic_words[DiagCciBranch] = read32(camcc, CameraCciBranch);
                record_diagnostic(CameraCci, (unsigned)gate);
                if (gate == NezhaCameraCamccOk) {
                    const vm_offset_t cci = map_clocked_cci(camcc);
                    if (cci != 0) {
                        if (requested_mode == 3) {
                            quiet = main_identity_probe(cci, mclk, 19200000);
                        }
                        ml_io_unmap(cci, RegisterPageBytes);
                    }
                }
            }
        }
        (void)hardware_mark(CameraBeforeRestore, 0, lease.enabled_by_us, (unsigned)quiet, 0);
        const int restore = quiet ? nezha_camera_camcc_release(&io, &lease) : NezhaCameraCamccBusy;
        diagnostic_words[DiagClockRestore] = (unsigned)restore;
        record_diagnostic(CameraClockRestored, (unsigned)restore);
        (void)hardware_mark(CameraClockRestored, (unsigned)restore, lease.enabled_by_us,
            (unsigned)rate, (unsigned)gate);
        printf("NEZHA CAMERA: clock probe top=%d rate=%d gate=%d restore=%d owned=%#x rate-owned=%u\n",
               acquire, rate, gate, restore, lease.enabled_by_us, (unsigned)lease.cci_rate_owned);
    } else if (requested_mode == 1) {
        const vm_offset_t cci = map_clocked_cci(camcc);
        if (cci != 0) {
            ml_io_unmap(cci, RegisterPageBytes);
        }
    }
    if (mclk != 0) {
        ml_io_unmap(mclk, RegisterPageBytes);
    }
    ml_io_unmap(camcc, RegisterPageBytes);
    record_diagnostic(CameraFinished, 0);
    (void)hardware_mark(CameraFinished, diagnostic_words[DiagFailedStatus],
        diagnostic_words[DiagFailedPhase], diagnostic_words[DiagIdWord], diagnostic_words[DiagClockRestore]);
}

static void camera_probe_thread(void *argument, wait_result_t wait_result)
{
    (void)argument;
    (void)wait_result;
    thread_set_thread_name(current_thread(), "nezha27-camera-probe");
    for (unsigned attempt = 0; attempt < 2400; ++attempt) {
        const uint32_t storage = native_source_storage_boot_state;
        /* GPU039 has a single existing APPS DRV2 consumer. The touch stage
         * reaches 7 only after its synchronous vote and controller setup. */
        if (usb_gated) {
            const unsigned request = __atomic_load_n(&probe_request, __ATOMIC_ACQUIRE);
            if (request != CameraQueued || nezha_touch_stage != 7) {
                IOSleep(50);
                continue;
            }
        }
        if (storage == 4 && nezha27_usb_reset_owner_ready != 0 && journal_token != 0 && !journal_ready) {
            const int error = nezha_camera_journal_begin(journal_token, requested_mode);
            journal_ready = error == 0;
            if (journal_ready) {
                (void)hardware_mark(CameraWaiting, 0, storage, nezha27_usb_reset_owner_ready,
                    (unsigned)nezha_touch_stage);
                printf("NEZHA CAMERA: journal token=%08x ready\n", journal_token);
            } else if (error != ENOENT && error != EROFS) {
                printf("NEZHA CAMERA: journal token=%08x error=%d\n", journal_token, error);
                record_diagnostic(CameraWorkerFailure, (unsigned)error);
                __atomic_store_n(&probe_request, CameraDone, __ATOMIC_RELEASE);
                thread_terminate_self();
                return;
            }
        }
        if (storage == 4 && nezha27_usb_reset_owner_ready != 0 &&
            (journal_token == 0 || journal_ready) && (requested_mode < 3 || nezha_touch_stage == 7)) {
            if (usb_gated) {
                /* Allow the existing USB worker to transmit its queue reply. */
                IOSleep(250);
                __atomic_store_n(&probe_request, CameraRunning, __ATOMIC_RELEASE);
            }
            printf("NEZHA CAMERA: worker ready mode=%u storage=%u usb=%u touch=%d\n",
                requested_mode, storage, nezha27_usb_reset_owner_ready, nezha_touch_stage);
            camera_probe();
            __atomic_store_n(&probe_request, CameraDone, __ATOMIC_RELEASE);
            thread_terminate_self();
            return;
        }
        if (storage == 5 || storage == 6) {
            break;
        }
        IOSleep(50);
    }
    printf("NEZHA CAMERA: waiting for storage/USB/touch timed out state=%u usb=%u touch=%d\n",
           native_source_storage_boot_state, nezha27_usb_reset_owner_ready, nezha_touch_stage);
    record_diagnostic(CameraDependencyTimeout, 1);
    __atomic_store_n(&probe_request, CameraDone, __ATOMIC_RELEASE);
    (void)hardware_mark(CameraDependencyTimeout, 1, native_source_storage_boot_state,
        nezha27_usb_reset_owner_ready, (unsigned)nezha_touch_stage);
    thread_terminate_self();
}

void nezha27_camera_probe_start(void);
void nezha27_camera_probe_start(void)
{
    unsigned mode = 0;
    unsigned stopped = 0;
    if (!PE_parse_boot_argn("nezha-camera", &mode, sizeof(mode)) || mode < 1 || mode > 3 ||
        !__atomic_compare_exchange_n(&requested_mode, &stopped, mode, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        return;
    }
    thread_t thread = THREAD_NULL;
    (void)PE_parse_boot_argn("nezha-cj", &journal_token, sizeof(journal_token));
    (void)PE_parse_boot_argn("nezha-cusb", &usb_gated, sizeof(usb_gated));
    if (usb_gated && (mode != 3 || !journal_token)) {
        printf("NEZHA CAMERA: invalid USB probe configuration\n");
        return;
    }
    /* Retain the same empty/local resource slot and verify it at the early
     * boot point, before the worker is eligible for any hardware access. */
    struct NezhaCameraProfile profile;
    const bool profile_present = nezha_camera_profile_open(nezha_camera_profile_slot,
        (size_t)nezha_camera_profile_slot_size, &profile);
    printf("NEZHA CAMERA: local stock profile=%u\n", (unsigned)profile_present);
    record_diagnostic(CameraWaiting, 0);
    __atomic_store_n(&probe_request, usb_gated ? CameraArmed : CameraDisarmed, __ATOMIC_RELEASE);
    printf("NEZHA CAMERA: USB provider28 registration=%u\n", (unsigned)qcom_usb_diag_register(&camera_provider));
    if (usb_gated) {
        printf("NEZHA CAMERA: armed, no hardware access before USB submit\n");
    }
    const kern_return_t result = kernel_thread_start(camera_probe_thread, NULL, &thread);
    if (result == KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        __atomic_store_n(&probe_request, CameraDone, __ATOMIC_RELEASE);
        record_diagnostic(CameraWorkerFailure, (unsigned)result);
        printf("NEZHA CAMERA: could not start probe thread: %d\n", result);
    }
}
