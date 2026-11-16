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
    const unsigned offset=item-0x81d00000;
    memset(data+offset,0x6b,NB_IPA_SMEM_BYTES);
    memcpy(original,data,sizeof data);
    m.masters_stopped=0;
    assert(nb_ipa_smem_stats_init(&m,0x81d00000,item)==NB_OWNERSHIP);
    assert(!memcmp(data,original,sizeof data));
    m.masters_stopped=1;m.initial_stop[0]=NB_IO;
    assert(nb_ipa_smem_stats_init(&m,0x81d00000,item)==NB_OWNERSHIP);
    assert(!memcmp(data,original,sizeof data));
    m.initial_stop[0]=0;
    assert(nb_ipa_smem_stats_init(&m,0x81d00000,item+4)==NB_OWNERSHIP);
    assert(!memcmp(data,original,sizeof data));
    assert(!nb_ipa_smem_stats_init(&m,0x81d00000,item));
    const unsigned ids[]={0,1,2,7,8,9,10,11,12,13,17,19};
    assert(qcom_smem_read32(&smem,offset+4)==0x6b6b000c);
    for(unsigned i=0;i<12;i++){
        assert(qcom_smem_read32(&smem,offset+8+8*i)==((4u<<16)|ids[i]));
        assert(qcom_smem_read32(&smem,offset+12+8*i)==0);
    }
    assert(!memcmp(data,original,offset+4));
    assert(!memcmp(data+offset+6,original+offset+6,2));
    assert(!memcmp(data+offset+104,original+offset+104,sizeof data-offset-104));
    word(part+36,NB_IPA_SMEM_BYTES-8);
    memcpy(original,data,sizeof data);
    assert(nb_ipa_smem_stats_init(&m,0x81d00000,item)==NB_OWNERSHIP);
    assert(!memcmp(data,original,sizeof data));
    word(part+36,NB_IPA_SMEM_BYTES);
    word(part+32,0);
    assert(nb_ipa_smem_region(&smem,&partition,0x81d00000,&region,&item)==NB_WIRE);
    assert(nb_ipa_smem_region(&smem,&partition,UINT64_MAX-4095,&region,&item)==NB_ARGUMENT);
    assert(nb_ipa_smem_region(&smem,&partition,0xfffff000,&region,&item)==NB_ARGUMENT);
    puts("PASS IPA SMEM: two-stop ownership, allocation and identity range; stock twelve MSM statistics records only, canary/reserved/filter tail preserved");
}
