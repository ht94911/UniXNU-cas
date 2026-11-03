// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Original17 uses the kernel event driver. Actual24A446 also attaches the
// DriverKit AppleUserHIDEventService kernel proxy to our owned IOHIDInterface. Never
// search outside the owned device subtree or treat an interface as a consumer.
static unsigned nezha_hid_consumer_kind(IORegistryEntry *entry) {
 if(!entry)return 0;
 if(entry->metaCast("IOHIDEventDriver"))return 1;
 if(entry->metaCast("AppleUserHIDEventService"))return 2;
 return 0;
}
