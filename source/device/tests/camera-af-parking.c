/* SPDX-License-Identifier: MIT */
#include "NezhaCameraAfParking.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Frozen original OFILM/SEMCO GenerateParkLens instruction outputs. */
struct Case { uint16_t current,first,last; unsigned count; uint16_t dac[50]; };
static const struct Case cases[]={
{540,50,2100,11,{1164,1538,1762,1897,1978,2026,2055,2073,2083,2093,2100}},
{1000,50,2100,10,{1440,1704,1862,1957,2014,2048,2068,2080,2090,2100}},
{100,50,2100,12,{900,1380,1668,1840,1944,2006,2043,2065,2079,2089,2099,2100}},
{0,50,2100,13,{50,870,1362,1657,1834,1940,2004,2042,2065,2079,2089,2099,2100}},
{2500,1800,4050,11,{3120,3492,3715,3849,3929,3977,4006,4023,4033,4043,4050}},
{3965,1800,4050,5,{3999,4019,4031,4041,4050}},
{1350,900,2100,10,{1650,1830,1938,2002,2041,2064,2078,2088,2098,2100}},
{1350,2100,900,8,{1170,1062,997,958,934,920,910,900}},
{0,200,3947,14,{200,1698,2597,3137,3461,3655,3771,3841,3883,3908,3923,3933,3943,3947}},
{4095,3947,200,14,{3947,2448,1548,1008,684,490,374,304,262,237,222,212,202,200}},
{200,200,200,1,{200}},
{200,0,200,1,{200}},
{3947,3947,200,13,{2448,1548,1008,684,490,374,304,262,237,222,212,202,200}},
{3965,2000,1800,8,{2000,1920,1872,1843,1825,1815,1805,1800}},
{4095,0,0,2,{0,0}},
};
int main(void){
struct NezhaCameraAfParking plan;
for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i){
assert(nezha_camera_af_parking(cases[i].current,cases[i].first,cases[i].last,&plan));
assert(plan.count==cases[i].count);
assert(!memcmp(plan.dac,cases[i].dac,plan.count*sizeof(*plan.dac)));
for(unsigned j=0;j<plan.count;++j)assert(plan.delay_ms[j]==10);
}
assert(!nezha_camera_af_parking(4096,900,2100,&plan));
assert(!nezha_camera_af_parking(900,900,2100,NULL));
assert(!nezha_camera_af_parking(2200,900,2100,&plan));
assert(!nezha_camera_af_parking(800,2100,900,&plan));
puts("AF parking matched15 original move sequences, reversed intervals and10ms delays");
return 0;
}
