// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaAudioAdspInputs.h"
#include <Sm8850ResourcesNative.h>
#include <string.h>
extern "C" volatile int nezha_touch_stage;
namespace {
int failed(NaNativeAdspInputs *s,int r) {s->error=r;return r;}
void progress(void *v,uint32_t pas,uint32_t point,uint32_t segment,uint64_t offset,uint64_t bytes,uint64_t pa) {
    auto *s=static_cast<NaNativeAdspInputs *>(v);
    if (s->progress) s->progress(s->progress_context,pas,point,segment,offset,bytes,pa);
}
}
extern "C" int na_native_pas_sync(void *v,void *cpu,uint64_t pa,uint64_t bytes) {
    auto *s=static_cast<NaNativeAdspInputs *>(v);
    if (!s || !cpu || !s->lease || !bytes || bytes>0x1000000 || pa>UINT64_MAX-bytes) return NA_ARGUMENT;
    for (auto &m:s->metadata) {
        if (!m.descriptor || !m.va || pa<m.physical || pa-m.physical>=m.bytes || bytes>m.bytes-(pa-m.physical)) continue;
        if (cpu!=m.va+(pa-m.physical)) return NA_OWNERSHIP;
        return na_native_clean(nullptr,cpu,pa,bytes); // owned cached RAM, unchanged
    }
    const NaRange expected[]={na_nezha_facts.firmware,na_nezha_facts.dtb};
    for (unsigned i=0;i<2;++i) {
        const auto &w=s->region[i];const auto &r=w.physical;
        if (!w.mapping || !w.descriptor || !w.va || w.lease!=s->lease || r.base!=expected[i].base || r.bytes!=expected[i].bytes ||
            pa<r.base || pa-r.base>=r.bytes || bytes>r.bytes-(pa-r.base)) continue;
        if (cpu!=const_cast<uint8_t *>(w.va)+(pa-r.base)) return NA_OWNERSHIP;
        // na_native_map uses kIOMapWriteCombineCache for these exact two
        // windows. Like the existing GPU/Pandora PAS loader, publish the WC
        // stores with DSB; flush_dcache64(...,TRUE) wrongly walks the RAM
        // physmap for a separately mapped reserved firmware region.
        na_native_publish_writes();return NA_OK;
    }
    return NA_OWNERSHIP;
}
extern "C" int na_native_prepare_adsp_inputs(NaNativeAdspInputs *s,NaAssemblyPlan *p) {
    if (!s || !p || s->attempted || p->adsp_lease) return NA_ARGUMENT;
    // USB initialization is once per boot. Its old/new banks stay reserved.
    // Touch stage7 is after its final GCC/RSC/SMMU setup; recovery only resets
    // its endpoint/GPI transport. Do not race either initial resource scan.
    if (__atomic_load_n(&nezha_touch_stage,__ATOMIC_ACQUIRE)!=7 || !sm8850_usb_domain_ready()) return NA_BUSY;
    s->attempted=1;
    int r=na_native_audio_inputs(&s->pack);if (r) return failed(s,r);
    for (unsigned i=0;i<2;++i) {
        r=na_input_firmware(&s->pack,i,s->pieces[i],QCOM_MDT_MAX_SEGMENTS,&s->count[i],&s->mdt[i]);
        if (r) return failed(s,r);
        r=na_split_firmware(&s->firmware[i],s->pieces[i],s->count[i]);if (r) return failed(s,r);
        if (qcom_mdt_metadata_size(&s->firmware[i].image)!=s->mdt[i].bytes) return failed(s,NA_WIRE);
    }
    Sm8850Resources *common=sm8850_native_resources();if (!common) return failed(s,NA_IO);
    r=na_resource_owner_init(&s->resources,common);if (r) return failed(s,r);
    // Resource8 owns only ADSP PAS1/36 and their fixed board carveouts.
    r=sm8850_resource_acquire(common,8,0x4e41,&s->lease);
    if (r) return failed(s,NA_OWNERSHIP);
    const NaRange range[]={na_nezha_facts.firmware,na_nezha_facts.dtb};
    for (unsigned i=0;i<2;++i) {
        uint64_t span;
        r=na_firmware_contract(&s->firmware[i].image,range[i],&span);if (r) return failed(s,r);
        uint32_t bytes=(uint32_t)((s->mdt[i].bytes+4095)&~UINT64_C(4095));
        r=na_native_allocate(&s->metadata[i],bytes);if (r) return failed(s,r);
        r=qcom_mdt_metadata(&s->firmware[i].image,na_split_read,&s->firmware[i],s->metadata[i].va,bytes);
        if (r || memcmp(s->metadata[i].va,s->mdt[i].data,(size_t)s->mdt[i].bytes)) return failed(s,NA_WIRE);
        r=na_native_map(&s->region[i],range[i],s->lease);if (r) return failed(s,r);
    }
    p->adsp_lease=s->lease;p->pas={s,na_native_pas,na_native_pas_sync,progress};
    for (unsigned i=0;i<2;++i) {
        p->firmware[i]=s->pieces[i];p->firmware_count[i]=s->count[i];
        p->firmware_map[i]=const_cast<uint8_t *>(s->region[i].va);
        p->metadata[i]={s->metadata[i].va,{s->metadata[i].physical,s->metadata[i].bytes}};
    }
    return NA_OK;
}
