// SPDX-License-Identifier: BSD-2-Clause-Patent
// The original D16/24A446 product node has supports-3rd-party-haptics=1 and
// closed-loop=1. Only publish with a visible Actuator plugin and its own
// original D16 preset. No J120/iPad identity, private vtable or binary patch.
#include "NezhaHaptics.h"
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
extern "C" int nezha_haptics_runtime_ready(void);
extern "C" int nezha_haptics_ios_commit(int plugin_visible,int preset_visible) {
    static bool committed;
    uint32_t stage=0;
    if(committed)return 0;
    if(!PE_parse_boot_argn("nezha-haptics-ios",&stage,sizeof stage) || !stage)return 0;
    if(stage>2 || !plugin_visible || !nezha_haptics_runtime_ready() || (stage==2 && !preset_visible))return ENXIO;
    IORegistryEntry *product=IORegistryEntry::fromPath("/product",gIODTPlane);
    if(!product)return ENODEV;
    IORegistryEntry *node=product->childFromPath("haptics",gIODTPlane);
    if(node){node->release();product->release();return EEXIST;}
    OSDictionary *properties=OSDictionary::withCapacity(4);
    if(!properties){product->release();return ENOMEM;}
    const uint32_t value=1;static const char name[]="haptics";
    OSData *third=OSData::withBytes(&value,sizeof value),*closed=OSData::withBytes(&value,sizeof value);
    OSData *nameData=OSData::withBytes(name,sizeof name);
    bool ok=third && closed && nameData && properties->setObject("name",nameData) &&
        properties->setObject("supports-3rd-party-haptics",third);
    if(ok && stage==2)ok=properties->setObject("closed-loop",closed);
    if(third)third->release();
    if(closed)closed->release();
    if(nameData)nameData->release();
    node=ok?new IORegistryEntry:nullptr;
    if(node && !node->init(properties)){node->release();node=nullptr;}
    properties->release();
    if(!node){product->release();return ENOMEM;}
    // MobileGestalt's DeviceSupportsHaptics enumerates the device-tree plane
    // but compares IORegistryEntryGetName, which reads the default name.
    // Match IODTImportEntry: publish both names before attaching the node.
    node->setName(name);node->setName(name,gIODTPlane);ok=node->attachToParent(product,gIODTPlane);
    if(ok)IOLog("NEZHA HAPTICS: iOS product node stage=%u Actuator=%d D16preset=%d name=%s dt-name=%s\n",
        stage,plugin_visible,preset_visible,node->getName(),node->getName(gIODTPlane));
    node->release();product->release();if(!ok)return EIO;
    committed=true;
    return 0;
}
