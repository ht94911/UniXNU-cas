/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Exercise the production board and GENI preparation with a register oracle.
 * This checks leased pin/clock behavior, not a physical amplifier identity. */
#include "../platform/NezhaAudioBoard.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GCC 0x100000ULL
/* Independent oracle from stock Canoe GPIO-group offsets and DT base. */
#define TLMM 0xf100000ULL
#define SE 0x1980000ULL
#define VOTE (GCC+0x52018)
typedef struct {uint64_t pa;uint32_t value;} Cell;
typedef struct {
    Cell cells[96];unsigned count,claims,releases,reads,writes;
    uint64_t touched[512],fail_read,fail_write;
    unsigned touches;uint64_t now;
} Fake;
static Cell *cell(Fake *f,uint64_t pa) {
    for (unsigned i=0;i<f->count;++i) if (f->cells[i].pa==pa) return &f->cells[i];
    assert(f->count<96);Cell *c=&f->cells[f->count++];*c=(Cell){pa,0};return c;
}
static void touch(Fake *f,uint64_t pa) {
    assert(!(pa>=0xf000000ULL && pa<0xf100000ULL));
    assert(f->touches<512);f->touched[f->touches++]=pa;
}
static int claim(void *v,uint32_t kind,uint64_t *lease) {
    Fake *f=v;assert(kind==NaBoardTas && !f->claims);++f->claims;*lease=42;return NA_OK;
}
static int release(void *v,uint32_t kind,uint64_t lease) {
    Fake *f=v;assert(kind==NaBoardTas && lease==42 && !f->releases);++f->releases;return NA_OK;
}
static int read32(void *v,uint64_t lease,uint64_t pa,uint32_t *value) {
    Fake *f=v;assert(lease==42);touch(f,pa);++f->reads;
    if (pa==f->fail_read) return NA_IO;
    *value=cell(f,pa)->value;return NA_OK;
}
static int update32(void *v,uint64_t lease,uint64_t pa,uint32_t mask,uint32_t value) {
    Fake *f=v;assert(lease==42 && mask && !(value&~mask));touch(f,pa);++f->writes;
    if (pa==f->fail_write) return NA_IO;
    Cell *c=cell(f,pa);c->value=(c->value&~mask)|value;return NA_OK;
}
static uint64_t now(void *v) {return ((Fake *)v)->now;}
static void pause_us(void *v,uint32_t n) {((Fake *)v)->now+=n;}
static void init(Fake *f) {
    memset(f,0,sizeof *f);
    const unsigned pins[]={54,56,64,65,174,122,123,124,125};
    for (unsigned i=0;i<9;++i) cell(f,TLMM+4096*pins[i])->value=0xabc00000u|pins[i];
    cell(f,TLMM+4096*54+4)->value=10;cell(f,TLMM+4096*56+4)->value=10;
    cell(f,TLMM+4096*174+4)->value=10;
    cell(f,VOTE)->value=0x07c00100;
    cell(f,GCC+0xa8018)->value=1; /* Existing XO /1 entry. */
    cell(f,SE+GENI_FW_REVISION)->value=GENI_PROTO_I2C<<8;
    cell(f,SE+0x7c)->value=9;cell(f,SE+GENI_CLK_CFG)->value=0x4440;
    cell(f,SE+GENI_SCL_COUNTERS)->value=0x123456;
    cell(f,SE+GENI_SE_IRQ_EN)->value=0x100;
}
static NaBoardIo io(Fake *f) {return (NaBoardIo){f,claim,release,read32,update32,0,now,pause_us,0};}
static void identity_accesses(const Fake *f) {
    for (unsigned i=0;i<f->touches;++i) {
        uint64_t pa=f->touched[i];
        if (pa>=TLMM && pa<TLMM+0x100000) {
            unsigned p=(unsigned)((pa-TLMM)/4096);
            assert(p==54 || p==56 || p==64 || p==65);
        }
    }
}
static void restored(Fake *f) {
    assert(cell(f,VOTE)->value==0x07c00100);
    assert(cell(f,TLMM+4096*64)->value==(0xabc00000u|64));
    assert(cell(f,TLMM+4096*65)->value==(0xabc00000u|65));
    assert(!(cell(f,TLMM+4096*54+4)->value&2));
    assert(!(cell(f,TLMM+4096*56+4)->value&2));
    assert(cell(f,SE+0x7c)->value==9 && cell(f,SE+GENI_CLK_CFG)->value==0x4440);
    assert(cell(f,SE+GENI_SCL_COUNTERS)->value==0x123456);
}
static void identity_case(unsigned failure) {
    Fake f;init(&f);NaGeniBoard board;NaTasPowerOps ops;NaBoardIo bus=io(&f);
    assert(!na_geni_board_init_identity(&board,&bus,&ops) && board.identity_only);
    uint64_t lease=0;assert(!ops.claim(ops.context,&lease) && lease==42);
    assert(ops.reset(ops.context,43,1)==NA_OWNERSHIP);
    assert(!ops.reset(ops.context,lease,1));
    if (failure==1) cell(&f,SE+GENI_FW_REVISION)->value=1u<<8;
    if (failure==2) f.fail_read=SE+GENI_SCL_COUNTERS;
    if (failure==3) cell(&f,GCC+0xa8018)->value=7;
    int r=ops.power(ops.context,lease,1);
    if (failure) assert(r==NA_IO || r==NA_MISSING);
    else {
        assert(!r && board.bus.ready && board.powered);
        assert(cell(&f,VOTE)->value==0x07c01300);
        assert(!ops.reset(ops.context,lease,0));
    }
    assert(!ops.reset(ops.context,lease,1));
    assert(!ops.power(ops.context,lease,0));
    assert(!ops.release(ops.context,lease) && !board.lease && f.releases==1);
    identity_accesses(&f);restored(&f);
    const unsigned other[]={174,122,123,124,125};
    for (unsigned i=0;i<5;++i) assert(cell(&f,TLMM+4096*other[i])->value==(0xabc00000u|other[i]));
    assert(cell(&f,TLMM+4096*174+4)->value==10);
    assert(cell(&f,GCC+0xa8018)->value==(failure==3 ? 7u:1u));
}
int main(void) {
    for (unsigned f=0;f<4;++f) identity_case(f);
    Fake f;init(&f);NaGeniBoard board;NaTasPowerOps ops;NaBoardIo bus=io(&f);uint64_t lease=0;
    assert(!na_geni_board_init(&board,&bus,&ops) && !board.identity_only);
    assert(!ops.claim(ops.context,&lease));assert(!ops.reset(ops.context,lease,1));
    assert(!ops.power(ops.context,lease,1));
    assert((cell(&f,TLMM+4096*174)->value&0x3ff)==0x203);
    assert(!ops.route(ops.context,lease,0));
    assert((cell(&f,TLMM+4096*174)->value&0x3ff)==0x200);
    assert((cell(&f,TLMM+4096*174+4)->value&2)==0);
    assert(!ops.route(ops.context,lease,1));
    assert((cell(&f,TLMM+4096*174)->value&0x3ff)==0x203);
    assert((cell(&f,TLMM+4096*174+4)->value&2)==2);
    for (unsigned p=122;p<=125;++p) assert((cell(&f,TLMM+4096*p)->value&0x3ff)==0xc4);
    assert(!ops.reset(ops.context,lease,1));assert(!ops.power(ops.context,lease,0));
    assert(!ops.release(ops.context,lease));restored(&f);
    puts("PASS: identity uses reset/I2C only, restores clocks/pins after three power failures; full playback board retains TDM/receiver behavior");
}
