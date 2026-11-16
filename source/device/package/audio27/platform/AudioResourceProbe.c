/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioResourceProbe.h"
#include "AdspPower.h"
#include <string.h>
static int r32(NaResourceSnapshot *s,const NaResourceReader *r,unsigned d,uint32_t o,uint32_t *v) {
    s->domain=d;s->offset=o;++s->reads;
    return r->read32(r->context,d,o,v) ? NA_IO:NA_OK;
}
static int r64(NaResourceSnapshot *s,const NaResourceReader *r,uint32_t o,uint64_t *v) {
    s->domain=NaProbeSmmu;s->offset=o;++s->reads;
    return r->read64(r->context,NaProbeSmmu,o,v) ? NA_IO:NA_OK;
}
static int capture(NaResourceSnapshot *s,const NaResourceReader *r) {
    s->phase=1;
    for (unsigned i=0;i<3;++i) if (r32(s,r,NaProbeSmmu,0x20+4*i,&s->id[i])) return NA_IO;
    uint32_t id0=s->id[0],id1=s->id[1];
    s->stream_count=id0&255;s->bank_count=id1&255;s->stage2_banks=(id1>>16)&255;
    s->page_bytes=(id1>>31) ? 65536:4096;
    uint32_t pages=1u<<(((id1>>28)&7)+1);
    s->context_base=pages*s->page_bytes;
    /* Same geometry as the retained ArmSmmu4KProgram. The observer is bounded
     * to the stock 1 MiB resource and the architectural 128 match slots. */
    if (!(id0&(1u<<27)) || !s->stream_count || s->stream_count>NA_PROBE_STREAMS ||
        !s->bank_count || s->bank_count>NA_PROBE_BANKS || s->bank_count>pages ||
        s->stage2_banks>s->bank_count || 2u*s->context_base>0x100000) return NA_MISSING;
    s->phase=2;
    for (unsigned i=0;i<s->stream_count;++i)
        if (r32(s,r,NaProbeSmmu,0x800+4*i,&s->stream[i].smr) ||
            r32(s,r,NaProbeSmmu,0xc00+4*i,&s->stream[i].s2cr)) return NA_IO;
    s->phase=3;
    /* Stage2-only banks are outside the proposed HLOS audio allocation. */
    for (unsigned i=s->stage2_banks;i<s->bank_count;++i) {
        NaProbeBank *b=&s->bank[i];uint64_t ttbr0,ttbr1;
        uint32_t cb=s->context_base+i*s->page_bytes;
        if (r32(s,r,NaProbeSmmu,cb,&b->sctlr) || r32(s,r,NaProbeSmmu,cb+4,&b->actlr) ||
            r32(s,r,NaProbeSmmu,s->page_bytes+4*i,&b->cbar) ||
            r32(s,r,NaProbeSmmu,s->page_bytes+0x800+4*i,&b->cba2r) ||
            r64(s,r,cb+0x20,&ttbr0) || r64(s,r,cb+0x28,&ttbr1)) return NA_IO;
        b->asid0=(uint32_t)(ttbr0>>48);b->asid1=(uint32_t)(ttbr1>>48);
    }
    s->phase=4;QcomRscLayout layout;
    if (r32(s,r,NaProbeRsc,0,&s->rsc_id) || r32(s,r,NaProbeRsc,0xc,&s->rsc_config)) return NA_IO;
    if (na_proxy_layout(s->rsc_id,s->rsc_config,&layout)) return NA_MISSING;
    const uint32_t offsets[4]={layout.status,layout.control,layout.cmd_enable,layout.wait_for_cmpl};
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<4;++j)
        if (r32(s,r,NaProbeRsc,0xd00+i*layout.tcs_stride+offsets[j],&s->tcs[i][j])) return NA_IO;
    if (r32(s,r,NaProbeGcc,0x18,&s->gcc_vote)) return NA_IO;
    s->phase=5;
    /* Detect configuration changes during the census. This check is evidence
     * of a stable read, not mutual exclusion or a hardware resource lease. */
    uint32_t value;
    for (unsigned i=0;i<3;++i) {
        if (r32(s,r,NaProbeSmmu,0x20+4*i,&value)) return NA_IO;
        if (value!=s->id[i]) return NA_BUSY;
    }
    for (unsigned i=0;i<s->stream_count;++i) {
        if (r32(s,r,NaProbeSmmu,0x800+4*i,&value)) return NA_IO;
        if (value!=s->stream[i].smr) return NA_BUSY;
        if (r32(s,r,NaProbeSmmu,0xc00+4*i,&value)) return NA_IO;
        if (value!=s->stream[i].s2cr) return NA_BUSY;
    }
    s->phase=6;return NA_OK;
}
int na_resource_capture(NaResourceSnapshot *s,const NaResourceReader *r) {
    if (!s || !r || !r->read32 || !r->read64) return NA_ARGUMENT;
    memset(s,0,sizeof *s);s->error=capture(s,r);return s->error;
}
