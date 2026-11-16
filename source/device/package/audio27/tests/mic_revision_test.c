/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../platform/NezhaAudioBoard.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define RESET (NA_TLMM_GPIO_BASE+161*4096)
#define SWR 0x07630000ULL
typedef struct {
    uint64_t now;
    uint32_t version,reset,reset_output,lpi[4];
    struct {uint64_t address;uint32_t value;} registers[128];
    unsigned register_count;
    uint32_t read_word;
    unsigned read_pending;
    unsigned version_reads,swr_reset_writes,claims,releases,rails;
    unsigned bank,broadcasts,host_div2,block_switch,slave_clock_writes;
} Fixture;
static const unsigned pins[]={0,1,2,14};
static int claim(void *v,uint32_t kind,uint64_t *lease) {Fixture *f=v;assert(kind==NaBoardCapture);++f->claims;*lease=77;return 0;}
static int release(void *v,uint32_t kind,uint64_t lease) {Fixture *f=v;assert(kind==NaBoardCapture && lease==77);++f->releases;return 0;}
static uint32_t *stored(Fixture *f,uint64_t address) {
    if(address==RESET)return &f->reset;
    if(address==RESET+4)return &f->reset_output;
    for(unsigned i=0;i<4;++i)if(address==0x07760000ULL+4096*pins[i])return &f->lpi[i];
    for(unsigned i=0;i<f->register_count;++i)if(f->registers[i].address==address)return &f->registers[i].value;
    assert(f->register_count<128);
    unsigned i=f->register_count++;f->registers[i].address=address;return &f->registers[i].value;
}
static int read_register(void *v,uint64_t lease,uint64_t address,uint32_t *out) {
    Fixture *f=v;assert(lease==77);
    if(address==SWR){++f->version_reads;*out=f->version;}
    else if(address==SWR+0x5064)*out=1;
    else if(address==SWR+0x1090)*out=4;
    else if(address==SWR+0x104c)*out=f->bank;
    else if(address==SWR+0x538)*out=0x01170223;
    else if(address==SWR+0x53c)*out=0x0e;
    else if(address==SWR+0x5000)*out=0;
    else if(address==SWR+0x5050)*out=f->read_pending ? 1u<<16:0;
    else if(address==SWR+0x5040){*out=f->read_word;f->read_pending=0;}
    else *out=*stored(f,address);
    return 0;
}
static int update_register(void *v,uint64_t lease,uint64_t address,uint32_t mask,uint32_t value) {
    Fixture *f=v;assert(lease==77 && !(value&~mask));
    if(address==SWR+8)++f->swr_reset_writes;
    if(address==SWR+0x5024) {
        uint16_t reg=(uint16_t)value;
        uint8_t data=reg==0x3402 ? 1:(uint8_t)*stored(f,0x100000000ULL+reg);
        f->read_word=((value>>16)&15u)<<8|data;f->read_pending=1;
    } else if(address==SWR+0x5020) {
        uint16_t reg=(uint16_t)value;
        if (reg==0x4d || reg==0x62 || reg==0x72) ++f->slave_clock_writes;
        if (((value>>20)&15)==15 && (reg==0xe0 || reg==0xf0)) {
            assert(value>>24==1);++f->host_div2;
        }
        if (((value>>20)&15)==15 && (reg==0x60 || reg==0x70)) {
            assert(((value>>16)&15)==15 && value>>24==NA_SWR_TX_FRAME_SHAPE);
            ++f->broadcasts;
            if (!f->block_switch) f->bank=(reg-0x60)/16;
        }
        *stored(f,0x100000000ULL+(uint16_t)value)=value>>24;
    }
    uint32_t *cell=stored(f,address);*cell=(*cell&~mask)|value;return 0;
}
static int rails(void *v,uint64_t lease,int on) {Fixture *f=v;assert(lease==77);f->rails=!!on;return 0;}
static uint64_t now_us(void *v) {return ((Fixture *)v)->now;}
static void pause_us(void *v,uint32_t delay) {((Fixture *)v)->now+=delay;}
static int prepare(uint32_t version,Fixture *f,NaMicBoard *board) {
    memset(f,0,sizeof *f);f->version=version;f->reset=0xffff03ff;f->reset_output=2;
    NaBoardIo io={f,claim,release,read_register,update_register,rails,now_us,pause_us,NULL};
    assert(!na_mic_board_init(board,&io,1,2));NaWcdCapture capture={0};int r=NA_PENDING;
    for(unsigned turn=0;turn<20 && r==NA_PENDING;++turn){r=na_mic_board_prepare(board,&capture);f->now+=2000;}
    return r;
}
int main(void) {
    Fixture f;NaMicBoard board;
    const uint32_t supported[]={0x02000000,0x02020000};
    for(unsigned i=0;i<2;++i) {
        assert(prepare(supported[i],&f,&board)==NA_OK);
        assert(board.stage==5 && !board.error && f.version_reads==2 && f.swr_reset_writes==1);
        assert((f.reset&~0x3ffu)==0xffff0000u);
        assert(!na_mic_board_release(&board) && f.releases==1 && !board.lease && !f.rails);
    }
    assert(prepare(0x02010000,&f,&board)==NA_MISSING);
    assert(board.error && f.version_reads==1 && !f.swr_reset_writes && !(f.reset_output&2));
    assert(na_mic_board_prepare(&board,&(NaWcdCapture){0})==NA_OWNERSHIP && f.version_reads==1);
    puts("Mic board: stock2.0 and observed2.2 initialize/release, unidentified revision stops before reset/enumeration");
    return 0;
}
