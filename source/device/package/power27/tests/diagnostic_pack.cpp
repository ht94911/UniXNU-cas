// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the new raw thermal consumer; unchanged fields use the027 fixture.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "../platform/NezhaPowerDiagnostic.cpp"
static unsigned variant;
extern "C" int qcom_usb_diag_register(const qud_diag_provider *){return 1;}
extern "C" int np_power_control_copy(NpPowerControl *p) {
    memset(p,0,sizeof(*p));p->mode=5;p->epoch=p->attachment=1;p->country_cn=1;
    p->sample.generation=p->sample.charger.epoch=variant==5?UINT64_MAX:1;
    p->buck.backend=NP_BUCK_OS4_STOCK_POLICY;p->buck.state=NP_BUCK_POLICY_MONITOR;
    p->buck.confirmation=NP_BUCK_CONFIRM_CURRENT;p->buck.stock.actual[0]=500;
    p->meta[NP_META_ICL].value=500;p->meta[NP_META_ICL].valid=1;
    auto &v=p->jeita.vote;v.valid=v.full_valid=v.enabled=1;v.fcc_ma=4000;v.fv_mv=4480;v.iterm_ma=328;
    p->buck.stock.actual[1]=4480;p->buck.stock.actual[2]=358;p->buck.stock.actual[3]=1;
    return 1;
}
extern "C" int np_power_thermal_copy(NpPackThermal *p) {
    memset(p,0,sizeof(*p));if(!variant)return 1;if(variant==4)return 0;
    p->enabled=1;p->valid_raw=variant!=2;p->error=variant==2?-5:0;
    p->pack_tbat_raw=variant==5?INT32_MIN:-2700;
    p->epoch=variant==5?UINT64_MAX:variant==3?2:1;
    p->generation=variant==5?UINT64_MAX:1;p->sampled_ms=variant==5?UINT64_MAX:1000;
    return 1;
}
extern "C" int np_power_log_status(NpPowerLogStatus *p){memset(p,0,sizeof(*p));return 1;}
extern "C" int np_power_auth_copy(NpAuthStatus *p){memset(p,0,sizeof(*p));return 1;}
extern "C" int np_power_identity_copy(NpGaugeIdentity *p){memset(p,0,sizeof(*p));return 1;}
extern "C" int np_power_control_request(unsigned){return 1;}
extern "C" int nezha_power27_lifecycle(void){return 1;}
extern "C" int np_power_lifecycle_request(unsigned){return 1;}
extern "C" int np_power_history_copy(uint64_t,unsigned,NpPowerHistoryRecord *,unsigned *,uint64_t *,uint64_t *){return 0;}
void IOSleep(unsigned){}
int main(int argc,char **argv) {
    assert(argc==2);
    for(variant=0;variant<6;++variant) {
        char data[QUD_DIAG_BYTES+16];memset(data,0x5a,sizeof(data));size_t used=0;unsigned truncated=0;
        int result=snapshot(nullptr,data,QUD_DIAG_BYTES,&used,&truncated);
        if(variant==3||variant==4){assert(result==QUD_BUSY);continue;}
        assert(result==QUD_OK&&!truncated&&used<QUD_DIAG_BYTES);
        for(unsigned i=QUD_DIAG_BYTES;i<sizeof(data);++i)assert(data[i]==0x5a);
        if(!variant)assert(!strstr(data,"\"thermal\""));
        else assert(strstr(data,"\"units_confirmed\":false"));
        char path[1024];assert(snprintf(path,sizeof(path),"%s/diag-pack-%u.json",argv[1],variant)>0);
        FILE *file=fopen(path,"wb");assert(file);assert(fwrite(data,1,used,file)==used);fclose(file);
    }
    puts("PASS new thermal JSON: disabled legacy bytes, negative/raw error, coherent generation, busy copy and wide values");
    return 0;
}
