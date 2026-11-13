/* SPDX-License-Identifier: BSD-2-Clause */
#include "../../package/baseband27/platform/NbIpaSmem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(4096) uint8_t data[0x200000], original[0x200000];
static void word(unsigned offset, uint32_t value) { memcpy(data+offset,&value,4); }

int main(void)
{
    QcomSmem smem={data,sizeof data};QcomSmemPartition partition={0x10000,0x40000,64,0,1};
    const unsigned part=partition.offset;
    word(part,0x54525024);word(part+4,1u<<16);word(part+8,partition.size);
    word(part+12,32+16+NB_IPA_SMEM_BYTES);word(part+16,partition.size);
    word(part+32,0xa5a5u|(NB_SMEM_IPA_FILTER_TABLE<<16));
    word(part+36,NB_IPA_SMEM_BYTES);
    NbRange region={0,0};uint32_t item=0;
    memcpy(original,data,sizeof data);
    assert(!nb_ipa_smem_region(&smem,&partition,0x81d00000,&region,&item));
    assert(item==0x81d10030 && region.base==0x81d10000 && region.bytes==0x1a000);
    assert(!memcmp(data,original,sizeof data));
    word(part+36,NB_IPA_SMEM_BYTES-8);
    assert(nb_ipa_smem_region(&smem,&partition,0x81d00000,&region,&item)==NB_OWNERSHIP);
    word(part+36,NB_IPA_SMEM_BYTES);
    word(part+32,0xa5a5u|(496u<<16));
    assert(nb_ipa_smem_region(&smem,&partition,0x81d00000,&region,&item)==NB_MISSING);
    word(part+12,32);word(part+16,partition.size);
    NbMss m={0};m.smem=smem;m.partition=partition;
    memcpy(original,data,sizeof data);
    assert(nb_ipa_smem_prepare(&m,0x81d00000,&region,&item)==NB_OWNERSHIP);
    assert(!memcmp(data,original,sizeof data));
    m.stage=NbMssCheck;m.masters_stopped=1;m.initial_stop_mask=3;
    m.initial_stop[1]=NB_IO;
    assert(nb_ipa_smem_prepare(&m,0x81d00000,&region,&item)==NB_OWNERSHIP);
    assert(!memcmp(data,original,sizeof data));
    m.initial_stop[1]=0;
    assert(!nb_ipa_smem_prepare(&m,0x81d00000,&region,&item));
    assert(item==0x81d10030 && region.base==0x81d10000 && region.bytes==0x1a000);
    memcpy(original,data,sizeof data);
    assert(!nb_ipa_smem_prepare(&m,0x81d00000,&region,&item));
    assert(!memcmp(data,original,sizeof data));
    word(part+32,0);
    assert(nb_ipa_smem_region(&smem,&partition,0x81d00000,&region,&item)==NB_WIRE);
    assert(nb_ipa_smem_region(&smem,&partition,UINT64_MAX-4095,&region,&item)==NB_ARGUMENT);
    assert(nb_ipa_smem_region(&smem,&partition,0xfffff000,&region,&item)==NB_ARGUMENT);
    puts("PASS IPA SMEM: no allocation before two stopped images; missing item allocated once; existing unaligned range reused, corruption/size rejected");
}
