/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "TiCalibration.h"
#include <string.h>
static uint32_t rd(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void wr(uint8_t *p,uint32_t n) {p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static int decimal(NaCalibrationFile f,uint32_t *q19,int integer) {
    if (!f.data || !q19 || !f.bytes || f.bytes>24) return NA_ARGUMENT;
    uint32_t n=f.bytes;while (n && (f.data[n-1]=='\n' || f.data[n-1]=='\r' || f.data[n-1]==' ')) --n;
    if (!n) return NA_WIRE;
    uint32_t value=0,scale=1,digits=0,point=0;
    for (unsigned i=0;i<n;++i) {
        uint8_t ch=f.data[i];
        if (ch=='.' && !integer && !point && digits && i+1<n) {point=1;continue;}
        if (ch<'0' || ch>'9' || digits++>=8 || (point && scale>=1000000)) return NA_WIRE;
        value=value*10+(ch-'0');if (point) scale*=10;
    }
    uint64_t q=((uint64_t)value<<19)/scale;
    if (q>UINT32_MAX) return NA_WIRE;*q19=(uint32_t)q;return NA_OK;
}
int na_ti_calibration_parse(NaTiCalibration *out,NaCalibrationFile binary,const NaCalibrationFile resistance[2],const NaCalibrationFile temp[2],const NaTasImage *image,const char *profile) {
    if (!out || !binary.data || binary.bytes!=8 || !resistance || !temp || !image || !profile) return NA_ARGUMENT;
    NaTiCalibration c={0};
    for (unsigned i=0;i<2;++i) {
        uint32_t bits=rd(binary.data+4*i),exponent=(bits>>23)&255,mantissa=(bits&0x7fffff)|0x800000;
        if ((bits>>31) || exponent<127 || exponent>132) return NA_WIRE;
        int shift=4-(int)(exponent-127);
        uint32_t q=shift>=0 ? mantissa>>shift:mantissa<<(-shift),ascii,t;
        if (q<(1u<<19) || q>(32u<<19) || decimal(resistance[i],&ascii,0) || decimal(temp[i],&t,1)) return NA_WIRE;
        if ((q>ascii ? q-ascii:ascii-q)>5243 || t>(85u<<19) || (t&((1u<<19)-1))) return NA_MISSING;
        c.r0_q19[i]=q;c.temperature[i]=t>>19;
        NaTasProfile p;int r=na_tas_profile(image,i,profile,&p);if (r) return r;
        if ((p.misc&0x10) || (p.misc&15)!=3 || (i && p.misc!=c.driver_mode)) return NA_MISSING;
        c.driver_mode=p.misc;
    }
    if (image->iv_width!=12 || image->vbat_monitor!=1) return NA_MISSING;
    c.iv_width=image->iv_width;c.vbat=image->vbat_monitor;c.valid=1;*out=c;return NA_OK;
}
static void header(uint8_t *p,uint32_t instance,uint32_t param,uint32_t words) {
    wr(p,instance);wr(p+4,param|(words<<16)|(1u<<28));wr(p+8,words*4);wr(p+12,0);
}
int na_ti_calibration_payload(const NaTiCalibration *c,uint32_t instance,uint8_t out[88],uint32_t *bytes) {
    if (!c || !c->valid || !instance || !out || !bytes || c->iv_width!=12 || c->vbat!=1 || (c->driver_mode&0x10) || (c->driver_mode&15)!=3) return NA_ARGUMENT;
    for (unsigned i=0;i<2;++i) if (c->r0_q19[i]<(1u<<19) || c->r0_q19[i]>(32u<<19) || c->temperature[i]>85) return NA_WIRE;
    memset(out,0,88);header(out,instance,3859,5);wr(out+16,2);
    wr(out+20,c->r0_q19[0]);wr(out+24,c->r0_q19[1]);wr(out+28,c->temperature[0]);wr(out+32,c->temperature[1]);
    header(out+40,instance,3840,1);wr(out+56,c->iv_width|(c->vbat<<16));
    header(out+64,instance,3854,1);wr(out+80,c->driver_mode);
    *bytes=88;return NA_OK;
}
