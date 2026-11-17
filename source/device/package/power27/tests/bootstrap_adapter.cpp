// SPDX-License-Identifier: BSD-2-Clause-Patent
// The checker includes the complete production bootstrap/probe source below.
// Only external hardware/OS callbacks are simulated; no guard is reimplemented.
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <IOKit/IOLib.h>
extern "C" {
#include "NezhaPowerBoard.h"
}
using wait_result_t=int;
using thread_t=void *;
using vm_offset_t=uintptr_t;
using vm_size_t=size_t;
#define KERN_SUCCESS 0
static unsigned variant,starts,threads,terminations,maps,board_prepares,binds,adc_calls;
static uint64_t slept_ms;
static int selected_mode=5,selected_ffc;
static volatile int nezha_touch_stage=5;
static uint32_t started;
static constexpr uint64_t leaseId=0x5032373031350001ULL;
struct {
    uintptr_t gcc,smmu,gpi,se,smem,mutex,ipcc,tlmm;
    IOLock *lock,*smemLock;
    bool ready,pumpPinOwned;
    NpAuthMaterial authMaterial;
    NpAdcNativeBootProbe adcProbe;
} binding;
static NpPowerBoard board;
void IOSleep(unsigned ms){slept_ms+=ms;}
static void delay(int){}
static int PE_parse_boot_argn(const char *key,void *out,int bytes) {
    assert(bytes==(int)sizeof(int));int value;
    if(!strcmp(key,"nezha-power"))value=selected_mode;
    else if(!strcmp(key,"nezha-power-ffc")){if(selected_mode==10||selected_mode==11)return 0;value=selected_ffc;}
    else if(!strcmp(key,"nezha-power-adc"))value=selected_mode==11?4:3;
    else if(!strcmp(key,"nezha-power-tsens"))value=0;
    else if(!strcmp(key,"nezha-power-auth"))value=selected_mode==5||selected_mode>=8;
    else return 0;
    memcpy(out,&value,sizeof(value));return 1;
}
static uintptr_t ml_io_map(uintptr_t physical,size_t){++maps;return physical;}
static uint32_t rd(uintptr_t base,uint32_t off) {
    if(base==0xa94000) {
        if(off==0x68)return 3u<<8;
        if(off==0x64)return 1;
        if(off==0x48)return 1u<<4;
        if(off==0x278)return 1;
    }
    return 0;
}
static int routePlan(){return variant==7?EACCES:0;}
static int vote(void *,uint64_t,int){return 0;}
static int pumpWake(void *,uint64_t,int){return 0;}
static int valid(void *,uint64_t){return 1;}
static int read32(void *,uint64_t,uint64_t,uint32_t *){return 0;}
static int dma(void *,uint64_t,uint32_t,uint64_t,uint32_t){return 0;}
static int smemLock(void *){return 1;}
static void smemUnlock(void *){}
static void ring(void *,uint32_t,uint32_t){}
static uint64_t now(void *){return slept_ms;}
static void pause(void *,uint32_t){}
static void authWait(void *,uint32_t){}
static int quiesceAdc(void *){return 0;}
static int beginConnector(void *,unsigned,NpAdcNativeReading *){++adc_calls;return 1;}
static int pollConnector(void *,NpAdcNativeReading *){++adc_calls;return 1;}
extern "C" int np_auth_native_material(NpAuthMaterial *){return 0;}
extern "C" int np_auth_native_random(void *,uint8_t *,size_t){return 0;}
extern "C" int np_auth_native_hmac(void *,const uint8_t *,size_t,const uint8_t *,size_t,uint8_t [32]){return 0;}
extern "C" int np_tsens_native_prepare(void){assert(false);return 0;}
extern "C" int np_tsens_native_sample(void *,NpTsensSample *){assert(false);return 0;}
extern "C" int np_adc_native_boot_probe(NpAdcNativeBootProbe *){assert(false);return 0;}
extern "C" int np_adc_native_boot_probe_qtm(NpAdcNativeBootProbe *){assert(false);return 0;}
extern "C" int np_board_read_original_temperature(void *,NpBoardTemperatureReading *){assert(false);return 0;}
extern "C" int np_board_prepare(NpPowerBoard *,const NpBoardResources *resources,NpPowerOps *ops) {
    ++board_prepares;assert(resources->stock_clock.clk_div==1&&resources->stock_clock.t_cycle==1);
    assert(ops->control_mode==(unsigned)(selected_mode>=10?9:selected_mode)&&ops->schema_verified&&ops->country_cn==1);
    assert(ops->begin_connector==beginConnector&&ops->poll_connector==pollConnector&&ops->quiesce_adc==quiesceAdc);
    assert(!ops->adc_boot_probe&&!ops->read_tsens);
    if(selected_mode>=9)assert(ops->stock_support100w_valid&&ops->stock_support100w==1);
    assert(ops->ffc_enabled==(unsigned)(selected_mode>=10?1:selected_ffc));
    assert(ops->stock_current_enabled==(selected_mode==11));
    assert(ops->auth_enabled==(selected_mode==5||selected_mode>=8));return 0;
}
extern "C" int np_power27_bind(const NpPowerOps *ops){++binds;assert(ops->begin_connector&&ops->poll_connector);
    if(selected_mode==11)assert(ops->stock_current_enabled&&ops->input_suspend_enabled&&ops->ffc_enabled&&ops->read_original_temperature);
    return 0;}
extern "C" void nezha_power27_start(void){++starts;}
static void *current_thread(){return nullptr;}
static void thread_terminate(void *){++terminations;}
static void thread_deallocate(void *){}
static int kernel_thread_start(void (*fn)(void *,wait_result_t),void *arg,thread_t *out) {
    ++threads;*out=nullptr;fn(arg,0);return KERN_SUCCESS;
}
#include "bootstrap-production-source.inc"
int main(int argc,char **argv) {
    assert(argc==2);variant=(unsigned)strtoul(argv[1],nullptr,10);assert(variant<14);
    if(variant==1)selected_mode=2;
    else if(variant==2)selected_mode=1;
    else if(variant==3)selected_mode=3;
    else if(variant==4)selected_mode=4;
    else if(variant==5)nezha_touch_stage=-1;
    else if(variant==6)nezha_touch_stage=4;
    else if(variant==8)selected_mode=9;
    else if(variant==9){selected_mode=9;selected_ffc=1;}
    else if(variant==10)selected_ffc=1;
    else if(variant==11){selected_mode=9;selected_ffc=2;}
    else if(variant==12)selected_mode=10;
    else if(variant==13)selected_mode=11;
    nezha_power27_probe();assert(threads==1&&terminations==1&&!adc_calls);
    if(variant<2||variant==8||variant==9||variant==12||variant==13)assert(starts==1&&binds==1&&board_prepares==1&&maps==8);
    else assert(!starts&&!binds&&!board_prepares);
    if((variant>=2&&variant<=6)||variant==10||variant==11)assert(!maps);
    if(variant==6)assert(slept_ms==60000);
    if(variant==7)assert(maps==8);
    printf("PASS actual bootstrap/probe gate%u: mode=%d, starts=%u, maps=%u, readiness sleep=%llu; no ADC conversion/hardware IO\n",
        variant,selected_mode,starts,maps,(unsigned long long)slept_ms);
}
