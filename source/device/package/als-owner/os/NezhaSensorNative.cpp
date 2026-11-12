// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board binding to the fresh owner's shared SMMU and GLINK. No extra worker.
#include "NezhaSensorNative.h"
#include <IOKit/IOLib.h>
#include <string.h>
extern "C" const uint8_t nezha_sensor_inputs[NEZHA_SENSOR_BUNDLE_CAPACITY];
namespace {
int fail(NezhaSensorNative *s,int result) {
    if (!s->error) {s->error=result ? result:NA_IO;printf("NEZHA SENSOR: failed rc=%d mapped=%u pdr=%#x listener=%u\n",s->error,s->mapped,s->pdr.state,s->listener.phase);}
    if (s->rpc.glink) qcom_rpc_lost(&s->rpc,QcomRpcLost);
    return s->error;
}
int sync(void *v,uint32_t offset,uint32_t bytes,int device) {
    auto *s=static_cast<NezhaSensorNative *>(v);
    int result=na_native_smmu_faulted(&s->native);if (result) return result;
    return na_native_sync(&s->memory,offset,bytes,device);
}
int room(void *v,uint32_t bytes) {
    auto *a=static_cast<NaAdsp *>(v);
    if (!a->mapper.online || a->mapper.error || bytes>NA_PD_PACKET_BYTES) return QcomPdrLost;
    return a->mapper.head-a->mapper.tail<NA_PD_TX_SLOTS ? 0:QcomPdrAgain;
}
void observe(void *,const char *kind,const char *name,uint32_t method,int result) {
    printf("NEZHA SENSOR RPC: kind=%s method=%u result=%d name=%s\n",kind,method,result,name);
}
int32_t invoke(void *v,uint32_t handle,uint32_t scalar,QcomRpcArg *args,uint32_t count) {
    auto *s=static_cast<NezhaSensorNative *>(v);
    uint32_t word0=0,word1=0;
    if (count && args && args[0].data && args[0].bytes>=4) memcpy(&word0,args[0].data,4);
    if (count && args && args[0].data && args[0].bytes>=8) memcpy(&word1,static_cast<uint8_t *>(args[0].data)+4,4);
    const int32_t result=nezha_sensor_files_invoke(&s->files,handle,scalar,args,count);
    // Fill the missing call identity in the existing observer. No extra trace
    // buffer, transport, file contents or property values are logged.
    printf("NEZHA SENSOR CALL: call=%u handle=%#x scalar=%#x argc=%u bytes=%u/%u/%u/%u words=%#x/%#x result=%d\n",
           s->listener.reverse_calls+1,handle,scalar,count,
           count>0 ? args[0].bytes:0,count>1 ? args[1].bytes:0,
           count>2 ? args[2].bytes:0,count>3 ? args[3].bytes:0,word0,word1,result);
    return result;
}
}
extern "C" int nezha_sensor_native_buffers(NezhaSensorNative *s,NaResourceOwner *owner,NaAdsp *a) {
    if (!s || !owner || !a || s->attempted || a->stage!=NaAdspDma || a->initial_stop_mask!=3 ||
        a->initial_stop[0] || a->initial_stop[1] || !a->masters_stopped) return NA_OWNERSHIP;
    s->attempted=1;s->owner=owner;
    int r=nezha_sensor_bundle_prepare(&s->bundle,nezha_sensor_inputs,NEZHA_SENSOR_BUNDLE_CAPACITY);
    if (r) return fail(s,NA_MISSING);
    NezhaSensorFileOps ops={&s->bundle,nezha_sensor_bundle_file,nezha_sensor_bundle_property,nezha_sensor_bundle_directory,observe};
    r=nezha_sensor_files_prepare(&s->files,&ops);if (r) return fail(s,r);
    r=na_native_allocate(&s->memory,QCOM_RPC_SLOTS*QCOM_RPC_SLOT_BYTES);if (r) return fail(s,r);
    r=na_native_allocate(&s->tables,4*4096);if (r) return fail(s,r);
    if (na_ranges_overlap({s->memory.physical,s->memory.bytes},{s->tables.physical,s->tables.bytes}) ||
        na_ranges_overlap({s->memory.physical,s->memory.bytes},na_nezha_facts.firmware) ||
        na_ranges_overlap({s->memory.physical,s->memory.bytes},na_nezha_facts.dtb) ||
        na_ranges_overlap({s->tables.physical,s->tables.bytes},na_nezha_facts.firmware) ||
        na_ranges_overlap({s->tables.physical,s->tables.bytes},na_nezha_facts.dtb)) return fail(s,NA_OWNERSHIP);
    r=na_native_smmu_bind(&s->native,owner,&s->config);if (r) return fail(s,r);
    const uint16_t sid[]={0x1005,0x1085,0x1045,0x1065};ArmSmmu4KStream streams[4];
    for (unsigned i=0;i<4;++i) {if (owner->assignment.sid[i]!=sid[i]) return fail(s,NA_OWNERSHIP);streams[i]={owner->assignment.slot[i],sid[i]};}
    r=s->config.enter(s->config.config_context,s->config.lease);if (r) return fail(s,r);
    const ArmSmmu4KRange range={0x70000000,s->memory.physical,s->memory.bytes,ArmSmmu4KRead|ArmSmmu4KWrite,1,3};
    s->table_config={32,s->config.output_bits,s->tables.physical,4096};
    r=ArmSmmu4KCheckFaultedStreams(&s->config.io,streams,4,s->config.bank,&s->stream_result);
    if (!r) r=ArmSmmu4KBuild(&s->table_config,&range,1,reinterpret_cast<uint64_t *>(s->tables.va),4,&s->table_result);
    if (!r) r=na_native_clean(nullptr,s->tables.va,s->tables.physical,s->table_result.table_pages*4096);
    s->context={};s->context.lower=&s->table_config;s->context.lower_tables=&s->table_result;
    s->context.asid=s->config.asid;s->context.coherent_walk=1;
    if (!r) r=ArmSmmu4KProgramDisabledContext(&s->config.io,s->config.bank,&s->context,10000,&s->program_result);
    s->context.id0=s->program_result.id0;s->context.id2=s->program_result.id2;
    if (!r) r=ArmSmmu4KAttachStreams(&s->config.io,&s->streams,streams,4,s->config.bank,&s->context,&s->stream_result);
    s->config.leave(s->config.config_context);if (r) return fail(s,NA_IO);
    s->mapped=1;
    printf("NEZHA SENSOR: mapped bank=%u sid=5 iova=0x70000000 bytes=%llu files=%u\n",s->config.bank,(unsigned long long)s->memory.bytes,s->bundle.count);
    return NA_OK;
}
extern "C" int nezha_sensor_native_link(NezhaSensorNative *s,NaAdsp *a) {
    if (!s || !a || s->linked || s->error || !s->mapped || !a->glink_initialized) return NA_OWNERSHIP;
    uint32_t port=0;
    for (uint32_t candidate=0x4000;candidate<0x4004;++candidate) {
        bool used=false;for (uint32_t i=0;i<a->mapper.qrtr.local_count;++i) if (a->mapper.qrtr.local[i].port==candidate) used=true;
        if (!used) {port=candidate;break;}
    }
    if (!port) return fail(s,NA_MISSING);
    QcomPdrOps pdr={a,room};int r=qcom_pdr_prepare(&s->pdr,&a->mapper.qrtr,port,"msm/adsp/sensor_pd",74,&pdr);
    QcomRpcOps rpc={s,sync};
    /* One actual fresh FastRPC session, unique within this owned link. The
     * Android driver also allocates its tgid_frpc instead of copying a PID. */
    if (!r) r=qcom_rpc_prepare(&s->rpc,&a->glink,s->memory.va,(uint32_t)s->memory.bytes,UINT64_C(0x0500000070000000),1,1,&rpc);
    QcomRpcProvider provider={s,invoke};
    if (!r) r=qcom_listener_prepare(&s->listener,&s->rpc,&provider);
    if (r) return fail(s,r);
    s->linked=1;printf("NEZHA SENSOR: registered FastRPC and PDR local=%u epoch=fresh-adsp\n",port);return 0;
}
extern "C" void nezha_sensor_native_poll(NezhaSensorNative *s,NaAdsp *a,uint64_t now) {
    if (!s || !a || !s->linked || s->error) return;
    if (a->stage!=NaAdspOnline || !a->mapper.online || a->error || a->mapper.error || a->glink.broken) {
        if (s->pdr.selected || s->rpc.selected) (void)fail(s,NA_REMOTE);
        return;
    }
    int r=na_native_smmu_faulted(&s->native);if (r) {(void)fail(s,r);return;}
    r=qcom_pdr_poll(&s->pdr,now);if (r<0) {(void)fail(s,r);return;}
    if (s->pdr.revision!=s->last_pdr_revision) {
        s->last_pdr_revision=s->pdr.revision;
        printf("NEZHA SENSOR PDR: state=%#x registered=%u revision=%u endpoint=%u/%u\n",s->pdr.state,s->pdr.registered,s->pdr.revision,s->pdr.node,s->pdr.port);
    }
    r=qcom_listener_poll(&s->listener,now,qcom_pdr_is_up(&s->pdr));if (r<0) {(void)fail(s,r);return;}
    if (s->listener.phase!=s->last_listener_phase || s->listener.reverse_calls!=s->last_reverse_calls) {
        s->last_listener_phase=s->listener.phase;s->last_reverse_calls=s->listener.reverse_calls;
        printf("NEZHA SENSOR LISTENER: phase=%u control=%u next=%u calls=%u sent=%u replies=%u error=%d\n",s->listener.phase,s->listener.control_token,s->listener.listener_token,s->listener.reverse_calls,s->rpc.sent,s->rpc.received,s->listener.error);
    }
}
extern "C" int nezha_sensor_native_state(const NezhaSensorNative *s,uint32_t out[12]) {
    if (!s || !out) return NA_ARGUMENT;
    const uint32_t values[]={s->mapped,s->pdr.state,s->pdr.registered,s->pdr.revision,s->listener.phase,s->rpc.sent,s->rpc.received,s->listener.reverse_calls,(uint32_t)s->error,s->rpc.stale,s->rpc.early,s->files.requests};
    memcpy(out,values,sizeof values);return 0;
}
