// SPDX-License-Identifier: BSD-2-Clause-Patent
// Original24A446 AppleARMBacklight ABI:169 authenticated slots,0x370 bytes.
// Fixture was checked against actual original instructions AND real registered
// QEMU metaclass. This declaration has not yet passed an Apple compiler check.
// No hardware functions are called and no backlight service is published here.
#pragma once
#include <IOKit/IOService.h>
#include <stdint.h>
class AppleARMBacklight : public IOService {
    OSDeclareAbstractStructors(AppleARMBacklight)
    unsigned char originalPrivate[0x370-sizeof(IOService)];
public:
    void free() override;
    bool init(OSDictionary *properties=nullptr) override;
    IOReturn setProperties(OSObject *) override;
    IOReturn configureReport(IOReportChannelList *,uint32_t,void *,void *) override;
    IOReturn updateReport(IOReportChannelList *,uint32_t,void *,void *) override;
    bool start(IOService *) override;
    IOWorkLoop *getWorkLoop() const override;
    IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) override;
    IOReturn message(uint32_t,IOService *,void *) override;
    IOReturn setPowerState(unsigned long,IOService *) override;
    // Unknown original signature. Retain the original authenticated entry and
    // forbid source callers,as in the upstream opaque Legacy display binding.
    virtual __attribute__((deprecated("Unclassified original ABI;never call from source"))) void opaque168_56708();
};
static_assert(sizeof(IOService)==0x88,"selected27 native IOService layout");
static_assert(sizeof(AppleARMBacklight)==0x370,"actual original AppleARMBacklight layout");
