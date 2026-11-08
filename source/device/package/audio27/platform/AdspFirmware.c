/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AdspFirmware.h"
#include <string.h>
#include <limits.h>
static int piece_read(void *v,unsigned piece,uint64_t offset,void *to,size_t bytes) {
    NaSplitFirmware *f=v;
    if (piece>=f->split.count || !to || !f->piece[piece].data || offset>f->piece[piece].bytes || bytes>f->piece[piece].bytes-offset) return NA_WIRE;
    memcpy(to,f->piece[piece].data+offset,bytes);return NA_OK;
}
int na_split_read(void *v,uint64_t offset,void *to,size_t bytes) {
    NaSplitFirmware *f=v;return f ? qcom_mdt_split_read(&f->split,piece_read,f,offset,to,bytes):NA_ARGUMENT;
}
int na_split_firmware(NaSplitFirmware *f,const NaFirmwarePiece *pieces,uint32_t count) {
    if (!f || !pieces || !count || count>QCOM_MDT_MAX_SEGMENTS || !pieces[0].data || pieces[0].bytes>65536) return NA_ARGUMENT;
    memset(f,0,sizeof *f);memcpy(f->piece,pieces,count*sizeof *pieces);
    if (qcom_mdt_split(&f->split,pieces[0].data,(size_t)pieces[0].bytes) || f->split.count!=count || f->split.size>0x10000000ULL) return NA_WIRE;
    for (unsigned i=0;i<count;++i) if (f->split.piece[i].size &&
        (!pieces[i].data || pieces[i].bytes!=f->split.piece[i].size)) return NA_WIRE;
    return qcom_mdt_parse(&f->image,f->split.size,na_split_read,f) ? NA_WIRE:NA_OK;
}
int na_pas_begin(NaPasLoad *l,NaSplitFirmware *f,uint32_t pas,NaRange range,void *mapped,
                 void *metadata,uint64_t pa,uint64_t capacity,const NaPasOps *ops,uint64_t now) {
    if (!l || !f || !mapped || !metadata || !pa || (pa&4095) || ((uintptr_t)metadata&4095) || (capacity&4095) ||
        capacity>0x100000 || now>UINT64_MAX-30000 || !ops || !ops->pas || !ops->sync ||
        !((pas==1 && range.base==na_nezha_facts.firmware.base) || (pas==36 && range.base==na_nezha_facts.dtb.base))) return NA_ARGUMENT;
    uint64_t span;
    if (na_firmware_contract(&f->image,range,&span) || !na_range_valid((NaRange){pa,capacity}) ||
        na_ranges_overlap((NaRange){pa,capacity},na_nezha_facts.firmware) || na_ranges_overlap((NaRange){pa,capacity},na_nezha_facts.dtb) ||
        qcom_mdt_metadata_size(&f->image)>capacity) return NA_OWNERSHIP;
    /* Reject overlapping load segments as well as carveout overflow. */
    for (unsigned i=0;i<f->image.count;++i) if (f->image.segment[i].load) {
        uint64_t at;qcom_mdt_place(&f->image,i,range.base,range.bytes,&at);
        for (unsigned j=0;j<i;++j) if (f->image.segment[j].load) {
            uint64_t other;qcom_mdt_place(&f->image,j,range.base,range.bytes,&other);
            if (na_ranges_overlap((NaRange){at,f->image.segment[i].memory_size},(NaRange){other,f->image.segment[j].memory_size})) return NA_WIRE;
        }
    }
    memset(metadata,0,(size_t)capacity);
    if (qcom_mdt_metadata(&f->image,na_split_read,f,metadata,capacity)) return NA_WIRE;
    int r=ops->sync(ops->context,metadata,pa,capacity);if (r) return r;
    memset(l,0,sizeof *l);l->firmware=f;l->ops=*ops;l->region=range;l->mapped=mapped;l->metadata=metadata;
    l->metadata_pa=pa;l->metadata_capacity=capacity;l->span=span;l->pas=pas;l->stage=NaPasInit;l->deadline=now+30000;
    return NA_OK;
}
static int failed(NaPasLoad *l,int r) {l->stage=NaPasFault;l->error=r<0 ? r:NA_IO;return l->error;}
static void progress(NaPasLoad *l,uint32_t point,uint64_t bytes,uint64_t pa) {
    if (l->ops.progress) l->ops.progress(l->ops.context,l->pas,point,l->segment,l->offset,bytes,pa);
}
int na_pas_poll(NaPasLoad *l,uint64_t now) {
    if (!l || l->stage==NaPasIdle) return NA_ARGUMENT;
    if (l->stage==NaPasFault) return l->error;
    if (l->stage==NaPasReady) return NA_OK;
    if (now>=l->deadline) return failed(l,NA_TIMEOUT);
    int r=0;QcomMdtImage *image=&l->firmware->image;
    if (l->stage==NaPasInit) {
        l->possible=1;r=l->ops.pas(l->ops.context,1,l->pas,l->metadata_pa,0);
        if (!r) l->stage=image->relocatable ? NaPasMemory:NaPasCopy;
    } else if (l->stage==NaPasMemory) {
        r=l->ops.pas(l->ops.context,2,l->pas,l->region.base,l->span);if (!r) l->stage=NaPasCopy;
    } else if (l->stage==NaPasCopy) {
        while (l->segment<image->count && !image->segment[l->segment].load) ++l->segment;
        if (l->segment==image->count) {l->stage=NaPasAuthenticate;return NA_PENDING;}
        QcomMdtSegment *p=&image->segment[l->segment];uint64_t at;
        if (qcom_mdt_place(image,l->segment,l->region.base,l->region.bytes,&at)) return failed(l,NA_WIRE);
        uint64_t bytes=p->memory_size-l->offset;if (bytes>65536) bytes=65536;
        uint64_t file=l->offset<p->file_size ? p->file_size-l->offset:0;if (file>bytes) file=bytes;
        progress(l,NaPasCopyBegin,bytes,l->region.base+at+l->offset);
        if (file) r=na_split_read(l->firmware,p->file_offset+l->offset,l->mapped+at+l->offset,(size_t)file);
        if (!r) progress(l,NaPasDataCopied,bytes,l->region.base+at+l->offset);
        if (!r && bytes>file) memset(l->mapped+at+l->offset+file,0,(size_t)(bytes-file));
        if (!r) {
            progress(l,NaPasBssZeroed,bytes,l->region.base+at+l->offset);
            progress(l,NaPasSyncBegin,bytes,l->region.base+at+l->offset);
            r=l->ops.sync(l->ops.context,l->mapped+at+l->offset,l->region.base+at+l->offset,bytes);
            progress(l,NaPasSyncReturn,bytes,l->region.base+at+l->offset);
        }
        if (!r) {l->offset+=bytes;if (l->offset==p->memory_size) {++l->segment;l->offset=0;}}
    } else if (l->stage==NaPasAuthenticate) {
        /* SM8850 hypervisor creates PIL metadata bridge; don't duplicate it. */
        progress(l,NaPasAuthBegin,l->span,l->region.base);
        r=l->ops.pas(l->ops.context,5,l->pas,0,0);
        progress(l,NaPasAuthReturn,l->span,l->region.base);
        if (!r) l->stage=NaPasReady;
    } else return failed(l,NA_OWNERSHIP);
    return r ? failed(l,r):l->stage==NaPasReady ? NA_OK:NA_PENDING;
}
