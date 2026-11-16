/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Source-only test cases, deliberately synthetic; never device calibration. */
#include "../core/AudioTopology.h"
#include "../platform/TiCalibration.h"
#include <assert.h>
#include <string.h>
static void wr(uint8_t *p,uint32_t n) {p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static uint32_t rd(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void modules(void) {
    uint8_t b[40]={0};uint32_t iid=0;
    wr(b,1);wr(b+4,0x08001002);wr(b+8,24);
    wr(b+16,1);wr(b+20,0xb0000002);wr(b+24,0xe0000002);wr(b+28,1);
    wr(b+32,0x870010a8);wr(b+36,0x4329);
    assert(!na_spf_validate(b,sizeof b));assert(!na_topology_find_module(b,sizeof b,0x870010a8,&iid) && iid==0x4329);
    assert(na_topology_find_module(b,sizeof b,0x870010a9,&iid)==NA_MISSING);
    wr(b+28,2);assert(na_topology_find_module(b,sizeof b,0x870010a8,&iid)==NA_WIRE);
    wr(b+28,1);wr(b+12,1);assert(na_spf_validate(b,sizeof b)==NA_WIRE);
}
static void calibration(void) {
    uint8_t profile[89]={0};wr(profile,85);profile[4]=1;memcpy(profile+5,"test",5);wr(profile+69,3);
    NaTasImage image={0};image.iv_width=12;image.vbat_monitor=1;
    image.section[0][NaTasProfiles]=(NaTasBytes){profile,sizeof profile};image.section[1][NaTasProfiles]=image.section[0][NaTasProfiles];
    uint8_t binary[8]={0,0,0xc0,0x40,0,0,0xe0,0x40}; /*6.0,7.0*/
    NaCalibrationFile r[2]={{(const uint8_t *)"6.00",4},{(const uint8_t *)"7.00",4}};
    NaCalibrationFile t[2]={{(const uint8_t *)"25",2},{(const uint8_t *)"24",2}};
    NaTiCalibration c={0};
    assert(!na_ti_calibration_parse(&c,(NaCalibrationFile){binary,8},r,t,&image,"test"));
    assert(c.r0_q19[0]==6u*524288 && c.r0_q19[1]==7u*524288 && c.driver_mode==3);
    uint8_t payload[88];uint32_t bytes=0;
    assert(!na_ti_calibration_payload(&c,0x4329,payload,&bytes) && bytes==88);
    assert(rd(payload)==0x4329 && rd(payload+4)==0x10050f13 && rd(payload+8)==20 && rd(payload+16)==2);
    assert(rd(payload+20)==6u*524288 && rd(payload+24)==7u*524288 && rd(payload+28)==25 && rd(payload+32)==24);
    assert(rd(payload+44)==0x10010f00 && rd(payload+56)==0x1000c);
    assert(rd(payload+68)==0x10010f0e && rd(payload+80)==3);
    assert(!na_spf_validate(payload,sizeof payload));
    c.driver_mode|=0x10;assert(na_ti_calibration_payload(&c,0x4329,payload,&bytes)==NA_ARGUMENT);
    t[0]=(NaCalibrationFile){(const uint8_t *)"-1",2};
    assert(na_ti_calibration_parse(&c,(NaCalibrationFile){binary,8},r,t,&image,"test")==NA_WIRE);
    t[0]=(NaCalibrationFile){(const uint8_t *)"25",2};binary[3]=0x7f;binary[2]=0xc0;
    assert(na_ti_calibration_parse(&c,(NaCalibrationFile){binary,8},r,t,&image,"test")==NA_WIRE);
}
int main(void) {modules();calibration();return 0;}
