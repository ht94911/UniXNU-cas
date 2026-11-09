// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../platform/NezhaPowerDiagnostic.cpp"
static const qud_diag_provider *registered;
static bool setter_case;
static bool fixed_case;
static int lifecycle_value=1,lifecycle_request_result=1;
static unsigned lifecycle_requests,lifecycle_target;
extern "C" int qcom_usb_diag_register(const qud_diag_provider *p){registered=p;return 1;}
extern "C" int np_power_control_copy(NpPowerControl *p){memset(p,0,sizeof(*p));p->mode=2;p->epoch=p->attachment=UINT64_MAX;
 p->sample.at_ms=UINT64_MAX;p->sample.gauge.current_ma=-2147483647;p->sample.charger.usb_uv=22000000;
 for(unsigned i=0;i<NP_META_COUNT;++i){p->meta[i].value=0xffffffff;p->meta[i].valid=1;p->meta[i].error=-100;p->meta[i].at_ms=UINT64_MAX;}
 for(unsigned i=0;i<NP_PD_ENTRIES;++i)p->caps.entry[i]={22000,22000,10000,3};
 if(setter_case){p->mode=3;p->buck.backend=NP_BUCK_OS4_SETTER;p->meta[NP_META_FCC].value=0;p->meta[NP_META_FCC].valid=0;}
 if(fixed_case){p->mode=4;p->buck.backend=NP_BUCK_OS4_FIXED_PD;p->buck.confirmed_vbus_mv=9000;p->buck.usb_ovp_baseline=15;p->pump.control[2]=0;}
 return 1;}
extern "C" int np_power_log_status(NpPowerLogStatus *p){memset(p,0,sizeof(*p));p->phase=2;strcpy(p->path,"/private/var/log/nezha-power-1234567890abcdef.log");return 1;}
extern "C" int np_power_auth_copy(NpAuthStatus *p){memset(p,0,sizeof(*p));p->epoch=p->attachment=UINT64_MAX;return 1;}
extern "C" int np_power_control_request(unsigned m){assert(m<=1);return 1;}
extern "C" int nezha_power27_lifecycle(void){return lifecycle_value;}
extern "C" int np_power_lifecycle_request(unsigned state){assert(state<=1);++lifecycle_requests;lifecycle_target=state;return lifecycle_request_result;}
extern "C" int np_power_history_copy(uint64_t cursor,unsigned limit,NpPowerHistoryRecord *rows,unsigned *count,uint64_t *next,uint64_t *oldest){
 if(cursor==999)return -1;
 assert(limit<=8);*count=limit;*oldest=1;*next=limit+1;
 for(unsigned i=0;i<limit;++i){memset(&rows[i],0,sizeof(rows[i]));rows[i].id=UINT64_MAX-i;rows[i].at_ms=UINT64_MAX;rows[i].battery_ma=-2147483647;}return 1;}
void IOSleep(unsigned){}
int main(int argc,char **argv){assert(argc==2);np_power_diagnostic_start();assert(registered&&registered->id==9);
 char data[QUD_DIAG_BYTES+16];memset(data,0x5a,sizeof(data));size_t used=0;unsigned trunc=0;
 assert(registered->snapshot(nullptr,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_OK&&!trunc&&used<QUD_DIAG_BYTES);
 assert(strstr(data,"\"lifecycle\":1"));
 assert(strstr(data,"\"primary_gauge_verified\":0")&&strstr(data,"\"adapter_verified\":0"));
 FILE *f=fopen(argv[1],"wb");assert(f);fwrite(data,1,used,f);fclose(f);
 setter_case=true;
 assert(registered->snapshot(nullptr,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_OK&&!trunc);
 assert(strstr(data,"\"requires_register_readback\":false")&&strstr(data,"\"mode\":3")&&strstr(data,"[0,0,-100,"));
 fixed_case=true;
 assert(registered->snapshot(nullptr,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_OK&&!trunc);
 assert(strstr(data,"\"mode\":4")&&strstr(data,"\"confirmed_mv\":9000")&&
        strstr(data,"\"ovp_baseline_raw\":15")&&strstr(data,"\"protection_requires_readback\":true")&&
        strstr(data,"\"requires_register_readback\":false"));
 char fixed_path[1024];assert(snprintf(fixed_path,sizeof(fixed_path),"%s-fixed.json",argv[1])>0);
 f=fopen(fixed_path,"wb");assert(f);fwrite(data,1,used,f);fclose(f);
 for(unsigned i=QUD_DIAG_BYTES;i<sizeof(data);++i)assert(data[i]==0x5a);
 assert(registered->snapshot(nullptr,data,32,&used,&trunc)==QUD_FAILED&&trunc&&used<=32);
 qud_diag_request r={};r.op=QUD_HISTORY;r.arg0=8;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_OK&&!trunc);
 r.cursor=999;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_EXPIRED);
 r={};r.op=QUD_CONFIG;r.arg0=1;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_OK&&strstr(data,"\"effect_confirmed\":false"));
 r.arg0=2;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_OK&&lifecycle_target==0&&
  strstr(data,"\"power_state\":0")&&strstr(data,"\"effect_confirmed\":false"));
 r.arg0=3;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_OK&&lifecycle_target==1);
 lifecycle_request_result=0;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_BUSY);
 lifecycle_request_result=-1;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_FAILED);
 unsigned before=lifecycle_requests;r.arg0=4;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_UNSUPPORTED);
 r.arg0=3;r.arg1=1;assert(registered->submit(nullptr,&r,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_UNSUPPORTED&&lifecycle_requests==before);
 lifecycle_value=-1;assert(registered->snapshot(nullptr,data,QUD_DIAG_BYTES,&used,&trunc)==QUD_OK&&!trunc&&strstr(data,"\"lifecycle\":-1"));
 puts("PASS diagnostic9 bounded JSON/history, queue-only charging and own-worker PM controls");}
