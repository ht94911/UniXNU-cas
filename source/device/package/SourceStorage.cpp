// SPDX-License-Identifier: BSD-2-Clause-Patent
// Start the source-built UFS adapter after the original storage family loads.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#if XNU_QCOM_USB_DEBUG
#include "QcomXnuUsbDebug.h"
#endif

extern kmod_info_t g_kernel_kmod_info;
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_ufs_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
extern "C" IOService *native_ufs_source_create(void);
extern "C" bool native_source_root_policies_load(void);
// NativeUfs records how far root start got and the last soft-failure reason.
extern "C" volatile uint32_t native_ufs_root_step;
extern "C" const char *volatile native_ufs_root_failure;

// NativeUfs polls the optional board diagnostic during window reads. The
// minimal kernel does not include it; a weak definition keeps the archive
// linkable and is replaced when the diagnostic component is built in.
extern "C" __attribute__((weak)) void boot_progress_poll(void) {}
// Starts the sheng-log writer (BootLog.c); the board page calls it when that page is built in.
extern "C" void boot_log_tick(unsigned now);
extern "C" volatile uint32_t boot_log_state, boot_log_rounds, boot_log_errors;
extern "C" const char *volatile boot_log_open_refusal;

// Without the board page nothing shows the writer's state, so a log that cannot be written stops the boot
// with the reason on the panic screen instead of leaving an unattended run without a log.
// Once it writes, a writer whose rounds stop for 20 s (a hang elsewhere that blocks it) also panics: the panic screen's log flush then
// keeps the message buffer up to the hang.
static void boot_log_check(void *, wait_result_t)
{
    unsigned waited = 0;
    for (; waited < 30 && !boot_log_rounds; ++waited) {
        if (boot_log_state == 3) break;
        IOSleep(1000);
    }
    if (!boot_log_rounds)
        panic("SourceStorage: sheng-log has no log (state %u, rounds %u, errors %u, open: %s)", boot_log_state,
              boot_log_rounds, boot_log_errors, boot_log_open_refusal ? boot_log_open_refusal : "no refusal");
    uint32_t last = boot_log_rounds;
    for (unsigned still = 0; boot_log_state == 2; IOSleep(1000)) {
        const uint32_t now = boot_log_rounds;
        still = now == last ? still + 1 : 0;
        last = now;
        if (still >= 20)
            panic("SourceStorage: sheng-log writer stalled for 20 s after %u rounds (errors %u)", now, boot_log_errors);
    }
}

static const OSMetaClass *storageParent;
static IOService *storageDriver;
static bool attempted;

// Used only by the deferred source IOBlockStorageDevice metaclass constructor.
// The C++ class name stays unchanged for compiler-authored arm64e discriminators.
extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!storageParent) panic("SourceStorage: metaclass initialized before original family");
    return storageParent;
}

// The Data seed the loader placed in /chosen/memory-map. Its disk starts after UFS, whose store window
// keeps the changed pages of the seed when the board enables persistence.
static void start_data_disk(IOService *provider)
{
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    const bool seed = map && (map->getProperty("PackedRAMDisk") || map->getProperty("RAMDisk"));
    OSSafeReleaseNULL(map);
    if (!seed) return;
    IOService *disk = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NativeRamBlock"));
    if (!disk) panic("SourceStorage: cannot allocate the Data disk");
    const char *step = !disk->init() ? "init" : !disk->attach(provider) ? "attach" : !disk->start(provider) ? "start" : nullptr;
    if (step) panic("SourceStorage: Data disk %s failed", step);
    printf("SourceStorage: Data disk started\n");
}

