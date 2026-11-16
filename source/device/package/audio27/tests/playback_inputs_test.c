/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Local-only test with caller-supplied private input. No device or audio I/O.
 * Revision bytes below are explicitly synthetic; they are NOT a board fact. */
#include "../platform/AudioPlaybackInputs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static _Alignas(4096) uint8_t dma[65536];
static uint32_t rd(const uint8_t *p) {uint32_t n;memcpy(&n,p,4);return n;}
static void wr(uint8_t *p,uint32_t n) {memcpy(p,&n,4);}
static uint8_t *section(uint8_t *b,unsigned kind) {
    uint32_t at=12;
    for (unsigned i=0;i<5;++i) {uint32_t k=rd(b+at),n=rd(b+at+4);if (k==kind) return b+at+8;at+=8+((n+7)&~7u);}
    abort();
}
static void roles(const NaInputPack *pack) {
    const char *names[]={"speaker.graph","capture.graph","vi.graph"};
    for (unsigned i=0;i<3;++i) {
        NaFirmwarePiece f;assert(!na_input_find(pack,names[i],&f));
        NaMediaEndpoints e;uint32_t protection=0;
        assert(!na_topology_resolve(f.data,(uint32_t)f.bytes,i,&e,&protection));
        assert(e.capture==i && (i==1 ? !protection:!!protection));
        NaTopology t;assert(!na_topology_load(f.data,(uint32_t)f.bytes,dma,sizeof dma,&e,&t));
        /* Same valid instances, wrong roles: old loader incorrectly accepted. */
        if (i<2) {
            uint32_t tmp=e.pcm[0];e.pcm[0]=e.pcm[1];e.pcm[1]=tmp;
            memset(dma,0xa5,sizeof dma);
            assert(na_topology_load(f.data,(uint32_t)f.bytes,dma,sizeof dma,&e,&t)==NA_MISSING);
            for (unsigned j=0;j<sizeof dma;++j) assert(dma[j]==0xa5);
        }
        uint8_t *copy=malloc((size_t)f.bytes);assert(copy);memcpy(copy,f.data,(size_t)f.bytes);
        uint8_t *tags=section(copy,5);uint32_t saved=rd(tags+16);
        wr(tags+16,rd(tags+8)); /* duplicate tag, distinct valid instance */
        NaMediaEndpoints unchanged;memset(&unchanged,0xa5,sizeof unchanged);e=unchanged;protection=0xabcdef;
        assert(na_topology_resolve(copy,(uint32_t)f.bytes,i,&e,&protection)==NA_WIRE);
        assert(!memcmp(&e,&unchanged,sizeof e) && protection==0xabcdef);
        wr(tags+16,saved);wr(tags+12,0xdeadbeef); /* missing OPEN instance */
        assert(na_topology_resolve(copy,(uint32_t)f.bytes,i,&e,&protection)==NA_MISSING);
        if (i!=1) {
            memcpy(copy,f.data,(size_t)f.bytes);
            uint8_t *hardware=section(copy,3);wr(hardware+20,1); /* wrong secondary TDM */
            assert(na_topology_resolve(copy,(uint32_t)f.bytes,i,&e,&protection)==NA_MISSING);
            wr(hardware+20,0);wr(hardware+32,0x00200004); /* four slots on two-slot clock */
            assert(na_topology_resolve(copy,(uint32_t)f.bytes,i,&e,&protection)==NA_MISSING);
        }
        free(copy);
    }
}
int main(int argc,char **argv) {
    assert(argc==5);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>0 && bytes<=0x8000000);
    rewind(f);uint8_t *blob=malloc((size_t)bytes);assert(blob);
    assert(fread(blob,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    NaInputPack pack;assert(!na_input_pack(&pack,blob,(uint64_t)bytes));roles(&pack);
    NaTasSelection selection={.revision={0x51,0x52},.revision_mask={0xff,0xff},
        .profile=argv[2],.rx_format=argv[3],.tx_format=argv[4]};
    NaPlaybackInputs inputs={0};int result=na_playback_inputs(&inputs,&pack,&selection);
    if (result) fprintf(stderr,"playback input result=%d\n",result);
    assert(!result && inputs.valid && inputs.calibration.valid && inputs.selection.calibrated);
    assert(!selection.calibrated); /* caller cannot assert calibration success */
    NaAssemblyPlan plan={0};assert(!na_playback_plan(&plan,&inputs));
    assert(plan.tuning==&inputs.tuning && plan.stream[0].graph && plan.stream[2].graph);
    assert(!plan.stream[1].graph && !plan.capture_proven && !plan.capture_probe_allowed && !plan.adsp_lease);
    NaCaptureInputs mic={0};assert(!na_capture_inputs(&mic,&pack));
    assert(mic.valid && mic.media.capture==1 && mic.media.channels==1);
    NaAssemblyPlan empty={0};assert(na_capture_plan(&empty,&mic)==NA_ARGUMENT);
    assert(!na_capture_plan(&plan,&mic) && plan.stream[1].graph==mic.graph.data);
    assert(plan.capture_probe_allowed && !plan.capture_proven && !plan.input_votes);
    assert(na_capture_inputs(&mic,&pack)==NA_ARGUMENT && na_capture_plan(&plan,&mic)==NA_ARGUMENT);
    assert(na_playback_plan(&plan,&inputs)==NA_ARGUMENT);
    assert(na_playback_inputs(&inputs,&pack,&selection)==NA_ARGUMENT);
    NaPlaybackInputs fresh={0};selection.revision_mask[0]=0;
    assert(na_playback_inputs(&fresh,&pack,&selection)==NA_ARGUMENT && !fresh.valid);
    selection.revision_mask[0]=0xff;selection.dvc_index[0]=0xffffffff;
    assert(na_playback_inputs(&fresh,&pack,&selection)==NA_MISSING && !fresh.valid);
    selection.dvc_index[0]=0;selection.profile="missing-profile";
    assert(na_playback_inputs(&fresh,&pack,&selection)==NA_MISSING && !fresh.valid);selection.profile=argv[2];
    NaFirmwarePiece cal;assert(!na_input_find(&pack,"ti_calr.bin",&cal));
    uint8_t *value=blob+(cal.data-blob);uint8_t saved=value[3];value[3]|=0x80;
    assert(na_playback_inputs(&fresh,&pack,&selection)==NA_WIRE && !fresh.valid);value[3]=saved;
    /* A required private member cannot silently fall back to another graph. */
    unsigned entry=0;while (strcmp((char *)blob+16+64*entry,"vi.graph")) {assert(++entry<pack.count);}
    blob[16+64*entry]='x';assert(na_playback_inputs(&fresh,&pack,&selection)==NA_MISSING && !fresh.valid);
    entry=0;while (strcmp((char *)blob+16+64*entry,"capture.graph")) {assert(++entry<pack.count);}
    blob[16+64*entry]='x';NaCaptureInputs missing={0};
    assert(na_capture_inputs(&missing,&pack)==NA_MISSING && !missing.valid);
    free(blob);
    puts("playback inputs: three real graph roles, swapped roles, duplicate/missing IDs, calibration and plan guards PASS; synthetic revisions, no hardware");
    return 0;
}
