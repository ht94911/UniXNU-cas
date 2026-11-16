/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Board constants: N0 OS4 live-props.txt qcom,hv-haptics@f000. Effects are
 * represented by the original FIFO lengths; no vendor waveform file here.
 * Six effects use 4/6/2/3/3/6 samples of 0x7f; primitive0 silence, others2. */
#include "NezhaHaptics.h"
static int discover(void *v,unsigned ee,const uint16_t *ppids,PghPeriph *p,uint32_t *version,uint32_t *channels) {
    NzHapticsBus *b=v;if(ee)return -22;
    *version=b->bus.version;*channels=b->bus.count[0]+b->bus.count[1];
    for(unsigned i=0;i<PGH_PERIPHS;++i) {
        int e=sm8850_spmi_v8_find(&b->bus,i==PGH_SDAM?0:1,ppids[i],&b->channels[i]);
        if(e && i!=PGH_SDAM)return e;
        p[i].ppid=ppids[i];p[i].apid=e?-1:(int16_t)b->channels[i].apid;
        p[i].owner=b->channels[i].owner;p[i].irq=b->channels[i].irq_owner;p[i].mappings=b->channels[i].mappings;
    }
    return 0;
}
static int owns(void *v,unsigned p) {
    NzHapticsBus *b=v;if(p>=PGH_PERIPHS)return -22;
    return sm8850_spmi_v8_owns(&b->bus,&b->channels[p]);
}
static int read_reg(void *v,unsigned p,uint8_t reg,uint8_t *data,unsigned n) {
    NzHapticsBus *b=v;if(p>=PGH_PERIPHS)return -22;
    return sm8850_spmi_v8_read(&b->bus,&b->channels[p],reg,data,n);
}
static int write_reg(void *v,unsigned p,uint8_t reg,const uint8_t *data,unsigned n) {
    NzHapticsBus *b=v;
    /* This board writes only CFG/PTN, subject to the component's register
     * whitelist; HBOOST, SDAM, OTP and PMIC calibration storage stay read-only. */
    if(p!=PGH_CFG && p!=PGH_PTN)return -13;
    if(p==PGH_CFG && (reg==0x72 || reg==0x5e || reg==0x5f))return -13;
    if(p==PGH_PTN && (reg>=0x70 && reg<0x78))return -13;
    return sm8850_spmi_v8_write(&b->bus,&b->channels[p],reg,data,n);
}
static uint32_t status(void *v) {return ((NzHapticsBus *)v)->bus.status;}
int nz_haptics_bus_init(NzHapticsBus *b,const Sm8850SpmiOps *ops) {
    if(!b)return -22;
    __builtin_memset(b,0,sizeof *b);
    int e=sm8850_spmi_v8_init(&b->bus,ops,0);if(e)return e;
    b->transport=(PghSpmiTransport){b,discover,owns,read_reg,write_reg,status};
    b->io.transport=&b->transport;return 0;
}
void nz_haptics_config(PghConfig *c,const PghFactoryState *f,unsigned mode) {
    __builtin_memset(c,0,sizeof *c);c->mode=mode;c->ee=0;
    c->ppid[PGH_CFG]=0x7f0;c->ppid[PGH_PTN]=0x7f1;c->ppid[PGH_HBST]=0x7f2;c->ppid[PGH_SDAM]=0x09d;
    c->t_lra_us=5555;c->vmax_mv=2000;c->effect_mv=3600;c->fifo_mv=3600;
    /* The original custom_vmax=8500 is a vendor custom-effect limit, not a
     * voltage for this source candidate. All requests stop at factory2000. */
    c->hard_mv=2000;c->cap_mv=2000;c->max_play_ms=NZ_HAPTICS_MAX_MS;c->max_stream_ms=NZ_HAPTICS_MAX_MS;
    c->budget_ms=500;c->refill_permille=500;c->drv_shape=1;c->brake_shape=1;c->brake_len=0;
    c->f0_min_dhz=1600;c->f0_max_dhz=1800;c->factory=f;c->log=1;c->autores=1;c->spmi_retry=0;
    static const uint8_t cycles[NZ_HAPTICS_EFFECT_COUNT]={4,6,2,3,3,6,0,2,2,2,2,2,2,2,2};
    __builtin_memcpy(c->effect_cycles,cycles,sizeof cycles);c->effects=NZ_HAPTICS_EFFECT_COUNT;
}
