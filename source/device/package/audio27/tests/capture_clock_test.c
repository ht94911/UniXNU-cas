/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Execute the board/WCD/SoundWire production code with the existing register
 * peer. Bank commands, clock ratios and ordering are checked; no real PCM. */
#define main original_mic_revision_main
#include "mic_revision_test.c"
#undef main
static NaWcdCapture codec;
static void prepared_board(Fixture *f,NaMicBoard *board,unsigned bank) {
    memset(f,0,sizeof *f);memset(&codec,0,sizeof codec);
    f->version=0x02020000;f->reset=0xffff03ff;f->reset_output=2;f->bank=bank;
    NaBoardIo io={f,claim,release,read_register,update_register,rails,now_us,pause_us,NULL};
    assert(!na_mic_board_init(board,&io,1,6));
    int r=NA_PENDING;
    for (unsigned i=0;i<20 && r==NA_PENDING;++i) {
        r=na_mic_board_prepare(board,&codec);f->now+=2000;
    }
    assert(!r && board->stage==5 && codec.version==1);
    assert(*stored(f,SWR+0x101c)==(5u<<16|15u));
    for (unsigned i=0;i<3;++i) assert(*stored(f,0x076600d0ULL+4*i)==0xcc);
}
static int prepared_codec(Fixture *f) {
    int r=NA_PENDING;
    for (unsigned i=0;i<100 && r==NA_PENDING;++i) {
        r=na_wcd_capture_prepare(&codec,f->now/1000);f->now+=2000;
    }
    return r;
}
int main(void) {
    Fixture f;NaMicBoard board;
    for (unsigned bank=0;bank<2;++bank) {
        prepared_board(&f,&board,bank);
        /* Android can leave a different interval in either bank. The upper
         * byte is part of the stock configuration, even when it is zero. */
        *stored(&f,SWR+0x123c)=0xa5;*stored(&f,SWR+0x127c)=0x5a;
        /* The prior adapter enabled packing, while the original1-bit PDM
         * route retains0. Initialize both slave and master banks explicitly. */
        *stored(&f,SWR+0x1238)=1;*stored(&f,SWR+0x1278)=1;
        *stored(&f,0x100000127ULL)=1;*stored(&f,0x100000137ULL)=1;
        static const struct {uint32_t reg;uint8_t value;} defaults[]={
            {0x88,0x60},{0xc0,0x0e},{0x508,0x1b},{0x50c,0},
            {0x510,0},{0x514,0},{0x518,1},{0x51c,0x3c},
            {0x520,0x20},{0x524,0},{0x528,0},
            {0x904,0x11},{0x908,1},{0x90c,2}
        };
        for (unsigned i=0;i<sizeof defaults/sizeof defaults[0];++i)
            *stored(&f,0x06ae0000ULL+defaults[i].reg)=0xa50000a5;
        *stored(&f,0x06ae0588ULL)=0xb6000066; /* neighboring DEC3 */
        *stored(&f,0x06ae0900ULL)=0xa5000051;
        *stored(&f,0x06ae0530ULL)=0xa5000003;
        assert(!prepared_codec(&f));
        assert(codec.prepared && codec.muted && !codec.active);
        for (unsigned i=0;i<sizeof defaults/sizeof defaults[0];++i)
            assert(*stored(&f,0x06ae0000ULL+defaults[i].reg)==(0xa5000000u|defaults[i].value));
        assert(*stored(&f,0x06ae0588ULL)==0xb6000066);
        /* The original capture endpoint's adaptive initialization, including
         * stale bypass/control values left by a preceding Android use. */
        assert(*stored(&f,0x06ae0934ULL)==0x59 && *stored(&f,0x06ae0938ULL)==6);
        assert((*stored(&f,0x06ae0900ULL)&0xff)==0x41 &&
            (*stored(&f,0x06ae0530ULL)&0xff)==1);
        static const uint32_t tune[7]={0x4c,0x1d,0x7c,0x3c,0x67,0x16,3};
        for(unsigned i=0;i<7;++i) assert((*stored(&f,0x06ae0910ULL+4*i)&0xff)==tune[i]);
        assert(f.broadcasts==1 && f.host_div2==1 && f.bank==(bank^1));
        assert(!f.slave_clock_writes); /* this WCD939x has no paging clock path */
        for (unsigned b=0;b<2;++b) {
            assert(*stored(&f,0x100000122ULL+16*b)==3);
            assert(*stored(&f,SWR+0x1224+64*b)==(4u<<24|3u));
            assert(!*stored(&f,SWR+0x123c+64*b));
            assert(!*stored(&f,SWR+0x1238+64*b));
            assert(!*stored(&f,0x100000127ULL+16*b));
            assert(*stored(&f,SWR+0x101c+64*b)==(5u<<16|15u));
        }
        assert(19200000u/(3u+1)==4800000u);
        assert(19200000u/(50u*16u*6u)==4000u);
        f.now+=300000;assert(!na_wcd_capture_activate(&codec,f.now/1000));
        assert(codec.active && !codec.muted && codec.adc_gain==6);
        assert((*stored(&f,0x10000300fULL)&0x1f)==6);
        assert(codec.registers_done==1 && codec.registers_valid[0]==NA_MIC_CAPTURE_REG_WORDS && !codec.registers_error[0]);
        assert(codec.registers[0][1]==(bank^1) && codec.registers[0][2]==(5u<<16|15u));
        assert(codec.registers[0][5]==(4u<<24|3u) && codec.registers[0][17]==3 && codec.registers[0][18]==1);
        assert(codec.registers[0][22]==0xa5000041 && codec.registers[0][21]==0xa5000001);
        assert(!codec.registers[0][13] && !codec.registers[0][14]);
        assert(!codec.registers[0][10] && !codec.registers[0][11]);
        assert((codec.registers[0][19]&0x10)==0 && codec.registers[0][29]==0x63);
        assert(codec.registers[0][23]==0xa500001b && codec.registers[0][24]==0xa5000000);
        assert(codec.registers[0][25]==0xa5000001 && codec.registers[0][26]==0xa5000011);
        assert(codec.registers[0][27]==0xa5000001 && codec.registers[0][28]==0xa5000002);
        assert(!na_wcd_capture_mute(&codec));
        assert(codec.registers_done==3 && codec.registers_valid[1]==NA_MIC_CAPTURE_REG_WORDS && !codec.registers_error[1]);
        assert((codec.registers[1][19]&0x10)==0); // sampled before the mute write
        uint32_t done=codec.bus->reads;assert(!na_wcd_capture_mute(&codec) && codec.bus->reads==done);
        assert(!na_wcd_capture_shutdown(&codec,1));
        assert(*stored(&f,0x06ae0530ULL)==0xa5000003);
        assert(!na_mic_board_release(&board) && !board.lease && !f.rails);
    }
    prepared_board(&f,&board,0);f.block_switch=1;uint64_t before=f.now;
    assert(prepared_codec(&f)==NA_TIMEOUT);
    assert(codec.error==NA_TIMEOUT && !codec.prepared && board.swr.failed);
    assert(f.broadcasts==1 && f.host_div2==1 && f.now-before<1000000);
    assert(!*stored(&f,0x06ae0110ULL)); /* no TX routing after failed switch */
    puts("production capture clocks: dual-edge SI3/4.8MHz,6SSP/4kHz, VA division, exact DIV2/bank broadcast, both starting banks, bounded failed switch PASS; synthetic registers only");
}
