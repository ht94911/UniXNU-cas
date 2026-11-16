/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The qualified Xiaomi status calculation (OS4 module159cfe080f10;
 * MiCode popsicle-w-oss45705be, haptics_get_closeloop_lra_period).
 * Uses existing volatile results only. No auto-calibration/PBS/SDAM write. */
#include "NezhaHaptics.h"
static int read(NzHapticsBus *b,unsigned p,uint8_t r,uint8_t *out,unsigned n) {
    return b->transport.read(b,p,r,out,n);
}
static int selection(NzHapticsBus *b,uint16_t s,uint8_t out[2]) {
    uint8_t xt=(uint8_t)(s>>8),sel=(uint8_t)s;
    int e=b->transport.write(b,PGH_CFG,0x71,&xt,1);
    if(!e)e=b->transport.write(b,PGH_CFG,0x70,&sel,1);
    if(!e)e=read(b,PGH_CFG,0x09,out,2);
    return e;
}
int nz_haptics_factory(NzHapticsBus *b,PghFactoryState *f) {
    if(!b || !f)return -22;
    __builtin_memset(f,0,sizeof *f);
    uint8_t playing=0,vreg=0,follow=0;
    int lease=read(b,PGH_CFG,0x4c,&playing,1);
    if(!lease)lease=read(b,PGH_HBST,0x46,&vreg,1);
    if(!lease)lease=read(b,PGH_HBST,0x41,&follow,1);
    if(lease)return lease;
    if((playing&0x80) || ((vreg&0x80) && !(follow&0x80)))return -16;
    uint8_t old[2]={0},cal=0,count[2]={0},ol[2]={0},result[2]={0},last[2]={0},duty=0,autores=0;
    int e=read(b,PGH_CFG,0x70,old,2);
    if(!e)e=read(b,PGH_CFG,0x72,&cal,1);
    if(!e)e=read(b,PGH_CFG,0x5e,count,2);
    if(!e)e=read(b,PGH_CFG,0x5c,ol,2);
    if(!e)e=read(b,PGH_CFG,0x60,&duty,1);
    if(!e)e=read(b,PGH_CFG,0x63,&autores,1);
    if(e)return e;
    /* SWR_ACCESS bits6/7 enable the input interface; they do not report a
     * playing motor. Stock is_swr_play_enabled reads HAP_DRV_STS selector5,
     * whose low three bits identify source4 (SWR). Keep that real owner. */
    uint8_t driver[2]={0};
    e=selection(b,5,driver);
    if(!e && (driver[1]&7u)==4u)e=-16;
    if(!e)e=selection(b,0,result);
    if(!e)e=selection(b,3,last);
    /* Restore the selection even when a read failed; a timed-out bus prevents
     * another CMD until the pending one has a real DONE status. */
    int restore=b->transport.write(b,PGH_CFG,0x71,old+1,1);
    if(!restore)restore=b->transport.write(b,PGH_CFG,0x70,old,1);
    if(e || restore)return e?e:restore;
    const uint64_t c=((uint64_t)(result[0]&31)<<8)|result[1];
    const uint64_t l=((uint64_t)(last[0]&31)<<8)|last[1];
    const uint32_t raw=((uint32_t)(ol[0]&15)<<8)|ol[1];
    uint32_t us=raw*5;
    if((result[0]&0x20) && c) {
        if((cal&12)==4 && l)us=(uint32_t)(l*l*833333/c/1000000)+70; /* original rev_t_lra_us */
        else if(!(cal&12))us=(uint32_t)(c*3333000/1000000);
    }
    /* The original accepts160..180Hz. Without a usable result use exactly the
     * already programmed period when it is in that range; never invent f0. */
    f->t_lra_us=us;f->rc_count=(uint16_t)(((count[0]&3u)<<8)|count[1]);f->rc_mode=cal&12;
    if(us<1000000/180 || us>1000000/160)return -34;
    if((cal&12)!=0 && (cal&12)!=4 && (cal&12)!=8)return -22;
    f->drv_duty=duty;f->autores_cfg=autores;f->valid=1;return 0;
}
