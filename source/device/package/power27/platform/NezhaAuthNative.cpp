// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaAuth.h"
#include <corecrypto/cchmac.h>
#include <corecrypto/ccsha2.h>
#include <pexpert/device_tree.h>
#include <sys/random.h>
#include <sys/errno.h>
#include <string.h>

extern "C" int np_auth_native_material(NpAuthMaterial *out) {
    if(!out)return EINVAL;
    DTEntry chosen=nullptr;
    void const *data=nullptr;unsigned int bytes=0;
    if(SecureDTLookupEntry(nullptr,"/chosen",&chosen)!=kSuccess||
       SecureDTGetProperty(chosen,"nezha-power-auth-material",&data,&bytes)!=kSuccess||
       !np_auth_material_valid(data,bytes))return ENOENT;
    memcpy(out,data,sizeof(*out));
    return 0;
}
extern "C" int np_auth_native_random(void *,uint8_t *out,size_t bytes) {
    if(!out||(bytes!=1&&bytes!=16&&bytes!=32))return EINVAL;
    read_random(out,static_cast<u_int>(bytes));
    return 0;
}
extern "C" int np_auth_native_hmac(void *,const uint8_t *key,size_t key_bytes,
    const uint8_t *data,size_t data_bytes,uint8_t mac[32]) {
    if(!key||!data||!mac||key_bytes!=32||(data_bytes!=20&&data_bytes!=32))return EINVAL;
    cchmac(ccsha256_di(),key_bytes,key,data_bytes,data,mac);
    return 0;
}
