/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production PAS loader against both real private firmware families. PAS and
 * cache callbacks are host fixtures; guarded RAM replaces firmware mappings.
 * Verifies copied bytes/BSS, bounded progress and failure without replay. */
#include "../platform/AudioInputPack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    NaPasLoad *load;void *metadata;uint8_t *mapped;NaRange region;
    uint32_t id,calls[7],syncs,point,observations,fail_sync;
} Fixture;
static int pas(void *v,uint32_t command,uint32_t id,uint64_t a,uint64_t b) {
    Fixture *f=v;assert(id==f->id && (command==1 || command==2 || command==5));++f->calls[command];
    if (command==1) assert(a==0x60000000 && !b);
    if (command==2) assert(a==f->region.base && b && b<=f->region.bytes);
    if (command==5) assert(!a && !b && f->point==NaPasAuthBegin);
    return NA_OK;
}
static int sync_memory(void *v,void *cpu,uint64_t pa,uint64_t bytes) {
    Fixture *f=v;
    if (cpu==f->metadata) {assert(pa==0x60000000 && bytes==65536);return NA_OK;}
    assert(pa>=f->region.base && bytes && bytes<=65536 && bytes<=f->region.bytes-(pa-f->region.base));
    assert(cpu==f->mapped+(pa-f->region.base) && f->point==NaPasSyncBegin);
    ++f->syncs;return f->fail_sync ? NA_IO:NA_OK;
}
static void observe(void *v,uint32_t id,uint32_t point,uint32_t segment,uint64_t offset,uint64_t bytes,uint64_t pa) {
    Fixture *f=v;assert(id==f->id && segment==f->load->segment && offset==f->load->offset);
    assert(bytes && pa>=f->region.base && bytes<=f->region.bytes-(pa-f->region.base));
    if (point==NaPasCopyBegin) assert(f->point==0 || f->point==NaPasSyncReturn);
    else if (point==NaPasAuthBegin) assert(f->point==NaPasSyncReturn);
    else assert(point==f->point+1);
    f->point=point;++f->observations;
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);long bytes=ftell(file);assert(bytes>0);rewind(file);
    void *blob=malloc((size_t)bytes);assert(blob && fread(blob,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);
    NaInputPack pack;assert(!na_input_pack(&pack,blob,(uint64_t)bytes));
    for (unsigned kind=0;kind<2;++kind) {
        NaFirmwarePiece pieces[QCOM_MDT_MAX_SEGMENTS],mdt;uint32_t count;NaSplitFirmware *fw=calloc(1,sizeof *fw);assert(fw);
        assert(!na_input_firmware(&pack,kind,pieces,QCOM_MDT_MAX_SEGMENTS,&count,&mdt));assert(!na_split_firmware(fw,pieces,count));
        NaRange region=kind ? na_nezha_facts.dtb:na_nezha_facts.firmware;
        uint8_t *guarded=aligned_alloc(4096,(size_t)region.bytes+8192);void *metadata=aligned_alloc(4096,65536);assert(guarded && metadata);
        for (unsigned failure=0;failure<2;++failure) {
            memset(guarded,0xa5,(size_t)region.bytes+8192);NaPasLoad load={0};
            Fixture f={.load=&load,.metadata=metadata,.mapped=guarded+4096,.region=region,.id=kind ? 36:1,.fail_sync=failure};
            NaPasOps ops={&f,pas,sync_memory,observe};
            assert(!na_pas_begin(&load,fw,f.id,region,f.mapped,metadata,0x60000000,65536,&ops,0));
            int r=NA_PENDING;unsigned ticks=0;
            while (r==NA_PENDING && ++ticks<5000) r=na_pas_poll(&load,ticks);
            assert(ticks<5000 && f.calls[1]==1 && f.calls[2]==(unsigned)fw->image.relocatable);
            if (failure) {
                assert(r==NA_IO && load.stage==NaPasFault && f.syncs==1 && !f.calls[5]);
                unsigned observations=f.observations;
                assert(na_pas_poll(&load,ticks+1)==NA_IO && f.syncs==1 && f.observations==observations);
            } else {
                assert(r==NA_OK && load.stage==NaPasReady && f.calls[5]==1 && f.point==NaPasAuthReturn);
                for (unsigned i=0;i<fw->image.count;++i) if (fw->image.segment[i].load) {
                    const QcomMdtSegment *segment=&fw->image.segment[i];uint64_t offset;
                    assert(!qcom_mdt_place(&fw->image,i,region.base,region.bytes,&offset));
                    if (segment->file_size) assert(!memcmp(f.mapped+offset,pieces[i].data,(size_t)segment->file_size));
                    for (uint64_t n=segment->file_size;n<segment->memory_size;++n) assert(!f.mapped[offset+n]);
                }
                printf("PAS%u: complete real input copy/BSS, %u bounded chunks, %u polls; host callbacks only\n",f.id,f.syncs,ticks);
            }
            assert(!memcmp(metadata,mdt.data,(size_t)mdt.bytes));
            for (unsigned i=0;i<4096;++i) assert(guarded[i]==0xa5 && guarded[4096+region.bytes+i]==0xa5);
        }
        free(metadata);free(guarded);free(fw);
    }
    free(blob);
}