static void source_storage_start_body(void)
{
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess)
        panic("SourceStorage: IOStorageFamily load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("SourceStorage: original base layout %u differs from source %lu",
              storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));

    // Authenticate policies before registering hardware-facing classes. The
    // firmware hands off the envelope; the trusted anchor is compiled in.
    if (!native_source_root_policies_load())
        panic("SourceStorage: root policy handoff or authentication failed");
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess)
        panic("SourceStorage: APFS load failed %x", status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("SourceStorage: cannot begin class registration");
    source_storage_base_init();
    source_storage_ufs_init();
    source_storage_ram_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess)
        panic("SourceStorage: class registration failed %x", status);

    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (!provider) panic("SourceStorage: platform root unavailable");
    storageDriver = native_ufs_source_create();
    if (!storageDriver) panic("SourceStorage: cannot allocate UFS service");
    const char *step = !storageDriver->init() ? "init" : !storageDriver->attach(provider) ? "attach" :
                       !storageDriver->start(provider) ? "start" : nullptr;
    if (step)
        panic("SourceStorage: UFS %s failed (root step %u: %s)", step, native_ufs_root_step,
              native_ufs_root_failure ? native_ufs_root_failure : "no reason recorded");
    printf("SourceStorage: UFS started through public IOStorageFamily interface\n");
    start_data_disk(provider);
    // The UFS log area exists from here on; the writer only runs with sheng-log=1.
    boot_log_tick(0);
    unsigned log = 0;
    thread_t check;
    if (PE_parse_boot_argn("sheng-log", &log, sizeof(log)) && log == 1 &&
        kernel_thread_start(boot_log_check, nullptr, &check) == KERN_SUCCESS)
        thread_deallocate(check);
}

// NEZHA_USB_ROOT_ORDER_BEGIN
// The standard hook runs after vfsinit, before bsd_autoconf starts the USB
// worker. When explicitly requested, queue this normal-thread work so BSD can
// reach the existing USB worker first. Never initialize USB in a console/IRQ
// or create a second USB worker, and never touch UFS on a failed USB gate.
extern "C" volatile uint32_t native_source_storage_boot_state;
volatile uint32_t native_source_storage_boot_state; // 1 queued, 2 wait USB, 3 UFS, 4 done, 5 thread error, 6 USB unavailable
static void source_storage_after_usb(void *, wait_result_t)
{
#if XNU_QCOM_USB_DEBUG
    unsigned waited = 0;
    qcom_usb_debug_status status = {};
    native_source_storage_boot_state = 2;
    for (;;) {
        qcom_usb_debug_get_status(&status);
        if (status.phase == QCOM_USB_DEBUG_ACTIVE) break;
        if ((status.phase != QCOM_USB_DEBUG_NOT_INITIALIZED && status.phase != QCOM_USB_DEBUG_PREPARING) || waited == 9000) {
            printf("SourceStorage: USB unavailable phase=%u error=%u; root transport not attempted\n", status.phase, status.error);
            native_source_storage_boot_state = 6;
            thread_terminate_self();
        }
        IOSleep(10);
        ++waited;
    }
    // Let the existing worker finish publishing control/log state after init.
    IOSleep(10);
    printf("SourceStorage: existing USB ready; begin independent read-only root runtime\n");
#else
    native_source_storage_boot_state = 6;
    printf("SourceStorage: USB-required root debug selected without USB; root transport not attempted\n");
    thread_terminate_self();
#endif
    native_source_storage_boot_state = 3;
    source_storage_start_body();
    native_source_storage_boot_state = 4;
    thread_terminate_self();
}

extern "C" void native_source_storage_start(void)
{
    unsigned enabled = 0, require_usb = 0;
    if (!PE_parse_boot_argn("native-source-storage", &enabled, sizeof(enabled)) || enabled != 1 ||
        __atomic_exchange_n(&attempted, true, __ATOMIC_ACQ_REL)) return;
    PE_parse_boot_argn("native-source-storage-require-usb", &require_usb, sizeof(require_usb));
    if (require_usb != 1) {
        source_storage_start_body();
        return;
    }
    native_source_storage_boot_state = 1;
    thread_t thread = THREAD_NULL;
    kern_return_t result = kernel_thread_start(source_storage_after_usb, nullptr, &thread);
    if (result != KERN_SUCCESS) {
        native_source_storage_boot_state = 5;
        printf("SourceStorage: root worker could not start kr=%d; root transport not attempted\n", result);
        return;
    }
    thread_deallocate(thread);
}
// NEZHA_USB_ROOT_ORDER_END
