/* SPDX-License-Identifier: MIT */
/* Actual native capture controller; hardware/KPI calls are fixtures.
 * The test checks lifecycle/data ownership, not Qualcomm register behavior. */
#include "NezhaCameraNativeCapture.h"
#include "NezhaCameraNativeGcc.h"
#include "NezhaCameraNativeIrq.h"
#include "NezhaCameraNativeRpmh.h"
#include "NezhaCameraCanoePins.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

static uint32_t camcc[0x80000/4],crm[0x2000/4],crmc[0x3000/4],crmcommon[0x1000/4],ife[0x1000/4],cpas[0x1000/4],phy[0x2000/4],cci[0x1000/4],csid[0x6000/4],vfe[0x20000/4],gpio[NezhaCameraGpioBytes/4],mclk[0x1000/4];
static unsigned maps, starts, stops, reads, copies;
static bool resources_active, irq_active, smmu_active, memory_active, stream_active, keep_running, source_fail;
static bool frame_timeout;
static bool selected_epd;
static unsigned stream_readbacks;
static NezhaCameraRpmh rpmh{};
static NezhaCameraDmaPlan plan{};
static ArmSmmu4KContextConfig context{};
extern "C" unsigned long ml_io_map(unsigned long physical,unsigned long) {
    ++maps;
    switch (physical) {
    case 0x956d000:return (unsigned long)camcc;case 0x9548000:return (unsigned long)crm;
    case 0x954a000:return (unsigned long)crmc;case 0x954c000:return (unsigned long)crmcommon;
    case 0x9545000:return (unsigned long)ife;case 0x900b000:return (unsigned long)cpas;
    case 0x9523000:return (unsigned long)phy;case 0x941b000:return (unsigned long)cci;
    case 0x9253000:return (unsigned long)csid;case 0x9151000:return (unsigned long)vfe;
    case NezhaCameraGpioPhysical:return (unsigned long)gpio;case 0x1764000:return (unsigned long)mclk;
    default:assert(false);return 0;
    }
}
extern "C" void IOSleep(unsigned) { }
extern "C" void IODelay(unsigned) { }
extern "C" bool nezha_camera_profile_open(const uint8_t *,size_t,NezhaCameraProfile *p)
{ p->vendor=3;p->width=4096;p->height=3072;p->epd_mode=selected_epd;return true; }
extern "C" bool nezha_camera_profile_revision_writes(const NezhaCameraProfile *,uint8_t vendor,uint8_t revision,NezhaCameraOvxModeWrite *,size_t,size_t *count)
{ assert(vendor==3 && revision==0x42);*count=1;return true; }
extern "C" bool nezha_camera_native_gcc(NezhaCameraGccIo *) { return true; }
extern "C" bool nezha_camera_native_rpmh_open(NezhaCameraRpmh **out,const uint8_t *,size_t)
{ assert(!*out && !resources_active);*out=&rpmh;return true; }
extern "C" bool nezha_camera_native_rpmh_close(NezhaCameraRpmh **out)
{ assert(*out==&rpmh && !resources_active);*out=nullptr;return true; }
extern "C" bool nezha_camera_rpmh_acquire(void *,NezhaCameraMainRail,unsigned) { return true; }
extern "C" bool nezha_camera_rpmh_release(void *,NezhaCameraMainRail) { return true; }
extern "C" bool nezha_camera_ife_bandwidth_checked(const uint8_t *,size_t,uint32_t,uint32_t,uint32_t,uint32_t,NezhaCameraIfeBandwidth *,NezhaCameraIfeBcmFailure *) { return true; }
extern "C" bool nezha_camera_capture_resources_start(const NezhaCameraCaptureResourceIo *,const NezhaCameraIfeBandwidth *,const NezhaCameraCsiphy24Profile *,NezhaCameraCaptureResources *r)
{ assert(!resources_active && r->step==NezhaResourcesIdle);r->step=NezhaResourcesReady;resources_active=true;++starts;return !source_fail; }
extern "C" bool nezha_camera_capture_resources_drain(const NezhaCameraCaptureResourceIo *,NezhaCameraCaptureResources *,bool quiet)
{ assert(quiet && !stream_active && resources_active);return true; }
extern "C" bool nezha_camera_capture_resources_stop(const NezhaCameraCaptureResourceIo *,const NezhaCameraIfeBandwidth *,const NezhaCameraCsiphy24Profile *,NezhaCameraCaptureResources *,bool quiet,bool detached)
{ assert(quiet && detached && !memory_active && !smmu_active && !irq_active);resources_active=false;++stops;return true; }
extern "C" bool nezha_camera_main_hold_reset(const NezhaCameraRegisterIo *,NezhaCameraMainPins *p) { p->reset_held=true;return true; }
extern "C" bool nezha_camera_main_start_pins(const NezhaCameraRegisterIo *,const NezhaCameraRegisterIo *,const NezhaCameraMainRailIo *,NezhaCameraMainPins *) { return true; }
extern "C" bool nezha_camera_main_restore_pins(const NezhaCameraRegisterIo *,const NezhaCameraRegisterIo *,const NezhaCameraMainRailIo *,NezhaCameraMainPins *p) { p->reset_held=false;return true; }
extern "C" NezhaCameraCciStatus nezha_camera_cci_recover_master(const NezhaCameraRegisterIo *,unsigned,unsigned,NezhaCameraCciObservation *) { return NezhaCciOk; }
extern "C" NezhaCameraCciStatus nezha_camera_cci_read_byte(const NezhaCameraRegisterIo *,unsigned master,uint8_t slave,uint16_t reg,uint8_t *value,NezhaCameraCciObservation *)
{ assert(stream_active && master==1 && slave==0x10 && reg==0x100);++stream_readbacks;*value=1;return NezhaCciOk; }
extern "C" NezhaCameraCciStatus nezha_camera_ovx10500u_read_identity(const NezhaCameraRegisterIo *,NezhaCameraOvxIdentity *id)
{ id->bytes[0]=0x50;id->bytes[1]=0x58;id->bytes_read=2;return NezhaCciOk; }
extern "C" NezhaCameraCciStatus nezha_camera_ovx10500u_read_revision(const NezhaCameraRegisterIo *,NezhaCameraOvxRevision *r)
{ r->vendor=3;r->version=0x42;return NezhaCciOk; }
extern "C" bool nezha_camera1080_prepare_csid(const NezhaCameraRegisterIo *,uint32_t *r) { *r=1;return true; }
extern "C" bool nezha_camera_native_irq_open(NezhaCameraNativeIrq **out,const NezhaCameraRegisterIo *,const NezhaCameraRegisterIo *,NezhaCameraNativeIrqSetup *)
{ assert(!*out && !irq_active);*out=(NezhaCameraNativeIrq *)1;irq_active=true;return true; }
extern "C" bool nezha_camera_native_irq_stop(NezhaCameraNativeIrq *) { irq_active=false;return true; }
extern "C" uint32_t nezha_camera_native_irq_fault(const NezhaCameraNativeIrq *) { return 0; }
extern "C" bool nezha_camera_native_irq_failure(const NezhaCameraNativeIrq *,NezhaCamera1080Events *) { return false; }
extern "C" bool nezha_camera_native_irq_next(NezhaCameraNativeIrq *,NezhaCamera1080Events *) { return true; }
extern "C" ArmSmmu4KStatus nezha_camera_native_smmu_prepare(NezhaCameraNativeSmmu **out,unsigned,unsigned,unsigned,unsigned *bits)
{ assert(!*out && !smmu_active);*out=(NezhaCameraNativeSmmu *)1;*bits=40;smmu_active=true;return ArmSmmu4KSuccess; }
extern "C" bool nezha_camera_native_memory_prepare(NezhaCameraNativeMemory **out,unsigned,uint32_t,uint32_t)
{ assert(!*out && !memory_active);*out=(NezhaCameraNativeMemory *)1;memory_active=true;return true; }
extern "C" bool nezha_camera_native_memory_hold(NezhaCameraNativeMemory *) { return true; }
extern "C" const NezhaCameraDmaPlan *nezha_camera_native_memory_plan(const NezhaCameraNativeMemory *) { return &plan; }
extern "C" const ArmSmmu4KConfig *nezha_camera_native_memory_config(const NezhaCameraNativeMemory *) { return nullptr; }
extern "C" const void *nezha_camera_native_memory_acquire(NezhaCameraNativeMemory *,unsigned) { return camcc; }
extern "C" bool nezha_camera_native_memory_publish(NezhaCameraNativeMemory *,unsigned) { return true; }
extern "C" bool nezha_camera_native_memory_drop(NezhaCameraNativeMemory *,unsigned) { return true; }
extern "C" bool nezha_camera_native_memory_release(NezhaCameraNativeMemory **out,bool detached)
{ assert(detached && !smmu_active && !stream_active);*out=nullptr;memory_active=false;return true; }
extern "C" ArmSmmu4KStatus nezha_camera_native_smmu_bind(NezhaCameraNativeSmmu *,const NezhaCameraNativeMemory *) { return ArmSmmu4KSuccess; }
extern "C" const ArmSmmu4KIo *nezha_camera_native_smmu_io(const NezhaCameraNativeSmmu *) { return nullptr; }
extern "C" const ArmSmmu4KContextConfig *nezha_camera_native_smmu_context(const NezhaCameraNativeSmmu *) { return &context; }
extern "C" bool nezha_camera_native_smmu_capture_enter(void *) { return true; }
extern "C" void nezha_camera_native_smmu_capture_leave(void *) { }
extern "C" bool nezha_camera_native_smmu_prepare_detach(NezhaCameraNativeSmmu *) { assert(!stream_active && !irq_active);return true; }
extern "C" bool nezha_camera_native_smmu_release(NezhaCameraNativeSmmu **out,bool detached)
{ assert(detached);*out=nullptr;smmu_active=false;return true; }
extern "C" uint32_t nezha_camera_native_smmu_fault(const NezhaCameraNativeSmmu *) { return 0; }
extern "C" void nezha_camera_native_smmu_fault_info(const NezhaCameraNativeSmmu *,NezhaCameraNativeSmmuFault *f) { *f={}; }
extern "C" bool nezha_camera_main_capture_start(const NezhaCameraMainCaptureIo *,const NezhaCameraMainMode *mode,const NezhaCameraDmaPlan *,const ArmSmmu4KContextConfig *,unsigned,unsigned,unsigned,NezhaCameraMainCapture *c)
{ assert(!stream_active && !c->rdi_started && mode->input.epd_mode==selected_epd);c->rdi_started=true;c->stream_may_be_on=true;stream_active=true;return true; }
extern "C" bool nezha_camera_main_capture_request_stop(const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *c) { c->stream_may_be_on=false;stream_active=false;return true; }
extern "C" bool nezha_camera_main_capture_finish_stop(const NezhaCameraMainCaptureIo *,const ArmSmmu4KContextConfig *,unsigned,NezhaCameraMainCapture *,bool quiet) { assert(quiet);return true; }
extern "C" bool nezha_camera_main_capture_release_frame(const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *,unsigned,bool) { return true; }
extern "C" NezhaCameraFrameStatus nezha_camera_frame_probe(const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *,const NezhaCameraRegisterIo *,const NezhaCameraRegisterIo *,const NezhaCameraFrameMemoryIo *,unsigned,unsigned,NezhaCameraFrameResult *)
{ assert(false);return NezhaFramesInvalid; }
extern "C" NezhaCameraFrameStatus nezha_camera_frame_consume(const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *capture,const NezhaCameraRegisterIo *,const NezhaCameraFrameMemoryIo *,const NezhaCameraFrameConsumer *consumer,unsigned budget)
{
    assert(budget==1000 && stream_active);++reads;++capture->completed_frames;
    if (frame_timeout) return NezhaFramesTimeout;
    const NezhaCameraRaw10Layout layout{4096,3072,5120,5120,15728640};
    uint8_t frame=0x5a;
    assert(consumer->deliver(consumer->context,&layout,&frame,layout.frame_bytes,100+capture->completed_frames,capture->completed_frames));
    return NezhaFramesOk;
}
static bool continuing(void *) { return keep_running; }
static bool delivered(void *,const NezhaCameraRaw10Layout *,const uint8_t *,size_t,uint64_t,uint32_t sequence)
{ ++copies;if (sequence==3) nezha_camera_native_capture_request_stop();return true; }
int main()
{
    cci[0]=NezhaCameraCciCanoeVersion;
    uint8_t profile=0,database=0;
    NezhaCameraFrameConsumer consumer{NULL,continuing,delivered};
    NezhaCameraNativeCaptureRequest request{};
    request.profile=&profile;request.profile_bytes=NezhaCameraProfileCapacity;
    request.command_database=&database;request.database_bytes=0x20000;request.cci_queue_words=NezhaCameraCciCanoeQueue0Words;request.consumer=&consumer;
    NezhaCameraNativeCaptureResult result{};
    keep_running=false;
    assert(nezha_camera_native_capture(&request,&result) && result.cleaned && !starts && !maps);
    keep_running=true;
    for (unsigned turn=0;turn<2;++turn) {
        selected_epd=turn!=0;
        assert(nezha_camera_native_capture(&request,&result));
        assert(result.cleaned && result.frames.frames==3 && starts==turn+1 && stops==turn+1);
        assert(!nezha_camera_native_capture_running() && !resources_active && !memory_active && !smmu_active && !irq_active);
        assert(maps==12); /* MMIO callbacks retain permanent mapping; no second-run leak */
    }
    assert(copies==6 && reads==6 && stream_readbacks==2);
    frame_timeout=true;
    csid[0x880/4]=0x1100212;
    csid[0x8c0/4]=27;
    vfe[0x2c00/4]=0x10001;
    assert(!nezha_camera_native_capture(&request,&result) && result.cleaned && stream_readbacks==4);
    assert(result.stream_start_readback==1 && result.stream_start_readback_status==NezhaCciOk);
    assert(result.status==NezhaFramesTimeout && result.stream_readback==1 && result.stream_readback_status==NezhaCciOk);
    assert(result.capture_registers[0]==0x1100212 && result.capture_registers[6]==27 &&
        result.capture_registers[12]==0x10001);
    const unsigned failed_starts=starts;
    assert(!nezha_camera_native_capture(&request,&result) && starts==failed_starts);
    puts("Native continuous camera: two three-frame sessions, callback Stop, permanent maps and ordered resource release passed");
}
