/* SPDX-License-Identifier: MIT */
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "NezhaCameraCamcc.h"

/* Stock nezha FDT: canoe-camcc, canoe-cambistmclkcc and CCI0. Map only
 * the pages of the recorded registers. Offsets checked against u0er
 * ed8978ac0087 and Qualcomm's Kaanapali camera clock/CCI source. */
enum {
    CameraClockPage = 0x958e000,
    MainMclkPage = 0x1764000,
    CameraCci0Page = 0x941b000,
    RegisterPageBytes = 0x1000,
    CameraTopGdsc = 0x334,
    CameraTopStatus = 0x338,
    CameraCciCommand = 0x250,
    CameraCciConfig = 0x254,
    CameraCciBranch = 0x268,
    CameraCciVersion = 0,
    CameraCciIrqMask = 0xc04,
    CameraCciIrqStatus = 0xc0c,
    CameraTopPowerUp = 1 << 16,
};
static const uint32_t clock_off = UINT32_C(1) << 31;

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern volatile uint32_t native_source_storage_boot_state;
extern volatile unsigned nezha27_usb_reset_owner_ready;
extern void thread_terminate_self(void);

static unsigned requested_mode;

static vm_offset_t map_registers(vm_offset_t physical)
{
    return ml_io_map_unmappable(physical, RegisterPageBytes, 7 /* VM_WIMG_IO */);
}

static uint32_t read32(vm_offset_t page, uint32_t offset)
{
    const uint32_t value = *(volatile const uint32_t *)(page + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void read_cci(vm_offset_t camcc)
{
    const uint32_t power = read32(camcc, CameraTopStatus);
    const uint32_t branch = read32(camcc, CameraCciBranch);
    const uint32_t root = read32(camcc, CameraCciCommand);
    if ((power & CameraTopPowerUp) == 0 || (branch & (clock_off | 1u)) != 1u ||
        (root & clock_off) != 0) {
        printf("NEZHA CAMERA: CCI0 not clocked top=%#x root=%#x branch=%#x\n",
               power, root, branch);
        return;
    }

    const vm_offset_t cci = map_registers(CameraCci0Page);
    if (cci == 0) {
        printf("NEZHA CAMERA: CCI0 map failed\n");
        return;
    }
    printf("NEZHA CAMERA: CCI0 hw=%#x irq-mask=%#x irq=%#x m1q0=%u m1q1=%u fifo=%u\n",
           read32(cci, CameraCciVersion), read32(cci, CameraCciIrqMask),
           read32(cci, CameraCciIrqStatus), read32(cci, 0x504), read32(cci, 0x604),
           read32(cci, 0x21c));
    ml_io_unmap(cci, RegisterPageBytes);
}

static uint32_t read_camcc(void *cookie, uint32_t offset)
{
    return read32((vm_offset_t)cookie, offset - 0x21000u);
}

static void write_camcc(void *cookie, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)((vm_offset_t)cookie + offset - 0x21000u) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}

static void wait_ms(void *cookie, unsigned milliseconds)
{
    (void)cookie;
    IOSleep(milliseconds);
}

static void camera_probe(void)
{
    const vm_offset_t camcc = map_registers(CameraClockPage);
    if (camcc == 0) {
        printf("NEZHA CAMERA: CAMCC map failed\n");
        return;
    }
    const uint32_t top = read32(camcc, CameraTopStatus);
    printf("NEZHA CAMERA: mode=%u CAMCC gdsc=%#x status=%#x cci0 cmd=%#x cfg=%#x branch=%#x\n",
           requested_mode, read32(camcc, CameraTopGdsc), top,
           read32(camcc, CameraCciCommand), read32(camcc, CameraCciConfig),
           read32(camcc, CameraCciBranch));

    const vm_offset_t mclk = map_registers(MainMclkPage);
    if (mclk != 0) {
        printf("NEZHA CAMERA: MCLK0 cmd=%#x cfg=%#x branch=%#x\n",
               read32(mclk, 0), read32(mclk, 4), read32(mclk, 0x18));
        ml_io_unmap(mclk, RegisterPageBytes);
    }

    if (requested_mode == 2 && top != UINT32_MAX) {
        /* Standalone probe on the GPU039 camera-only derivative. CCI stays
         * idle and no sensor or PMIC write is issued by this mode. */
        const struct NezhaCameraRegisterIo io = {(void *)camcc, read_camcc, write_camcc, wait_ms};
        struct NezhaCameraCamccLease lease = {0};
        const int acquire = nezha_camera_camcc_acquire_top(&io, &lease);
        int rate = NezhaCameraCamccInvalid;
        int gate = NezhaCameraCamccInvalid;
        if (acquire == NezhaCameraCamccOk) {
            rate = nezha_camera_camcc_select_cci0_rate(&io, &lease, 19200000);
            if (rate == NezhaCameraCamccOk) {
                gate = nezha_camera_camcc_enable_cci0(&io, &lease);
                if (gate == NezhaCameraCamccOk) {
                    read_cci(camcc);
                }
            }
        }
        const int restore = nezha_camera_camcc_release(&io, &lease);
        printf("NEZHA CAMERA: clock probe top=%d rate=%d gate=%d restore=%d owned=%#x rate-owned=%u\n",
               acquire, rate, gate, restore, lease.enabled_by_us, (unsigned)lease.cci_rate_owned);
    } else {
        read_cci(camcc);
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
        if (storage == 4 && nezha27_usb_reset_owner_ready != 0) {
            camera_probe();
            thread_terminate_self();
            return;
        }
        if (storage == 5 || storage == 6) {
            break;
        }
        IOSleep(50);
    }
    printf("NEZHA CAMERA: waiting for storage/USB timed out state=%u usb=%u\n",
           native_source_storage_boot_state, nezha27_usb_reset_owner_ready);
    thread_terminate_self();
}

void nezha27_camera_probe_start(void);
void nezha27_camera_probe_start(void)
{
    unsigned mode = 0;
    unsigned stopped = 0;
    if (!PE_parse_boot_argn("nezha-camera", &mode, sizeof(mode)) || mode < 1 || mode > 2 ||
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
