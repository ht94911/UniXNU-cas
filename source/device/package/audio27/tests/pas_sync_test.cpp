// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual native sync router with fake cache/barrier calls. No hardware access.
#include "../os/NezhaAudioAdspInputs.h"
#include <cassert>
static unsigned cleans,barriers;
extern "C" int na_native_clean(void *,const void *,uint64_t,uint64_t) {++cleans;return NA_OK;}
extern "C" void na_native_publish_writes(void) {++barriers;}
int main() {
    NaNativeAdspInputs s={};s.lease=42;
    alignas(4096) uint8_t metadata[4096],mapped[8192];
    s.metadata[0]={reinterpret_cast<void *>(1),metadata,0x880100000,4096};
    s.region[1]={reinterpret_cast<void *>(1),reinterpret_cast<void *>(2),mapped,na_nezha_facts.dtb,42};
    assert(!na_native_pas_sync(&s,metadata+32,s.metadata[0].physical+32,64));
    assert(cleans==1 && !barriers);
    assert(!na_native_pas_sync(&s,mapped+32,na_nezha_facts.dtb.base+32,64));
    assert(cleans==1 && barriers==1);
    // A matching PA does not authorize another CPU alias or an expired lease.
    assert(na_native_pas_sync(&s,metadata,na_nezha_facts.dtb.base,64)==NA_OWNERSHIP);
    assert(na_native_pas_sync(&s,mapped,na_nezha_facts.dtb.base+32,64)==NA_OWNERSHIP);
    s.region[1].lease=43;assert(na_native_pas_sync(&s,mapped,na_nezha_facts.dtb.base,64)==NA_OWNERSHIP);
    s.region[1].lease=42;
    assert(na_native_pas_sync(&s,mapped,na_nezha_facts.dtb.base,na_nezha_facts.dtb.bytes+1)==NA_OWNERSHIP);
    s.region[1].physical=na_nezha_facts.firmware;
    assert(na_native_pas_sync(&s,mapped,na_nezha_facts.firmware.base,64)==NA_OWNERSHIP);
    assert(na_native_pas_sync(&s,metadata,s.metadata[0].physical+4090,64)==NA_OWNERSHIP);
    assert(na_native_pas_sync(&s,mapped,0xfffffffffffffff0ULL,64)==NA_ARGUMENT);
    assert(cleans==1 && barriers==1);
}
