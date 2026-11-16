// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included in selected NativeUfs.cpp after loadExtentProperties. Uses its
// existing maps, GPT geometry, healthy host and serialized READ(10) path.
static UfsExtentMap nbUfsMap = {};
static int nbUfsOpened;
static uint8_t nbUfsDigest[32];
extern "C" int nezha_baseband_enabled(void);
extern "C" int native_ufs_bb_open(uint64_t *blocks,uint8_t digest[32])
{
    if (!blocks || !digest || !nezha_baseband_enabled()) return -1;
    if (!gHost.ready || gHost.poisoned || gHostPanic || !gStoreGeometry.valid ||
        !gExtentMap.bound || !gDataExtentMap.bound || !gProtectedExtentMap.bound) return -1;
    if (!nbUfsOpened) {
        nbUfsOpened = -1;
        const char *why = nullptr;
        if (loadExtentProperties(nbUfsMap,"nezha-baseband-extents","nezha-baseband-blocks",&why) != 1 ||
            ufsExtentBind(nbUfsMap,gStoreGeometry.first,gStoreGeometry.last) ||
            !ufsExtentDisjoint(nbUfsMap,gExtentMap) || !ufsExtentDisjoint(nbUfsMap,gDataExtentMap) ||
            !ufsExtentDisjoint(nbUfsMap,gProtectedExtentMap)) return -1;
        IORegistryEntry *chosen = IORegistryEntry::fromPath("IODeviceTree:/chosen");
        if (!chosen) return -1;
        OSData *sha = OSDynamicCast(OSData,chosen->getProperty("nezha-baseband-sha256"));
        bool valid = sha && sha->getLength() == sizeof nbUfsDigest;
        if (valid) memcpy(nbUfsDigest,sha->getBytesNoCopy(),sizeof nbUfsDigest);
        chosen->release();
        if (!valid || !nbUfsMap.fileBlocks || nbUfsMap.fileBlocks > UINT64_MAX/4096) return -1;
        nbUfsOpened = 1;
    }
    if (nbUfsOpened != 1) return -1;
    *blocks = nbUfsMap.fileBlocks;memcpy(digest,nbUfsDigest,sizeof nbUfsDigest);return 0;
}
extern "C" int native_ufs_bb_read(uint64_t rel,uint32_t blocks,void *dst)
{
    if (nbUfsOpened != 1 || !dst || !blocks || blocks > kDataMax/4096 ||
        rel >= nbUfsMap.fileBlocks || blocks > nbUfsMap.fileBlocks-rel ||
        !gHost.ready || gHost.poisoned || gHostPanic) return -1;
    return ufsExtentRead(nbUfsMap,0,nbUfsMap.partFirst+rel,blocks,static_cast<uint8_t *>(dst),
        [](uint64_t l,uint32_t n,uint8_t *d) {return hostRead4kRaw(0,l,n,d);}) == kIOReturnSuccess ? 0:-1;
}
