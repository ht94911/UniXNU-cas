// SPDX-License-Identifier: MIT
// Adapted from 57UU/UniXNU-sirius f269452c36eceb7f91446fccc4c3bed175aaa98e,
// package/SiriusPmgr.cpp (source SHA256: 9cd370f326af9a133d6489e10926badcf2d25d03cd7c81a3563d0ba3d948e3db).
// Publish the real donor ApplePMGRNub through its OSMetaClass allocation.
// iPhone14,4 / 24A446 AMFI matches this class then invokes its read/OR(4)/write.
// The donor nub helpers load its private provider at +0x88; absent provider
// reads return 0xffffffff and writes return without touching Apple MMIO.
// No Apple power controller, latch readback, CPU power management or security
// policy implementation is claimed. No object sizes/vtables are fabricated.
// The existing XNU_SHENG_PMGR hook invokes this before AMFI configuration.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

extern "C" void sheng_pmgr_run(void);
extern "C" void nezha_wifi_register(void);
extern "C" void nezha_bluetooth_register(void);

// Bounded bring-up state: 0 disabled, 1 class absent, 2 allocation/init failed,
// 3 published and discovered, 4 discovery mismatch.
volatile uint32_t nezha_ios27_pmgr_step;
volatile uint32_t nezha_ios27_pmgr_class_size;
volatile uint64_t nezha_ios27_pmgr_nub;

static const OSMetaClass *nubMetaClass(void)
{
  const OSSymbol *name = OSSymbol::withCString("ApplePMGRNub");
  if (name == nullptr) {
    return nullptr;
  }
  const OSMetaClass *meta = OSMetaClass::getMetaClassWithName(name);
  name->release();
  return meta;
}

extern "C" void sheng_pmgr_run(void)
{
  // Both board adapters defer hardware work until XNU networking is ready.
  nezha_wifi_register();
  nezha_bluetooth_register();
  unsigned mode = 0;
  if (!PE_parse_boot_argn("sheng-pmgr", &mode, sizeof(mode)) || (mode == 0)) {
    return;
  }
  nezha_ios27_pmgr_step = 1;
  const OSMetaClass *meta = nubMetaClass();
  if (meta == nullptr) {
    // The prelinked kext may not have registered its classes yet at this point; load it explicitly
    // (a no-op when it is already loaded) and look the class up again.
    OSKext::loadKextWithIdentifier("com.apple.driver.ApplePMGR");
    meta = nubMetaClass();
  }
  if (meta == nullptr) {
    IOLog("nezha-ios27-pmgr: ApplePMGRNub class absent\n");
    return;
  }
  nezha_ios27_pmgr_class_size = meta->getClassSize();
  IOService *nub = static_cast<IOService *>(meta->alloc());
  if (nub == nullptr) {
    nezha_ios27_pmgr_step = 2;
    IOLog("nezha-ios27-pmgr: ApplePMGRNub allocation failed\n");
    return;
  }
  if (!nub->init()) {
    nezha_ios27_pmgr_step = 2;
    IOLog("nezha-ios27-pmgr: ApplePMGRNub init failed\n");
    nub->release();
    return;
  }
  IOService *platform = IOService::getPlatform();
  if ((platform != nullptr) && !nub->attach(platform)) {
    nezha_ios27_pmgr_step = 2;
    IOLog("nezha-ios27-pmgr: ApplePMGRNub attach failed\n");
    nub->release();
    return;
  }
  // The donor-specific PMGR provider remains absent; IOService attachment
  // above is only the registry/service-plane parent.
  nub->registerService(kIOServiceSynchronous);
  nezha_ios27_pmgr_nub = reinterpret_cast<uint64_t>(nub);
  nezha_ios27_pmgr_step = 3;
  IOService *found = nullptr;
  OSDictionary *matching = IOService::serviceMatching("ApplePMGRNub");
  if (matching != nullptr) {
    // OSPtr is the raw pointer in this configuration; waitForMatchingService returns it retained.
    found = IOService::waitForMatchingService(matching, 1000000);
    matching->release();
  }
  nezha_ios27_pmgr_step = (found == nub) ? 3 : 4;
  IOLog("nezha-ios27-pmgr: ApplePMGRNub size=%u nub=%p found=%p\n", nezha_ios27_pmgr_class_size, nub, found);
  if (found != nullptr) {
    found->release();
  }
}
