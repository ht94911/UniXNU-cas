/* SPDX-License-Identifier: BSD-2-Clause */
#include "NbScm.h"
#include <string.h>
static void u32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void u64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
int nb_scm_assignment_page(void *page,uint64_t pa,NbRange region,int to_remote,int shared){
    if(!page || ((uintptr_t)page&4095) || !pa || (pa&4095) || pa>0xfffff000 ||
       !nb_range_valid(region) || ((region.base|region.bytes)&4095) ||
       nb_ranges_overlap((NbRange){pa,4096},region) || (to_remote!=0&&to_remote!=1) || (shared!=0&&shared!=1))return NB_ARGUMENT;
    uint8_t *p=page;memset(p,0,4096);
    unsigned source_count=to_remote||!shared?1:2,dest_count=to_remote&&shared?2:1;
    u32(p,to_remote?3:15);if(source_count==2)u32(p+4,3);
    u64(p+64,region.base);u64(p+72,region.bytes);
    /* qcom_scm_current_perm_info: 24-byte entries, context fields zero. */
    u32(p+128,to_remote?15:3);u32(p+132,6);
    if(dest_count==2){u32(p+152,3);u32(p+156,6);}
    /* More than four SMCCC arguments use the owner's standard x5 array. */
    u64(p+256,source_count*4);u64(p+264,pa+128);u64(p+272,dest_count*24);u64(p+280,0);
    return NB_OK;
}
