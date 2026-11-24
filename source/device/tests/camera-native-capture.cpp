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
#include <cstring>

static uint32_t camcc[0x80000/4],crm[0x2000/4],crmc[0x3000/4],crmcommon[0x1000/4],ife[0x1000/4],cpas[0x1000/4],phy[0x2000/4],cci[0x1000/4],csid[0x6000/4],vfe[0x20000/4],gpio[NezhaCameraGpioBytes/4],mclk[0x1000/4];
static unsigned maps, starts, stops, reads, copies;
static bool resources_active, irq_active, smmu_active, memory_active, stream_active, keep_running, source_fail;
static bool frame_timeout;
static bool drain_failure;
static bool csid_stop_failure, irq_stop_failure, csid_stop_done;
static bool irq_retire_failure;
static unsigned irq_retire_calls;
static unsigned csid_stop_calls, drain_calls;
static uint32_t irq_fault;
static bool selected_epd;
static unsigned stream_readbacks;
static unsigned control_takes, control_commits, control_completions;
static NezhaCameraCciStatus control_failure=NezhaCciOk;
static bool control_pending, stop_at_control;
static unsigned control_readbacks;
static bool readback_mismatch;
static bool longer_control_session;
static bool delayed_controls,waiting_controls;
static bool waiting_control_write;
static bool stop_wait_controls,fault_wait_controls;
static NezhaCameraCciStatus initial_controls_failure;
static unsigned standby_readbacks, stream_control_readbacks;
static unsigned control_pending_frames,control_poll_frames,control_waits;
static unsigned AF_eeprom_calls,AF_position_calls;
static bool memory_prepare_failure,memory_hold_failure;
static bool copy_service_test,copy_service_fault,copy_service_stop;
static unsigned copy_service_tests;
static NezhaCameraCciStatus AF_eeprom_failure=NezhaCciOk,AF_position_failure=NezhaCciOk;
extern "C" NezhaCameraCciStatus nezha_camera_eeprom_read_af(const NezhaCameraRegisterIo *io,NezhaCameraEepromAfRead *r)
{
 assert(io && resources_active && !stream_active && !memory_active && !smmu_active && !irq_active);
 ++AF_eeprom_calls;*r={};r->bytes_read=68;r->AF_checksum_valid=AF_eeprom_failure==NezhaCciOk;
 r->infinity_dac=512;r->macro_dac=3640;r->transfer.needs_recovery=AF_eeprom_failure!=NezhaCciOk;
 return AF_eeprom_failure;
}
extern "C" NezhaCameraCciStatus nezha_camera_ak7316_read_position(const NezhaCameraRegisterIo *io,NezhaCameraAk7316Result *r)
{
 assert(io && resources_active && !stream_active && !memory_active && !smmu_active && !irq_active);
 ++AF_position_calls;*r={};r->position_read=AF_position_failure==NezhaCciOk;
 r->position_dac=1350;r->register_value=0x5460;r->transfer.needs_recovery=AF_position_failure!=NezhaCciOk;
 return AF_position_failure;
}
extern "C" NezhaCameraCciStatus nezha_camera_ovx10500u_read_controls(
 const NezhaCameraRegisterIo *io,NezhaCameraOvxControlReadback *r)
{
 assert(io && smmu_active && irq_active);++control_readbacks;
 if (!stream_active) {
  ++standby_readbacks;
  if (initial_controls_failure!=NezhaCciOk) {
   *r={};r->bytes_read=3;r->transfer.needs_recovery=true;
   return initial_controls_failure;
  }
 } else ++stream_control_readbacks;
 if(delayed_controls && stream_active) {
  waiting_controls=true;control_pending_frames=2;
  io->wait_ms(io->context,1);++control_waits;
  assert(control_pending_frames==0 || fault_wait_controls);
  waiting_controls=false;
 }
 *r={};r->bytes_read=11;r->exposure_lines=readback_mismatch && stream_active ? 321:320;
 r->frame_lines=4000;r->analog_gain_q8=544;r->digital_gain_q10=1024;return NezhaCciOk;
}
static void controls_readback(void *,const NezhaCameraOvxControlReadback *r,uint32_t after)
{ assert(after>=1 && r->bytes_read==11); }
extern "C" NezhaCameraCciStatus nezha_camera_ovx10500u_apply_controls(
    const NezhaCameraRegisterIo *bus,const NezhaCameraOvxControls *c,NezhaCameraOvxControlResult *r)
{
 assert(bus && stream_active && memory_active && smmu_active && irq_active);
 assert(c->exposure_lines==320 && c->gain_q8==544 && c->frame_lines==4000);
 if(delayed_controls) {
  waiting_controls=waiting_control_write=true;control_pending_frames=2;
  bus->wait_ms(bus->context,1);++control_waits;
  assert(control_pending_frames==0 || stop_wait_controls || fault_wait_controls);
  waiting_controls=waiting_control_write=false;
 }
 ++control_commits;r->plan.exposure_lines=320;r->plan.frame_lines=4000;r->plan.analog_gain_q8=544;r->plan.digital_gain_q10=1024;r->applied_writes=control_failure==NezhaCciOk ? 14:0;
 r->transfer.needs_recovery=control_failure!=NezhaCciOk;
 r->group_may_be_held=control_failure!=NezhaCciOk;
 return control_failure;
}
static bool take_controls(void *,NezhaCameraOvxControls *out)
{
 ++control_takes;
 if (!control_pending) return false;
 control_pending=false;*out={320,4000,544};return true;
}
static void complete_controls(void *,NezhaCameraCciStatus status,
 const NezhaCameraOvxControlResult *r,uint32_t before)
{
 ++control_completions;assert((delayed_controls ? before>=1:before==1) && status==control_failure);
 assert(r->applied_writes==(status==NezhaCciOk ? 14:0));
 if(stop_at_control)nezha_camera_native_capture_request_stop();
}
static uint32_t original_bus_cgc;
extern "C" uint64_t mach_absolute_time(void)
{
    static uint64_t ticks;
    return ++ticks;
}
static NezhaCameraRpmh rpmh{};
static NezhaCameraDmaPlan plan{};
static ArmSmmu4KContextConfig context{};
static uint32_t display[2][0x1000 / 4]{};
extern "C" unsigned long ml_io_map(unsigned long physical,unsigned long) {
    ++maps;
    switch (physical) {
    case 0x956d000:return (unsigned long)camcc;case 0x9548000:return (unsigned long)crm;
    case 0x954a000:return (unsigned long)crmc;case 0x954c000:return (unsigned long)crmcommon;
    case 0x9545000:return (unsigned long)ife;case 0x900b000:return (unsigned long)cpas;
    case 0x9523000:return (unsigned long)phy;case 0x941b000:return (unsigned long)cci;
    case 0x9253000:return (unsigned long)csid;case 0x9151000:return (unsigned long)vfe;
    case NezhaCameraGpioPhysical:return (unsigned long)gpio;case 0x1764000:return (unsigned long)mclk;
    case 0x09800000:return (unsigned long)display[0];case 0x09ac0000:return (unsigned long)display[1];
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
extern "C" bool nezha_camera_capture_resources_drain(const NezhaCameraCaptureResourceIo *,NezhaCameraCaptureResources *r,bool quiet)
{ assert(quiet && !stream_active && !irq_active && resources_active); ++drain_calls;
  assert(!memory_active || !smmu_active || csid_stop_done || initial_controls_failure!=NezhaCciOk || memory_prepare_failure || memory_hold_failure);
  if(drain_failure) {r->cleanup_status=NezhaCameraCpasTimeout;r->detail=r->cpas.failed_offset=0xf0;
    r->cpas.failed_control=0;r->cpas.failed_status=1;r->cpas.failed_expected=0;return false;}
  return true; }
extern "C" bool nezha_camera_capture_resources_stop(const NezhaCameraCaptureResourceIo *,const NezhaCameraIfeBandwidth *,const NezhaCameraCsiphy24Profile *,NezhaCameraCaptureResources *,bool quiet,bool detached)
{ assert(quiet && detached && !memory_active && !smmu_active && !irq_active);
  assert(vfe[0x1008/4]==original_bus_cgc);resources_active=false;++stops;return true; }
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
extern "C" bool nezha_camera1080_reset_stopped_paths(const NezhaCameraRegisterIo *io,uint32_t *r)
{ assert(io && !irq_active && !stream_active && resources_active && memory_active && smmu_active);
  ++csid_stop_calls;*r=csid_stop_failure ? 0x10000:0x10001;
  return csid_stop_done=!csid_stop_failure; }
extern "C" bool nezha_camera_native_irq_open(NezhaCameraNativeIrq **out,const NezhaCameraRegisterIo *,const NezhaCameraRegisterIo *,NezhaCameraNativeIrqSetup *)
{ assert(!*out && !irq_active);*out=(NezhaCameraNativeIrq *)1;irq_active=true;return true; }
extern "C" bool nezha_camera_native_irq_retire(NezhaCameraNativeIrq **out)
{ assert(*out==(NezhaCameraNativeIrq *)1 && !irq_active && !stream_active &&
    !resources_active && !memory_active && !smmu_active);
  ++irq_retire_calls;if(irq_retire_failure)return false;*out=nullptr;return true; }
extern "C" bool nezha_camera_native_irq_stop(NezhaCameraNativeIrq *) { if(irq_stop_failure)return false;irq_active=false;return true; }
extern "C" uint32_t nezha_camera_native_irq_fault(const NezhaCameraNativeIrq *) { assert(!irq_active);return irq_fault; }
extern "C" bool nezha_camera_native_irq_failure(const NezhaCameraNativeIrq *,NezhaCamera1080Events *event)
{ assert(!irq_active);if(!irq_fault)return false;*event={};event->rx_irq=1u<<12;return true; }
extern "C" bool nezha_camera_native_irq_next(NezhaCameraNativeIrq *,NezhaCamera1080Events *) { return true; }
extern "C" ArmSmmu4KStatus nezha_camera_native_smmu_prepare(NezhaCameraNativeSmmu **out,unsigned,unsigned,unsigned,unsigned *bits)
{ assert(!*out && !smmu_active);*out=(NezhaCameraNativeSmmu *)1;*bits=40;smmu_active=true;return ArmSmmu4KSuccess; }
extern "C" bool nezha_camera_native_memory_prepare(NezhaCameraNativeMemory **out,unsigned,uint32_t,uint32_t)
{ assert(!*out && !memory_active);*out=(NezhaCameraNativeMemory *)1;memory_active=true;return !memory_prepare_failure; }
extern "C" bool nezha_camera_native_memory_hold(NezhaCameraNativeMemory *) { return !memory_hold_failure; }
extern "C" void nezha_camera_native_memory_failure(NezhaCameraNativeMemoryFailure *out)
{
 *out={};
 if(memory_prepare_failure || memory_hold_failure) {
  out->step=memory_prepare_failure ? NezhaMemoryFrameAllocate:NezhaMemoryHold;
  out->object=memory_prepare_failure ? 1:2;
  out->status=0xe00002bd;
  out->requested_bytes=15728640;
 }
}
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
extern "C" bool nezha_camera_main_capture_start(const NezhaCameraMainCaptureIo *io,const NezhaCameraMainMode *mode,const NezhaCameraDmaPlan *,const ArmSmmu4KContextConfig *,unsigned,unsigned,unsigned,NezhaCameraMainCapture *c)
{ assert(!stream_active && !c->rdi_started && mode->input.epd_mode==selected_epd);
  csid_stop_done=false;
  assert(vfe[0x1008/4]==(original_bus_cgc|0x07ffffff));
  if(io->standby_controls) {
   auto *initial=io->standby_controls;initial->attempted=true;
   initial->status=nezha_camera_ovx10500u_read_controls(io->cci,&initial->sample);
   c->sensor_status=initial->status;c->sensor_transfer=initial->sample.transfer;
   if(initial->status!=NezhaCciOk)return false;
  }
  c->rdi_started=true;c->stream_may_be_on=true;stream_active=true;return true; }
extern "C" bool nezha_camera_main_capture_request_stop(const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *c) { c->stream_may_be_on=false;c->rdi_started=false;stream_active=false;return true; }
extern "C" bool nezha_camera_main_capture_finish_stop(const NezhaCameraMainCaptureIo *,const ArmSmmu4KContextConfig *,unsigned,NezhaCameraMainCapture *,bool quiet) { assert(quiet);return true; }
extern "C" bool nezha_camera_main_capture_release_frame(const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *,unsigned,bool) { return true; }
extern "C" NezhaCameraFrameStatus nezha_camera_frame_probe(const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *,const NezhaCameraRegisterIo *,const NezhaCameraRegisterIo *,const NezhaCameraFrameMemoryIo *,unsigned,unsigned,NezhaCameraFrameResult *)
{ assert(false);return NezhaFramesInvalid; }
extern "C" NezhaCameraFrameStatus nezha_camera_frame_consume(const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *capture,const NezhaCameraRegisterIo *,const NezhaCameraFrameMemoryIo *,const NezhaCameraFrameConsumer *consumer,unsigned budget)
{
    assert((budget==1000 || budget==0) && stream_active);
    if(!budget) {
        if(!control_pending_frames)return NezhaFramesOk;
        if(fault_wait_controls)return NezhaFramesIo;
        --control_pending_frames;++control_poll_frames;
    }
    ++reads;++capture->completed_frames;
    if(drain_failure) {irq_fault=2;return NezhaFramesIo;}
    if (frame_timeout) return NezhaFramesTimeout;
    const NezhaCameraRaw10Layout layout{4096,3072,5120,5120,15728640};
    uint8_t frame=0x5a;
    capture->cpu_frames=1;
    const bool accepted=consumer->deliver(consumer->context,&layout,&frame,layout.frame_bytes,
        100+capture->completed_frames,capture->completed_frames);
    capture->cpu_frames=0;
    if(!copy_service_test)assert(accepted);
    return accepted?NezhaFramesOk:NezhaFramesIo;
}
extern "C" NezhaCameraFrameStatus nezha_camera_frame_recycle_during_copy(
 const NezhaCameraMainCaptureIo *,NezhaCameraMainCapture *capture,const NezhaCameraFrameMemoryIo *memory,
 const NezhaCameraFrameConsumer *consumer,unsigned held,unsigned *frames)
{
 assert(copy_service_test && held==0 && frames && capture->cpu_frames==1 && memory->next_event && !consumer->deliver);
 ++copy_service_tests;*frames=0;
 if(copy_service_fault)return NezhaFramesSequence;
 if(copy_service_stop)nezha_camera_native_capture_request_stop();
 else {*frames=2;capture->completed_frames+=2;}
 return NezhaFramesOk;
}
static bool continuing(void *) { return keep_running; }
static bool delivered(void *,const NezhaCameraRaw10Layout *,const uint8_t *frame,size_t,uint64_t,uint32_t sequence)
{
 ++copies;
 if(copy_service_test) {
  assert(!nezha_camera_native_capture_service_copy(frame+1));
  const bool serviced=nezha_camera_native_capture_service_copy(frame);
  assert(serviced==(!copy_service_fault && !copy_service_stop));
  if(serviced)nezha_camera_native_capture_request_stop();
  return serviced || copy_service_stop;
 }
 if(delayed_controls) {
  if((stop_wait_controls && waiting_control_write) || (!waiting_controls && sequence>=10))
   nezha_camera_native_capture_request_stop();
 } else if(sequence==(longer_control_session ? 4u:3u))nezha_camera_native_capture_request_stop();
 return true;
}
int main(int argc,char **argv)
{
    const bool test_mismatch=argc==2 && !strcmp(argv[1],"controls-mismatch");
    const bool test_controls=test_mismatch || (argc==2 && !strcmp(argv[1],"controls"));
    if(argc==2 && (!strcmp(argv[1],"control-wait-frames") ||
       !strcmp(argv[1],"control-wait-stop") || !strcmp(argv[1],"control-wait-fault"))) {
        keep_running=true;delayed_controls=true;control_pending=true;
        stop_wait_controls=!strcmp(argv[1],"control-wait-stop");
        fault_wait_controls=!strcmp(argv[1],"control-wait-fault");
        cci[0]=NezhaCameraCciCanoeVersion;uint8_t profile=0,database=0;
        NezhaCameraFrameConsumer consumer{NULL,continuing,delivered};
        NezhaCameraNativeControlConsumer controls{NULL,take_controls,complete_controls,controls_readback};
        NezhaCameraNativeCaptureRequest request{};
        request.profile=&profile;request.profile_bytes=NezhaCameraProfileCapacity;
        request.command_database=&database;request.database_bytes=0x20000;
        request.cci_queue_words=NezhaCameraCciCanoeQueue0Words;request.consumer=&consumer;request.controls=&controls;
        NezhaCameraNativeCaptureResult result{};
        const bool okay=nezha_camera_native_capture(&request,&result);
        assert(result.cleaned && control_waits>=1);
        assert(standby_readbacks==1 && result.control_readback_before_stream);
        if(fault_wait_controls)assert(!okay && result.status==NezhaFramesIo && control_commits==1);
        else if(stop_wait_controls)assert(okay && result.control_serviced_frames>=1 && control_completions==1);
        else assert(okay && control_commits==1 && control_completions==1 && control_poll_frames>=4 &&
            stream_control_readbacks==1 && result.control_serviced_frames==control_poll_frames &&
            result.control_readback_after>=6);
        puts("Native CCI waits service queued RAW frames, keep ACK/readback separate, and honor Stop/first frame error");
        return 0;
    }
    if(argc==2 && (!strcmp(argv[1],"AF-eeprom-fail") || !strcmp(argv[1],"AF-position-fail") || !strcmp(argv[1],"AF-readonly"))) {
        keep_running=true;cci[0]=NezhaCameraCciCanoeVersion;
        uint8_t profile=0,database=0;
        NezhaCameraFrameConsumer consumer{NULL,continuing,delivered};
        NezhaCameraNativeCaptureRequest request{};
        request.profile=&profile;request.profile_bytes=NezhaCameraProfileCapacity;
        request.command_database=&database;request.database_bytes=0x20000;
        request.cci_queue_words=NezhaCameraCciCanoeQueue0Words;request.consumer=&consumer;request.AF_readonly_probe=true;
        if(!strcmp(argv[1],"AF-eeprom-fail")) AF_eeprom_failure=NezhaCciNack;
        if(!strcmp(argv[1],"AF-position-fail")) AF_position_failure=NezhaCciTimeout;
        NezhaCameraNativeCaptureResult result{};
        const bool success=AF_eeprom_failure==NezhaCciOk && AF_position_failure==NezhaCciOk;
        assert(nezha_camera_native_capture(&request,&result)==success);
        assert(result.cleaned && result.AF_readonly_attempted && AF_eeprom_calls==1);
        assert(AF_position_calls==(AF_eeprom_failure==NezhaCciOk ? 1u:0u));
        if(!success) {
            const NezhaCameraNativeCaptureResult first=result;
            assert(nezha_camera_native_capture_can_reopen());
            AF_eeprom_failure=AF_position_failure=NezhaCciOk;keep_running=true;
            assert(nezha_camera_native_capture(&request,&result) && result.frames.frames==3);
            assert(!first.passed && first.cleaned);
        }
        puts("Native AF read-only phase runs before DMA; first error retained and fully cleaned new reservation succeeds");
        return 0;
    }
    cci[0]=NezhaCameraCciCanoeVersion;
    uint8_t profile=0,database=0;
    NezhaCameraFrameConsumer consumer{NULL,continuing,delivered};
    NezhaCameraNativeCaptureRequest request{};
    request.profile=&profile;request.profile_bytes=NezhaCameraProfileCapacity;
    request.command_database=&database;request.database_bytes=0x20000;request.cci_queue_words=NezhaCameraCciCanoeQueue0Words;request.consumer=&consumer;
    NezhaCameraNativeCaptureResult result{};
    if(argc==2 && (!strcmp(argv[1],"csid-stop-timeout") || !strcmp(argv[1],"irq-stop-timeout"))) {
        keep_running=true;
        csid_stop_failure=!strcmp(argv[1],"csid-stop-timeout");
        irq_stop_failure=!strcmp(argv[1],"irq-stop-timeout");
        assert(!nezha_camera_native_capture(&request,&result) && !result.cleaned);
        assert(result.cleanup_step==(csid_stop_failure ? NezhaNativeStopCsid:NezhaNativeStopIrq));
        assert(result.csid_stop_attempted==csid_stop_failure && !result.csid_stop_acknowledged);
        assert(csid_stop_calls==(csid_stop_failure ? 1u:0u) && drain_calls==0);
        assert(!stream_active && resources_active && smmu_active && memory_active && !stops);
        assert(result.irq_stopped==csid_stop_failure && irq_active==irq_stop_failure);
        assert(!nezha_camera_native_capture_can_reopen() && !irq_retire_calls);
        const unsigned first_starts=starts;
        assert(!nezha_camera_native_capture(&request,&result) && starts==first_starts);
        puts("Native stop timeout retains DMA/backing/resource owners and forbids drain or restart");
        return 0;
    }
    if(argc==2 && (!strcmp(argv[1],"fault-reopen") || !strcmp(argv[1],"irq-retire-failure"))) {
        keep_running=true;frame_timeout=true;irq_fault=2;
        irq_retire_failure=!strcmp(argv[1],"irq-retire-failure");
        assert(!nezha_camera_native_capture(&request,&result) && result.cleaned);
        assert(result.irq_fault==2 && result.status==NezhaFramesTimeout && irq_retire_calls==1);
        assert(!resources_active && !stream_active && !memory_active && !smmu_active && !irq_active);
        const NezhaCameraNativeCaptureResult first=result;
        if(irq_retire_failure) {
            assert(!nezha_camera_native_capture_can_reopen());
            const unsigned before=starts;
            assert(!nezha_camera_native_capture(&request,&result) && starts==before);
        } else {
            assert(nezha_camera_native_capture_can_reopen());
            frame_timeout=false;irq_fault=0;keep_running=true;
            assert(nezha_camera_native_capture(&request,&result) && result.cleaned && result.frames.frames==3);
            assert(starts==2 && stops==2 && maps==14 && irq_retire_calls==2);
            assert(nezha_camera_native_capture_can_reopen());
        }
        assert(!first.passed && first.cleaned && first.irq_fault==2 && first.status==NezhaFramesTimeout);
        puts("Native failed stream remains failed; only fully released owners and retired IRQ permit a new capture");
        return 0;
    }
    if(argc==2 && (!strcmp(argv[1],"copy-service") || !strcmp(argv[1],"copy-service-fault") ||
        !strcmp(argv[1],"copy-service-stop"))) {
        keep_running=true;copy_service_test=true;
        copy_service_fault=!strcmp(argv[1],"copy-service-fault");
        copy_service_stop=!strcmp(argv[1],"copy-service-stop");
        NezhaCameraNativeControlConsumer control{NULL,take_controls,complete_controls,controls_readback};
        request.controls=&control;
        uint8_t unrelated=0;
        assert(!nezha_camera_native_capture_service_copy(&unrelated));
        assert(nezha_camera_native_capture(&request,&result)==!copy_service_fault && result.cleaned);
        assert(copy_service_tests==1 && result.work.copy_service_calls==1 && copies==1);
        assert(result.work.copy_recycled_frames==(!copy_service_fault && !copy_service_stop?2u:0u));
        assert(!nezha_camera_native_capture_service_copy(&unrelated) && !control_commits);
        if(copy_service_fault)assert(result.status==NezhaFramesSequence);
        else assert(result.status==NezhaFramesOk && result.frames.frames==(copy_service_stop?1u:3u));
        puts("Native copy service: sole synchronous source, no recursive delivery or CCI, exact frame counts, first fault and Stop retained");
        return 0;
    }
    if(argc==2 && (!strcmp(argv[1],"initial-controls") || !strcmp(argv[1],"initial-controls-nack") ||
        !strcmp(argv[1],"initial-controls-timeout"))) {
        keep_running=true;
        NezhaCameraNativeControlConsumer control{NULL,take_controls,complete_controls,controls_readback};
        request.controls=&control;
        if(!strcmp(argv[1],"initial-controls-nack"))initial_controls_failure=NezhaCciNack;
        if(!strcmp(argv[1],"initial-controls-timeout"))initial_controls_failure=NezhaCciTimeout;
        const bool success=initial_controls_failure==NezhaCciOk;
        assert(nezha_camera_native_capture(&request,&result)==success && result.cleaned);
        assert(standby_readbacks==1 && !stream_control_readbacks && result.control_readback_before_stream);
        assert(!control_commits && !control_completions);
        if(success)assert(result.frames.frames==3 && result.control_readback_verified && result.control_readback_after==1);
        else {
            assert(!result.frames.frames && !copies && !stream_readbacks && !result.control_readback_verified);
            assert(result.control_readback_status==initial_controls_failure && result.control_readback.bytes_read==3);
        }
        puts("Native initial controls use real standby bytes, qualify after the first frame, and stop partial failures before streaming");
        return 0;
    }
    if(argc==2 && (!strcmp(argv[1],"memory-prepare-failure") || !strcmp(argv[1],"memory-hold-failure"))) {
        keep_running=true;
        memory_prepare_failure=!strcmp(argv[1],"memory-prepare-failure");
        memory_hold_failure=!strcmp(argv[1],"memory-hold-failure");
        assert(!nezha_camera_native_capture(&request,&result));
        assert(result.step==NezhaNativeMemory && result.status==0xe00002bd);
        assert(result.memory_failure.step==(memory_prepare_failure ? NezhaMemoryFrameAllocate:NezhaMemoryHold));
        assert(result.cleaned && !result.frames.frames && !stream_active && !memory_active && !smmu_active);
        assert(nezha_camera_native_capture_can_reopen());
        memory_prepare_failure=memory_hold_failure=false;keep_running=true;
        assert(nezha_camera_native_capture(&request,&result) && result.frames.frames==3 && result.cleaned);
        puts("Native memory failure retains actual adapter status; complete pre-DMA teardown permits a fresh capture");
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"drain-failure")) {
        keep_running=true;drain_failure=true;
        assert(!nezha_camera_native_capture(&request,&result));
        assert(result.status==NezhaFramesIo && !result.cleaned && result.cleanup_step==NezhaNativeDrain);
        assert(result.irq_stopped && result.irq_fault==2 && result.irq_failure.rx_irq==(1u<<12));
        assert(result.resources.cleanup_status==NezhaCameraCpasTimeout && result.resources.detail==0xf0);
        assert(resources_active && memory_active && smmu_active && !irq_active && !stream_active && !stops);
        const unsigned before=starts;
        assert(!nezha_camera_native_capture(&request,&result) && starts==before);
        assert(!nezha_camera_native_capture_can_reopen() && !irq_retire_calls);
        puts("Native failed CPAS drain retains DMA/resources and observes the quiesced original IRQ fault without reopening");
        return 0;
    }
    keep_running=false;
    assert(nezha_camera_native_capture(&request,&result) && result.cleaned && !starts && !maps);
    keep_running=true;
    for (unsigned turn=0;turn<2;++turn) {
        selected_epd=turn!=0;
        original_bus_cgc=turn ? 0x88000400 : 0x40000000;
        vfe[0x1008/4]=original_bus_cgc;
        assert(nezha_camera_native_capture(&request,&result));
        assert(result.cleaned && result.frames.frames==3 && starts==turn+1 && stops==turn+1);
        assert(!nezha_camera_native_capture_running() && !resources_active && !memory_active && !smmu_active && !irq_active);
        assert(vfe[0x1008/4]==original_bus_cgc);
        assert(maps==14); /* Includes two retained read-only display observers; no second-run leak. */
    }
    assert(copies==6 && reads==6 && stream_readbacks==2);
    if(test_controls) {
    NezhaCameraNativeControlConsumer control{NULL,take_controls,complete_controls,controls_readback};
    request.controls=&control;
    const unsigned before_controls=starts;
    control_pending=true;
    assert(nezha_camera_native_capture(&request,&result));
    assert(result.control_attempted && result.control_status==NezhaCciOk &&
        result.control_completed_before==1 && result.control_result.applied_writes==14);
    assert(control_takes==1 && control_commits==1 && control_completions==1 &&
        result.frames.frames==3 && starts==before_controls+1);
    control_pending=true;stop_at_control=true;
    assert(nezha_camera_native_capture(&request,&result) && result.frames.frames==1);
    assert(control_commits==2 && control_completions==2 && result.cleaned);
    stop_at_control=false;control_pending=false;
    control_takes=0;
    assert(nezha_camera_native_capture(&request,&result) && !result.control_attempted &&
        control_commits==2 && result.frames.frames==3);
    NezhaCameraNativeControlConsumer invalid{NULL,take_controls,NULL,controls_readback};
    request.controls=&invalid;
    assert(!nezha_camera_native_capture(&request,&result));
    request.controls=&control;
    // Register sampling follows the complete group by two actual frame boundaries.
    longer_control_session=true;control_pending=true;
    const unsigned readbacks_before=control_readbacks;
    assert(nezha_camera_native_capture(&request,&result) && result.frames.frames==4);
    assert(result.control_readback_after==3 && result.control_readback.bytes_read==11 &&
        control_readbacks==readbacks_before+2 && control_commits==3);
    longer_control_session=false;
    if(test_mismatch) {
        control_pending=true;longer_control_session=true;readback_mismatch=true;
        const unsigned commits_before=control_commits;
        assert(!nezha_camera_native_capture(&request,&result) && result.cleaned);
        assert(result.status==NezhaFramesIo && result.control_readback_after==3 &&
            result.control_readback.exposure_lines==321 && control_commits==commits_before+1);
        puts("Native control mismatch: real sampled value retained, one group only, no fabricated match, normal teardown passed");
        return 0;
    }
    // The first NACK ends this owner without another take or group commit.
    control_failure=NezhaCciNack;control_pending=true;
    const unsigned takes_before=control_takes;
    assert(!nezha_camera_native_capture(&request,&result) && result.cleaned);
    assert(result.control_attempted && result.control_status==NezhaCciNack &&
        result.control_result.applied_writes==0 && result.control_result.group_may_be_held &&
        result.control_completed_before==1 && result.sensor_status==NezhaCciNack);
    assert(control_takes==takes_before+1 && control_commits==4 && control_completions==4);
    puts("Native controls: sole worker frame boundary, one ACK, callback Stop, no empty/repeated commits and first NACK cleanup passed");
    return 0;
    }
    frame_timeout=true;
    csid[0x880/4]=0x1100212;
    csid[0x8c0/4]=27;
    vfe[0x2c00/4]=0x10001;
    assert(!nezha_camera_native_capture(&request,&result) && result.cleaned && stream_readbacks==4);
    assert(result.stream_start_readback==1 && result.stream_start_readback_status==NezhaCciOk);
    assert(result.status==NezhaFramesTimeout && result.stream_readback==1 && result.stream_readback_status==NezhaCciOk);
    assert(result.capture_registers[0]==0x1100212 && result.capture_registers[6]==27 &&
        result.capture_registers[12]==0x10001 &&
        result.capture_registers[13]==(original_bus_cgc|0x07ffffff));
    assert(vfe[0x1008/4]==original_bus_cgc);
    const unsigned failed_starts=starts;
    assert(nezha_camera_native_capture_can_reopen());
    const NezhaCameraNativeCaptureResult failed=result;
    frame_timeout=false;keep_running=true;
    assert(nezha_camera_native_capture(&request,&result) && starts==failed_starts+1 && result.frames.frames==3);
    assert(!failed.passed && failed.status==NezhaFramesTimeout && failed.cleaned);
    puts("Native continuous camera: two three-frame sessions, callback Stop, permanent maps and ordered resource release passed");
}
