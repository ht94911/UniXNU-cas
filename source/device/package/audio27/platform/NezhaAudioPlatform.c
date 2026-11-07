/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaAudioPlatform.h"
#include <string.h>
#include <limits.h>
/* Saved stock DTS: remoteproc-adsp, tas25xx@4a, lpass-cdc, and
 * wcd939x_reset_active. gpio162 belongs to USBSS; audio codec uses gpio161.
 * See SOURCE.md for the field-level source locations and unverified inputs. */
const NaBoardFacts na_nezha_facts={
    {0xa1400000,0x5580000},{0xa1380000,0x80000},
    0x01980000,0x06ae0000,0x07660000,0x07630000,
    {0x4a,0x48},{54,56},47,161,{0x0e,0x01170223},2750,
    {{0x1001,0x80},{0x1041,0x20}}
};
int na_range_valid(NaRange a) {return a.bytes && a.base<=UINT64_MAX-a.bytes;}
int na_ranges_overlap(NaRange a,NaRange b) {
    if (!na_range_valid(a) || !na_range_valid(b)) return 1;
    return a.base<b.base+b.bytes && b.base<a.base+a.bytes;
}
int na_sid_overlap(NaSid a,NaSid b) {
    return (((uint32_t)a.id^b.id)&~((uint32_t)a.mask|b.mask)&0xffffu)==0;
}
int na_dma_contract(NaRange physical,uint32_t iova,NaSid sid,
                    const NaRange *reserved,uint32_t count,const NaSid *streams,
                    uint32_t stream_count,uint64_t *encoded) {
    if (!encoded || !na_range_valid(physical) || ((physical.base|physical.bytes|iova)&4095) ||
        physical.bytes>UINT32_MAX || (uint64_t)iova+physical.bytes>0x100000000ULL ||
        !iova || count>128 || stream_count>128 || (count && !reserved) || (stream_count && !streams)) return NA_ARGUMENT;
    if (!((sid.id==0x1001 && sid.mask==0x80) || (sid.id==0x1041 && sid.mask==0x20))) return NA_MISSING;
    if (na_ranges_overlap(physical,na_nezha_facts.firmware) || na_ranges_overlap(physical,na_nezha_facts.dtb)) return NA_OWNERSHIP;
    for (uint32_t i=0;i<count;++i) if (na_ranges_overlap(physical,reserved[i])) return NA_OWNERSHIP;
    for (uint32_t i=0;i<stream_count;++i) if (na_sid_overlap(sid,streams[i])) return NA_OWNERSHIP;
    *encoded=(uint64_t)(sid.id&0xf)<<32|iova;return NA_OK;
}
int na_firmware_contract(const QcomMdtImage *image,NaRange range,uint64_t *span) {
    if (!image || !span || !na_range_valid(range) || (range.base&4095) ||
        !((range.base==na_nezha_facts.firmware.base && range.bytes==na_nezha_facts.firmware.bytes) ||
          (range.base==na_nezha_facts.dtb.base && range.bytes==na_nezha_facts.dtb.bytes))) return NA_ARGUMENT;
    if (!image->count || image->count>QCOM_MDT_MAX_SEGMENTS || image->hash<0 ||
        (unsigned)image->hash>=image->count || !qcom_mdt_metadata_size(image)) return NA_WIRE;
    uint32_t loaded=0;
    for (uint32_t i=0;i<image->count;++i) if (image->segment[i].load) {
        uint64_t offset;
        if (qcom_mdt_place(image,i,range.base,range.bytes,&offset)) return NA_WIRE;
        ++loaded;
    }
    *span=qcom_mdt_span(image);
    return loaded && *span && *span<=range.bytes ? NA_OK : NA_WIRE;
}
int na_endpoint_init(NaEndpoint *e,const NaEndpointProfile *p,const NaEndpointOps *ops) {
    if (!e || !p || !ops || !ops->claim || !ops->power_muted || !ops->identify ||
        !ops->configure_muted || !ops->gain || !ops->mute || !ops->shutdown || !ops->release ||
        !p->device_count || p->device_count>2 || p->capture>1 || !p->channels || p->channels>2 ||
        p->rate!=48000 || p->bits!=16 || !p->max_gain_q15 || p->max_gain_q15>4096) return NA_ARGUMENT;
    uint32_t required=NaProofIdentity|NaProofPower|NaProofCalibration|NaProofTopology|NaProofClock|NaProofDma;
    if (p->capture) required|=NaProofCaptureRoute;
    if ((p->proofs&required)!=required) return NA_MISSING;
    for (unsigned i=0;i<p->device_count;++i) if (!p->device_id[i]) return NA_MISSING;
    memset(e,0,sizeof *e);e->profile=*p;e->ops=*ops;return NA_OK;
}
int na_endpoint_mute(NaEndpoint *e) {
    if (!e) return NA_ARGUMENT;
    if (!e->lease) return NA_OK;
    int r=e->ops.mute(e->ops.context,e->lease,1);
    if (r) {e->cleanup_error=r;e->state=NaEndpointFault;}
    else if (e->state!=NaEndpointFault) e->state=NaEndpointMuted;
    return r;
}
int na_endpoint_release(NaEndpoint *e,int quiescent) {
    if (!e) return NA_ARGUMENT;
    if (!e->lease) return NA_OK;
    int r=na_endpoint_mute(e);
    if (!quiescent) return NA_OWNERSHIP;
    int stopped=e->ops.shutdown(e->ops.context,e->lease);
    if (stopped) {e->cleanup_error=stopped;e->state=NaEndpointFault;return stopped;}
    int released=e->ops.release(e->ops.context,e->lease);
    if (released) {e->cleanup_error=released;e->state=NaEndpointFault;return released;}
    e->lease=0;e->gain_q15=0;e->state=r ? NaEndpointFault : NaEndpointOff;
    return r;
}
int na_endpoint_prepare(NaEndpoint *e) {
    if (!e || e->state!=NaEndpointOff || e->lease || e->error) return NA_OWNERSHIP;
    int r=e->ops.claim(e->ops.context,&e->lease);
    if (r || !e->lease) {
        e->error=r ? r : NA_OWNERSHIP;
        /* On claim failure ownership must be empty; a returned lease remains
         * quarantined rather than being passed to an unknown release owner. */
        if (e->lease) e->state=NaEndpointFault;
        return e->error;
    }
    uint32_t ids[2]={0,0};
    r=e->ops.power_muted(e->ops.context,e->lease);
    if (!r) r=e->ops.identify(e->ops.context,e->lease,ids,e->profile.device_count);
    for (unsigned i=0;!r && i<e->profile.device_count;++i)
        if (ids[i]!=e->profile.device_id[i]) r=NA_MISSING;
    if (!r) r=e->ops.configure_muted(e->ops.context,e->lease,&e->profile);
    if (!r) r=e->ops.gain(e->ops.context,e->lease,0);
    if (r) {e->error=r;na_endpoint_release(e,1);return r;}
    e->state=NaEndpointMuted;return NA_OK;
}
int na_endpoint_gain(NaEndpoint *e,uint32_t q15) {
    if (!e || !e->lease || e->state==NaEndpointFault || q15>e->profile.max_gain_q15) return NA_ARGUMENT;
    int r=e->ops.gain(e->ops.context,e->lease,q15);
    if (r) {e->error=r;na_endpoint_mute(e);e->state=NaEndpointFault;}
    else e->gain_q15=q15;
    return r;
}
int na_endpoint_activate(NaEndpoint *e) {
    if (!e || e->state!=NaEndpointMuted || !e->lease || e->error) return NA_OWNERSHIP;
    int r=e->ops.mute(e->ops.context,e->lease,0);
    if (r) {e->error=r;na_endpoint_mute(e);e->state=NaEndpointFault;}
    else e->state=NaEndpointActive;
    return r;
}
int na_endpoint_input_publishable(const NaEndpoint *e) {
    return e && e->profile.capture && (e->profile.proofs&NaProofCapturePcm) &&
        !e->error && e->state!=NaEndpointFault;
}
