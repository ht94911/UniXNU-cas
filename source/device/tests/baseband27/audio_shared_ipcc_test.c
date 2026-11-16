/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Existing audio102 owner and MSS share one dispatcher after either starts. */
#include "../../package/audio27/platform/AudioResourceOwner.h"
#include "../../package/baseband27/platform/NbPower.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned held, audio_calls, mss_calls, clears;
static uint32_t next = UINT32_MAX;
static int lock(void *p) {(void)p; assert(!held); held=1; return 0;}
static void unlock(void *p) {(void)p; assert(held); held=0;}
static int rd(void *p,unsigned domain,uint32_t offset,uint32_t *value)
{(void)p; assert(domain==Sm8850Ipcc && offset==0x10); *value=next; return 0;}
static int wr(void *p,unsigned domain,uint32_t offset,uint32_t value)
{(void)p; assert(domain==Sm8850Ipcc); if(offset==0x1c){assert(value==next);next=UINT32_MAX;clears++;} return 0;}
static int rd64(void *p,unsigned d,uint32_t at,uint64_t *v)
{(void)p;(void)d;(void)at;*v=0;return 0;}
static int fence(void *p) {(void)p;return 0;}
static void aop(void *p,uint32_t id) {(void)p;assert(!held&&id==0);}
static void mss(void *p,uint32_t id) {(void)p;assert(!held&&id==0x20002);mss_calls++;}
/* Hardware SMEM/DSP is outside this test. The real owner route callback
 * reaches this existing publication interface with no configuration lock. */
void na_adsp_smp2p_event(NaAdsp *d) {assert(!held&&d);audio_calls++;}

int main(void)
{
    for(unsigned audio_first=0;audio_first<2;audio_first++){
        Sm8850Resources common={0};NaResourceOwner owner={0};NaAdsp dsp={0};
        Sm8850ResourceIo io={0,lock,unlock,rd,wr,rd64,fence};
        held=audio_calls=mss_calls=clears=0;next=UINT32_MAX;
        assert(!sm8850_resources_init(&common,&io));
        assert(!sm8850_ipcc_register(&common,0,aop,0));
        assert(!na_resource_owner_init(&owner,&common));
        if(!audio_first)assert(!sm8850_ipcc_register(&common,0x20002,mss,0));
        next=0;assert(!sm8850_ipcc_dispatch(&common,32)&&common.ipcc_sealed);
        assert(!na_resource_owner_ipcc(&owner,&dsp));
        if(audio_first)assert(!sm8850_ipcc_register(&common,0x20002,mss,0));
        assert(common.ipcc_count==4); /* originalAOP0, ADSP0/2 and MSS2 */
        next=0x30002;assert(!sm8850_ipcc_dispatch(&common,32));
        next=0x20002;assert(!sm8850_ipcc_dispatch(&common,32));
        assert(audio_calls==1&&mss_calls==1&&clears==3&&!held);
        assert(sm8850_ipcc_register(&common,0x20002,mss,0)==Sm8850Conflict);
        uint64_t adsp=0,ipa=0;
        assert(!sm8850_resource_acquire(&common,8,0x4e41,&adsp));
        assert(!sm8850_resource_acquire(&common,NB_RES_SMMU,NB_OWNER,&ipa));
        assert(NB_RES_SMMU==9&&adsp&&ipa&&adsp!=ipa);
        assert(common.owner[8]==0x4e41&&common.owner[9]==NB_OWNER);
    }
    puts("PASS audio102/MSS: either startup order, AOP route retained, append afterdispatch, separate ADSP/IPA leases");
}
