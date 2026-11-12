/* SPDX-License-Identifier: MIT */
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "NezhaCameraCamcc.h"
#include "NezhaCameraOvx10500u.h"
#include "NezhaCameraRpmh.h"

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
    if ((power & CameraTopPowerUp) == 0 || (branch & (clock_off | 1u)) != 1u ||
        (root & clock_off) != 0) {
        printf("NEZHA CAMERA: CCI0 not clocked top=%#x root=%#x branch=%#x\n",
               power, root, branch);
        return 0;
    }

    const vm_offset_t cci = map_registers(CameraCci0Page, RegisterPageBytes);
    if (cci == 0) {
        printf("NEZHA CAMERA: CCI0 map failed\n");
        return 0;
    }
    printf("NEZHA CAMERA: CCI0 hw=%#x irq-mask=%#x irq=%#x m1q0=%u m1q1=%u fifo=%u\n",
           read32(cci, CameraCciVersion), read32(cci, CameraCciIrqMask),
           read32(cci, CameraCciIrqStatus), read32(cci, 0x504), read32(cci, 0x604),
           read32(cci, 0x21c));
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

static bool read_main_identity(vm_offset_t cci, unsigned cci_hz)
{
    const struct NezhaCameraRegisterIo bus = {(void *)cci, read_page, write_page, wait_ms};
    const uint32_t mask = read32(cci, CameraCciIrqMask);
    uint32_t timing[5];
    if (mask == UINT32_MAX) {
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
    printf("NEZHA CAMERA: main CCI0 preparation=%d phase=%d irq=%#x fifo=%u\n",
           prepared, prep.phase, prep.irq, prep.fifo_words);
    if (prepared != NezhaCciOk) {
        write_page((void *)cci, CameraCciIrqMask, mask);
        return false;
    }

    struct NezhaCameraOvxIdentity id;
    const enum NezhaCameraCciStatus status = nezha_camera_ovx10500u_read_identity(&bus, &id);
    const unsigned word = (unsigned)id.bytes[0] << 8 | id.bytes[1];
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
        printf("NEZHA CAMERA: main probe prerequisites missing CCI0=%#x MCLK0=%u\n",
               version, (unsigned)(mclk != 0));
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
            const struct NezhaCameraMainRailIo rails = {&rpmh, nezha_camera_rpmh_acquire, nezha_camera_rpmh_release};
            struct NezhaCameraMainPins pins = {0};
            const bool held = nezha_camera_main_hold_reset(&gpio, &pins);
            const bool powered = held && nezha_camera_main_start_pins(&gpio, &clock, &rails, &pins);
            printf("NEZHA CAMERA: main power held=%u on=%u votes=%#x rpmh=%d pending=%u\n",
                   (unsigned)held, (unsigned)powered, pins.rails_requested, rpmh.status, (unsigned)rpmh.unsettled);
            const bool quiet = !powered || read_main_identity(cci, cci_hz);
            released = quiet && (!pins.reset_held || nezha_camera_main_restore_pins(&gpio, &clock, &rails, &pins));
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
            printf("NEZHA CAMERA: main RPMh configuration or resources did not match\n");
        }
    } else {
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
    const vm_offset_t camcc = map_registers(CameraClockPage, RegisterPageBytes);
    if (camcc == 0) {
        printf("NEZHA CAMERA: CAMCC map failed\n");
        return;
    }
    const uint32_t top = read32(camcc, CameraTopStatus);
    printf("NEZHA CAMERA: mode=%u CAMCC gdsc=%#x status=%#x cci0 cmd=%#x cfg=%#x branch=%#x\n",
           requested_mode, read32(camcc, CameraTopGdsc), top,
           read32(camcc, CameraCciCommand), read32(camcc, CameraCciConfig),
           read32(camcc, CameraCciBranch));

    const vm_offset_t mclk = map_registers(MainMclkPage, RegisterPageBytes);
    if (mclk != 0) {
        printf("NEZHA CAMERA: MCLK0 cmd=%#x cfg=%#x branch=%#x\n",
               read32(mclk, 0), read32(mclk, 4), read32(mclk, 0x18));
    }

    if (requested_mode >= 2 && top != UINT32_MAX) {
        const struct NezhaCameraRegisterIo io = {(void *)(camcc - 0x21000u), read_page, write_page, wait_ms};
        struct NezhaCameraCamccLease lease = {0};
        const int acquire = nezha_camera_camcc_acquire_top(&io, &lease);
        printf("NEZHA CAMERA: top result=%d gdsc=%#x status=%#x core=%#x cpas=%#x owned=%#x\n",
               acquire, read32(camcc, CameraTopGdsc), read32(camcc, CameraTopStatus),
               read32(camcc, 0x348), read32(camcc, 0x37c), lease.enabled_by_us);
        int rate = NezhaCameraCamccInvalid;
        int gate = NezhaCameraCamccInvalid;
        bool quiet = true;
        if (acquire == NezhaCameraCamccOk) {
            rate = nezha_camera_camcc_select_cci0_rate(&io, &lease, 19200000);
            printf("NEZHA CAMERA: CCI0 rate result=%d cmd=%#x cfg=%#x branch=%#x\n",
                   rate, read32(camcc, CameraCciCommand), read32(camcc, CameraCciConfig),
                   read32(camcc, CameraCciBranch));
            if (rate == NezhaCameraCamccOk) {
                gate = nezha_camera_camcc_enable_cci0(&io, &lease);
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
        const int restore = quiet ? nezha_camera_camcc_release(&io, &lease) : NezhaCameraCamccBusy;
        printf("NEZHA CAMERA: clock probe top=%d rate=%d gate=%d restore=%d owned=%#x rate-owned=%u\n",
               acquire, rate, gate, restore, lease.enabled_by_us, (unsigned)lease.cci_rate_owned);
    } else {
        const vm_offset_t cci = map_clocked_cci(camcc);
        if (cci != 0) {
            ml_io_unmap(cci, RegisterPageBytes);
        }
    }
    if (mclk != 0) {
        ml_io_unmap(mclk, RegisterPageBytes);
    }
    ml_io_unmap(camcc, RegisterPageBytes);
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
        if (storage == 4 && nezha27_usb_reset_owner_ready != 0 &&
            (requested_mode < 3 || nezha_touch_stage == 7)) {
            camera_probe();
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
    const kern_return_t result = kernel_thread_start(camera_probe_thread, NULL, &thread);
    if (result == KERN_SUCCESS) {
        thread_deallocate(thread);
    } else {
        printf("NEZHA CAMERA: could not start probe thread: %d\n", result);
    }
}
