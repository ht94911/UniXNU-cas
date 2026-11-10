// SPDX-License-Identifier: BSD-2-Clause-Patent
// New PACK_TBAT read through the actual runtime; all wire peers are local.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define PE_parse_boot_argn fixture_default_parse_boot
#include "host_stubs/runtime/pexpert/pexpert.h"
#undef PE_parse_boot_argn
int PE_parse_boot_argn(const char *,void *,int);
#include "../platform/NezhaPowerRuntime.cpp"
static int testCase,packFlag=1;
static uint64_t testMs=1000,testEpoch=1;
static unsigned packReads,chargeWrites,oldEpochDiscarded;
static bool linked,bounced;
struct Reply {uint8_t wire[NP_MCA_REQUEST];uint64_t epoch,due;};
static std::vector<Reply> replies;
static std::string logRows;
extern "C" {uint32_t system_inshutdown;}
int PE_parse_boot_argn(const char *name,void *out,int bytes) {
    if(strcmp(name,"nezha-power-pack-tbat"))return 0;
    assert(bytes==(int)sizeof(packFlag));memcpy(out,&packFlag,sizeof(packFlag));return 1;
}
void IOSleep(unsigned ms){testMs+=ms;}
extern "C" IOService *np_battery_service_create(void){static IOService service;return &service;}
extern "C" int np_power_log_start(void){return 0;}
extern "C" int np_power_log_push(const char *row,size_t bytes){assert(bytes&&bytes<1024);logRows.append(row,bytes);return 0;}
extern "C" void np_power_log_stop(void){}
extern "C" int np_power_log_status(NpPowerLogStatus *out){memset(out,0,sizeof(*out));out->phase=2;return 1;}
extern "C" void np_power_diagnostic_start(void){}
extern "C" void qcom_usb_debug_get_status(qcom_usb_debug_status *out){memset(out,0,sizeof(*out));out->phase=QCOM_USB_DEBUG_ACTIVE;}
static uint64_t now(void *){return testMs;}
static uint32_t value(uint32_t property,uint64_t epoch) {
    switch(property) {
    case NP_PACK_TBAT_PROPERTY:return testCase==1?(uint32_t)-2700:testCase==4?(epoch==1?(uint32_t)-100:31000):29000;
    case 0x20001:return 1;case 0x2001:return 3;case 0x20002:return 5000000;case 0x20004:return 490000;
    case 0x2008:return 500;case 0x2007:return 4400;case 0x2003:return 1;case 0x2000c:return 1;
    case 0x2100d:case 0x2100e:return 1;default:return 0;
    }
}
static int send(void *,const uint8_t *wire,uint32_t bytes) {
    assert(bytes==NP_MCA_REQUEST&&mca.pending);
    if(np_le32(wire+8)==NP_MCA_WRITE){++chargeWrites;assert(!"Read-only thermal candidate sent a write");}
    assert(np_le32(wire+8)==NP_MCA_READ);
    const uint32_t property=np_le32(wire+12);
    if(property==NP_PACK_TBAT_PROPERTY){++packReads;if(testCase==3)return 0;}
    Reply reply={};memcpy(reply.wire,wire,bytes);reply.epoch=testEpoch;reply.due=testMs+30;
    replies.push_back(reply);return 0;
}
static int poll(void *) {
    if(!linked){linked=true;np_power27_link(1,testEpoch);}
    for(size_t i=0;i<replies.size();) {
        Reply reply=replies[i];if(reply.due>testMs){++i;continue;}
        replies.erase(replies.begin()+i);
        const uint32_t property=np_le32(reply.wire+12);
        if(testCase==4&&!bounced&&property==NP_PACK_TBAT_PROPERTY) {
            bounced=true;np_power27_link(0,++testEpoch);np_power27_link(1,++testEpoch);
        }
        uint8_t answer[NP_MCA_RESPONSE]={};memcpy(answer,reply.wire,16);
        np_put32(answer+20,np_le32(reply.wire+16));np_put32(answer+24,value(property,reply.epoch));
        if(testCase==2&&property==NP_PACK_TBAT_PROPERTY)np_put32(answer+16,1);
        if(reply.epoch!=testEpoch)++oldEpochDiscarded;
        np_power27_receive(reply.epoch,answer,sizeof(answer));
    }
    return 0;
}
static int gauge_read(void *,uint8_t (*out)[2],uint32_t *valid) {
    const uint16_t words[10]={4000,0xff9c,3032,66,3750,5699,6800,328,90,0};
    for(unsigned i=0;i<10;++i){out[i][0]=(uint8_t)words[i];out[i][1]=(uint8_t)(words[i]>>8);}
    *valid=0x3ff;testMs+=4;return 0;
}
static int pump_read(void *,NpPump *out){memset(out,0,sizeof(*out));out->chip=0x85;out->valid=18;
    out->controls_valid=out->switching_valid=1;out->die_centi_c=3200;return 0;}
static int quiet(void *){return 0;}
int main(int argc,char **argv) {
    assert(argc==3);testCase=atoi(argv[1]);if(testCase==5)packFlag=0;if(testCase==7)packFlag=2;
    NpPowerOps ops={};ops.now_ms=now;ops.poll_link=poll;ops.send_mca=send;ops.read_gauge=gauge_read;
    ops.read_pump=pump_read;ops.quiesce=ops.resume=quiet;ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
    ops.property_online=testCase==6?0x999:0x20001;ops.property_status=0x2001;
    ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
    ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;ops.control_mode=5;
    int result=np_power27_bind(&ops);
    if(testCase==6||testCase==7){assert(result==-1&&!bound&&!packReads&&!chargeWrites);puts("PASS unsupported pack configuration refused before IO");return 0;}
    assert(!result);lifecycle=1;persistentLog=1;
    for(unsigned i=0;i<240;++i){step();testMs+=50;}
    NpPackThermal raw={};assert(np_power_thermal_copy(&raw));
    NpSnapshot os={};assert(np_snapshot_read(&os));
    const uint32_t fields=NP_BIT(NP_EXTERNAL)|NP_BIT(NP_CHARGING)|NP_BIT(NP_USB_VOLTAGE)|NP_BIT(NP_USB_CURRENT);
    assert(os.valid&fields);assert((os.valid&fields)==fields&&!os.charger.fault);
    assert(!chargeWrites&&!control.buck.owned);
    if(testCase==5){assert(!raw.enabled&&!packReads&&logRows.find("T1 ")==std::string::npos);}
    else {
        assert(raw.enabled&&packReads&&raw.generation==os.generation&&raw.epoch==os.charger.epoch);
        if(testCase==2||testCase==3)assert(!raw.valid_raw&&raw.error==(testCase==2?-EIO:-ETIMEDOUT));
        else {
            assert(raw.valid_raw&&!raw.error&&raw.pack_tbat_raw==(testCase==1?-2700:testCase==4?31000:29000));
            if(testCase==4)assert(bounced&&oldEpochDiscarded==1&&raw.epoch==3);
            testMs+=NP_CHARGER_TTL_MS+1;assert(np_power_thermal_copy(&raw)&&!raw.valid_raw&&raw.error==-ETIMEDOUT);
        }
        invalidate(NP_FAULT_STALE);assert(np_power_thermal_copy(&raw)&&!raw.valid_raw);
    }
    FILE *file=fopen(argv[2],"wb");assert(file);assert(fwrite(logRows.data(),1,logRows.size(),file)==logRows.size());fclose(file);
    printf("PASS new pack case%d: reads=%u writes=%u epoch=%llu; raw units remain unqualified\n",testCase,packReads,chargeWrites,(unsigned long long)testEpoch);
    return 0;
}
