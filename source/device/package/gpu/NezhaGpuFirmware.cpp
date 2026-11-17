/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaGpuFirmware.h"
#include "NativeAdrenoSha256.h"
#include <string.h>

static uint64_t little(const uint8_t *p, unsigned count)
{
    uint64_t value=0;
    for(unsigned i=0;i<count;++i)value|=uint64_t(p[i])<<(i*8);
    return value;
}
extern "C" int nezha_gpu_firmware_validate(const uint8_t *pack,size_t capacity,NezhaGpuFirmwareView *out)
{
    if(!out)return NezhaGpuFirmwareHeader;
    *out={};
    if(!pack || capacity!=NEZHA_GPU_FW_CAPACITY || memcmp(pack,"PANDFW01",8))return NezhaGpuFirmwareMissing;
    if(little(pack+8,4)!=3 || little(pack+12,4) ||
        memcmp(pack+NEZHA_GPU_FW_MANIFEST,"NGFW001",8) ||
        little(pack+520,4)!=1 || little(pack+524,4))return NezhaGpuFirmwareHeader;
    const uint64_t used=little(pack+528,8);
    if(used<4096 || used>capacity || (used&4095))return NezhaGpuFirmwareRange;
    const char *const names[]={"gen80200_gmu.bin","gen80200_sqe.fw","gen80200_zap.mbn"};
    NezhaGpuFirmwareView view{};
    uint64_t next=4096;
    for(unsigned i=0;i<3;++i) {
        const uint8_t *entry=pack+16+i*64;
        const size_t name_bytes=strlen(names[i]);
        if(memcmp(entry,names[i],name_bytes+1))return NezhaGpuFirmwareHeader;
        for(size_t j=name_bytes+1;j<48;++j)if(entry[j])return NezhaGpuFirmwareHeader;
        const uint64_t offset=little(entry+48,8),bytes=little(entry+56,8);
        if(offset!=next || bytes<8 || offset>used || bytes>used-offset)return NezhaGpuFirmwareRange;
        next=offset+((bytes+4095)&~uint64_t(4095));
        if(next>used)return NezhaGpuFirmwareRange;
        uint8_t digest[32]{};
        NativeAdrenoSha256::hash(pack+offset,size_t(bytes),digest);
        if(memcmp(digest,pack+NEZHA_GPU_FW_HASHES+i*32,32))return NezhaGpuFirmwareDigest;
        view.data[i]=pack+offset;view.bytes[i]=size_t(bytes);
    }
    if(next!=used)return NezhaGpuFirmwareRange;
    *out=view;
    return NezhaGpuFirmwareOK;
}
