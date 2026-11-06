/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* A840 board backend over the maintained NativeAdreno software core. Hardware
 * setup comes from f5b0fdd; lifecycle/publication follows current sheng.
 * One attempt, one ring, CPU table switching, no implicit reset or scanout. */
#include "NezhaGpuPlatform.h"
#include "NezhaGpuFirmware.h"
// NEZHA_GPU_DIAGNOSTIC_PUBLICATION_BEGIN
#include "NezhaGpuPublicationMode.h"
// NEZHA_GPU_DIAGNOSTIC_PUBLICATION_END
#include "NativeAdrenoClient.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoUserClient.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "NezhaGpuPerfUsb.h"

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *,NativeAdrenoSubmit *,NativeAdrenoRing *,
    const NativeAdrenoRingClock *,const NativeAdrenoTimer *,const NativeAdrenoAbiDeviceInfo *,uint32_t,uint32_t);
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t thread_terminate(thread_t);
extern "C" volatile uint32_t native_adreno_diag;
extern "C" volatile uint32_t nezha_gpu_state,nezha_gpu_level;
extern "C" char nezha_gpu_step[96];
extern "C" {
volatile uint32_t native_adreno840_stage,native_adreno840_status,native_adreno840_pixels;
volatile uint32_t native_adreno840_fence,native_adreno840_completed,native_adreno840_metal;
volatile uint32_t native_adreno840_board_error,native_adreno840_wptr_retries;
}
// A display may use its CPU path before any Metal renderer is discoverable.
// Once publication starts this is monotonic: a GPU failure never converts an
// unknown fence into a CPU-complete frame.
extern "C" bool native_adreno_gpu_rendering_active(void)
{
    return __atomic_load_n(&native_adreno840_metal,__ATOMIC_ACQUIRE)!=0;
}

// Reuse sheng's fresh uncached DRAM mapping, restricted to an already-owned
// IB range and one page per call. The caller holds the existing GPU gate;
// no mapping survives the read and no CPU cache line is cleaned or discarded.
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned int);
extern "C" void ml_io_unmap(vm_offset_t,vm_size_t);
extern "C" uint64_t gPhysBase,gPhysSize;
static int readStallWords(uint64_t physical,uint32_t *out,size_t count)
{
    if(!out || !count || count>64 || (physical&3) ||
        count>(4096-size_t(physical&4095))/4 || physical<gPhysBase ||
        physical-gPhysBase>=gPhysSize || count>(gPhysSize-(physical-gPhysBase))/4)return 1;
    const vm_offset_t mapped=ml_io_map_unmappable(vm_offset_t(physical&~uint64_t(4095)),4096,0x7 /* VM_WIMG_IO */);
    if(!mapped)return 1;
    const volatile uint32_t *words=reinterpret_cast<const volatile uint32_t *>(mapped+(physical&4095));
    __asm__ volatile("dsb sy":::"memory");
    for(size_t i=0;i<count;++i)out[i]=words[i];
    __asm__ volatile("dmb oshld":::"memory");
    ml_io_unmap(mapped,4096);
    return 0;
}

namespace {
// NEZHA_GPU_DIAGNOSTIC_PUBLICATION_BEGIN
NezhaGpuPublicationMode publicationMode = NezhaGpuPublicationMode::Off;
// NEZHA_GPU_DIAGNOSTIC_PUBLICATION_END
NezhaGpuPlatform platform;
NativeAdrenoRing ring;
NativeAdrenoCpInit cp;
NativeAdrenoSubmit submit;
NativeAdrenoTimer timer;
NativeAdrenoUserClient probe_core;
NativeAdrenoClient probe_wire;
NativeAdrenoAbiDeviceInfo info{};
NativeAdrenoGpuContext *context;

uint64_t nowUs(void *)
{
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;
}
int idle(void *) {IOSleep(1);return 0;}
const NativeAdrenoRingClock clock{nullptr,nowUs,idle};
int read32(void *,uint32_t reg,uint32_t *value)
{
    if(!value || !platform.gpu || reg>=platform.gpu_bytes/4)return -1;
    *value=reinterpret_cast<volatile uint32_t *>(platform.gpu)[reg];
    __asm__ volatile("dmb oshld":::"memory");return 0;
}
int write32(void *,uint32_t reg,uint32_t value)
{
    if(!platform.gpu || reg>=platform.gpu_bytes/4)return -1;
    const size_t fence_offset=0x23b13u*4u-0x37000u;
    if(reg==0x807 && (!platform.gmu || fence_offset+4>platform.gmu_bytes))return -1;
    for(unsigned attempt=0;attempt<=200;++attempt) {
        __asm__ volatile("dmb oshst":::"memory");
        reinterpret_cast<volatile uint32_t *>(platform.gpu)[reg]=value;
        __asm__ volatile("dsb sy":::"memory");
        if(reg!=0x807 || !(*reinterpret_cast<volatile uint32_t *>(platform.gmu+fence_offset)&1u))return 0;
        // f5b0fdd/mainline fenced write: wait and retry WPTR. The status word
        // is not treated as a write-one-to-clear register.
        native_adreno840_wptr_retries=native_adreno840_wptr_retries+1;
        IODelay(10);
    }
    return -1; // Ring poisons and retains all outstanding resources.
}
int barrier(void *) {__asm__ volatile("dsb sy":::"memory");return 0;}
int publish(void *,const volatile uint32_t *words,size_t bytes)
{
    if(!context)return -1;
    for(unsigned i=1;i<=2;++i) {
        auto &memory=const_cast<NativeAdrenoMemory &>(context->buffer(i));
        if(words==memory.mapping() && bytes==memory.bytes())return memory.publishForDevice(0,bytes,i==2,true);
    }
    return -1;
}
int acquire(void *,volatile uint32_t *words,size_t bytes)
{
    if(!context)return -1;
    auto &memory=const_cast<NativeAdrenoMemory &>(context->buffer(2));
    return words==memory.mapping() && bytes==memory.bytes()?memory.acquireDeviceWrites(0,bytes):-1;
}
IOReturn checkpoint(unsigned stage,IOReturn status,const char *what)
{
    native_adreno840_stage=stage;native_adreno840_status=uint32_t(status);
    strlcpy(nezha_gpu_step,what,sizeof nezha_gpu_step);
    IOLog("NEZHA A840: stage %u status %#x %s\n",stage,unsigned(status),what);
    return status;
}
IOReturn ioResult(int result)
{
    if(result) {
        native_adreno840_board_error=uint32_t(result);
        IOLog("NEZHA A840: board operation returned %d\n",result);
    }
    return result?kIOReturnIOError:kIOReturnSuccess;
}
IOReturn dispatch(void *,uint32_t method,const void *in,size_t in_size,void *out,size_t out_size)
{
    return probe_core.dispatch(method,in,in_size,out,out_size,&clock);
}

/* Command-processor writes, not a shader/Metal claim. Both BOs are allocated
 * through the actual wire dispatcher and retained until the real fence. */
IOReturn knownPixels(NativeAdrenoVm *vm,unsigned input_bits,unsigned output_bits)
{
    IOReturn status=probe_core.open(NativeAdrenoAbiVersion,vm,&submit,&ring,input_bits,output_bits);
    if(status)return status;
    status=probe_core.setScope(0,GpuClientVaStart,1u<<20);
    if(status)return status;
    NativeAdrenoTransport transport{nullptr,dispatch};
    if((status=probe_wire.open(&transport)))return status;
    uint64_t ib=0,target=0,mapped=0;
    if((status=probe_wire.allocBo(4096,12,true,&ib)) ||
       (status=probe_wire.allocBo(4096,12,true,&target)))return status;
    const uint64_t ib_va=GpuClientVaStart,target_va=GpuClientVaStart+0x10000;
    if((status=probe_wire.mapBo(ib,ib_va,1,0,2,&mapped)) ||
       (status=probe_wire.mapBo(target,target_va,3,0,2,&mapped)))return status;
    NativeAdrenoMemory *commands=probe_core.memoryOf(ib),*pixels=probe_core.memoryOf(target);
    if(!commands || !pixels || !commands->mapping() || !pixels->mapping())return kIOReturnNotReady;
    auto *words=static_cast<uint32_t *>(commands->mapping());
    auto *image=static_cast<uint32_t *>(pixels->mapping());
    image[0]=0x51a84001;image[80]=0x51a84002;
    if(AdrenoPM4Type7(0x3d,66,&words[0])!=AdrenoPM4Success)return kIOReturnBadArgument;
    words[1]=uint32_t(target_va+64);words[2]=uint32_t((target_va+64)>>32);
    for(unsigned i=0;i<64;++i)words[3+i]=(i&1)?0xff20c060:0xffc04020;
    if((status=commands->publishForDevice(0,4096,false,true)) ||
       (status=pixels->publishForDevice(0,4096,true,true)))return status;
    NativeAdrenoAbiSubmitBuffer buffer{ib,ib_va,67*4};
    uint64_t fence=0;
    if((status=probe_wire.submit(&buffer,1,0,0,&fence)))return status;
    native_adreno840_fence=uint32_t(fence);
    if((status=probe_wire.waitFence(fence,1000000000ull)))return status;
    native_adreno840_completed=ring.completedFence();
    if((status=pixels->acquireDeviceWrites(0,4096)))return status;
    if(image[0]!=0x51a84001 || image[80]!=0x51a84002)return kIOReturnIOError;
    for(unsigned i=0;i<64;++i)if(image[16+i]!=words[3+i])return kIOReturnIOError;
    native_adreno840_pixels=64;
    IOLog("NEZHA A840: CP_PIXEL_PASS 64 words; fence %llu completed %u; guards intact\n",fence,ring.completedFence());
    if((status=probe_wire.freeBo(ib)) || (status=probe_wire.freeBo(target)))return status;
    if((status=probe_wire.close()) || (status=probe_core.close()))return status;
    return kIOReturnSuccess;
}

IOReturn publishMetal()
{
    IOReturn status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if(status)return status;
    const OSSymbol *name=OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent=name?OSMetaClass::getMetaClassWithName(name):nullptr;
    OSSafeReleaseNULL(name);
    if(!parent || parent->getClassSize()!=136)return kIOReturnNotFound;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle)return kIOReturnNoResources;
    native_adreno_accelerator_static_init();
    if((status=OSMetaClass::postModLoad(handle)))return status;
    if(native_adreno_accelerator_instance_size()!=136)return kIOReturnInvalid;
    IOService *service=native_adreno_accelerator_allocate();
    if(!service)return kIOReturnNoMemory;
    if(!service->init()) {service->release();return kIOReturnError;}
    service->setProperty("MetalPluginName","NativeAdrenoMetal");
    service->setProperty("MetalPluginClassName","NativeAdrenoMetalDevice");
    service->setProperty("NativeSandboxGrantProcess","*");
    if(!service->attach(IOService::getPlatform())) {service->release();return kIOReturnNotAttached;}
    // NEZHA_GPU_DIAGNOSTIC_PUBLICATION_BEGIN
    // The maintained plugin's default diagnostic path starts its real GPU
    // tests and returns nil to Metal discovery. Only published UI rendering
    // selects GPU display fences; diagnostic work keeps CPU presentation.
    if (nezhaGpuUsesDisplayFences(publicationMode)) {
    // NEZHA_GPU_DIAGNOSTIC_PUBLICATION_END
    __atomic_store_n(&native_adreno840_metal,1u,__ATOMIC_RELEASE);
    // NEZHA_GPU_DIAGNOSTIC_PUBLICATION_BEGIN
    }
    // NEZHA_GPU_DIAGNOSTIC_PUBLICATION_END
    service->registerService();
    return kIOReturnSuccess;
}

IOReturn bringUp()
{
    NezhaGpuFirmwareView firmware{};
    int fw=nezha_gpu_firmware_validate(nezha_gpu_fwpack,size_t(nezha_gpu_fwpack_size),&firmware);
    if(fw)return checkpoint(1,kIOReturnNotFound,"firmware missing/invalid; no MMIO");
    IOReturn status=checkpoint(2,ioResult(nezha_gpu_platform_prepare(&platform)),"board GMU/HFI/GX");
    if(status)return status;
    if((status=checkpoint(3,ioResult(nezha_gpu_platform_prepare_context()),"owned GPU bank and stream")))return status;
    NativeAdrenoGpuContextPlan plan;plan.sqe_bytes=0x20000;plan.buffer_count=5;plan.stream_count=1;plan.coherent_walk=false;
    NativeAdrenoGmuContextReport report{};
    status=NativeAdrenoGpuContextBringupWithPlan(platform.smmu,platform.smmu_bytes,platform.gpu_bank,platform.gpu_asid,
        platform.gpu_slot,platform.gpu_sid,0,0,nullptr,&report,&plan);
    IOLog("NEZHA A840: GPU context step %u status %#x routes %u IAS %u OAS %u\n",report.step,unsigned(status),
        report.routes_verified,NativeAdrenoGpuContextInputBits(),report.output_bits);
    if(checkpoint(4,status,"native4K context"))return status;
    context=NativeAdrenoGpuContextObject();
    if(!context || context->bufferCount()!=5 || firmware.bytes[1]<=4 || firmware.bytes[1]-4>plan.sqe_bytes)
        return checkpoint(5,kIOReturnBadArgument,"SQE bounds");
    auto &sqe=const_cast<NativeAdrenoMemory &>(context->buffer(0));
    memcpy(sqe.mapping(),firmware.data[1]+4,firmware.bytes[1]-4);
    if((status=sqe.publishForDevice(0,sqe.bytes(),false,true)))return checkpoint(5,status,"SQE publication");
    if((status=ioResult(nezha_gpu_platform_program_cp(GpuSqeIova,GpuMemptrsIova))))return checkpoint(6,status,"CP registers");
    NativeAdrenoRingIo io{nullptr,write32,read32,barrier,nullptr,nullptr};
    NativeAdrenoRingCache cache{nullptr,publish,acquire};
    if((status=checkpoint(6,ring.useChip(&NativeAdrenoChipA8xx),"select validated A8xx ring profile")))return status;
    status=ring.attach(&io,&cache,context->buffer(1).mapping(),context->buffer(1).bytes(),GpuRingIova,
        context->buffer(2).mapping(),context->buffer(2).bytes(),GpuMemptrsIova);
    if(checkpoint(7,status,"single A840 ring"))return status;
    if((status=ioResult(nezha_gpu_platform_start_sqe())))return checkpoint(8,status,"SQE start write");
    if((status=cp.useChip(&NativeAdrenoChipA8xx)))return status;
    NativeAdrenoCpInitConfig cfg{GpuSqeIova,GpuPwrupIova,1};
    uint32_t fence=0,observed=0;
    if((status=cp.run(&io,&ring,&cfg,&fence)))return checkpoint(8,status,"CP_ME_INIT submit");
    if(ring.wait(fence,&clock,1000000,&observed)!=NativeAdrenoRingFenceComplete)
        return checkpoint(8,kIOReturnTimeout,"CP_ME_INIT real fence pending");
    if((status=ioResult(nezha_gpu_platform_authenticate_zap())))return checkpoint(9,status,"ZAP PAS13");
    uint32_t secure[2]{};
    if(AdrenoPM4Type7(0x66,1,&secure[0])!=AdrenoPM4Success)return kIOReturnBadArgument;
    if((status=ring.submit(secure,2,&fence)))return checkpoint(9,status,"secure-mode exit submit");
    if(ring.wait(fence,&clock,1000000,&observed)!=NativeAdrenoRingFenceComplete)
        return checkpoint(9,kIOReturnTimeout,"secure-mode exit real fence pending");
    if((status=checkpoint(10,ioResult(nezha_gpu_platform_render_state()),"A840 rendering registers")))return status;
    NativeAdrenoVm *vm=context->vm();
    if((status=submit.attach(vm,&ring)))return checkpoint(11,status,"dispatcher attach");
    if((status=checkpoint(12,knownPixels(vm,context->inputBits(),report.output_bits),"CP pixel/readback/retirement")))return status;
    NativeAdrenoTimeIo time{nullptr,read32};
    if((status=timer.useChip(&NativeAdrenoChipA8xx)) || (status=timer.attach(&time,NativeAdrenoTimeCp)))return status;
    info.chip_id=platform.chip_id;info.gmem_bytes=18u<<20;info.gmem_base=64u<<20;
    info.uche_trap_base=0x1fffffffff000ull;info.va_start=GpuClientVaStart;
    info.va_size=(1ull<<context->inputBits())-GpuClientVaStart;
    info.highest_bank_bit=16;info.ubwc_swizzle=6;info.macrotile_mode=1;info.ubwc_version=6;
    info.flags=NativeAdrenoDeviceClientIova;
    native_adreno_stall_read_words=readStallWords;
    status=native_adreno_publish(vm,&submit,&ring,&clock,&timer,&info,context->inputBits(),report.output_bits);
    if(checkpoint(13,status,"NativeAdreno user client"))return status;
    ring.setTimeline(true); // existing A8xx CP_ALWAYS_ON timestamps around each real job
    if(nezha_gpu_platform_perf_start())return checkpoint(13,kIOReturnIOError,"GPU performance owner");
    const int perf_registered=qcom_usb_diag_register(&nezhaPerfProvider);
    IOLog("NEZHA GPU PERF: USB provider7 registration=%d; real job timestamps enabled; CPU load sampled every2s\n",perf_registered);
    if(perf_registered)return checkpoint(13,kIOReturnIOError,"GPU performance provider");
    int metal=0;PE_parse_boot_argn("nezha-gpu-metal",&metal,sizeof metal);
    // NEZHA_GPU_DIAGNOSTIC_PUBLICATION_BEGIN
    publicationMode = nezhaGpuPublicationMode(metal);
    if (publicationMode == NezhaGpuPublicationMode::Invalid)
        return checkpoint(14,kIOReturnBadArgument,"unsupported accelerator publication mode");
    if (publicationMode == NezhaGpuPublicationMode::Diagnostic)
        IOLog("NEZHA A840: diagnostic accelerator discovery; CPU presentation retained; no Metal device publication claim\n");
    // NEZHA_GPU_DIAGNOSTIC_PUBLICATION_END
    if(metal && (status=checkpoint(14,publishMetal(),"27 accelerator publication")))return status;
    return checkpoint(15,kIOReturnSuccess,"ready; Metal/rendering still require independent tests");
}
void worker(void *,wait_result_t)
{
    IOReturn status=bringUp();
    native_adreno840_status=uint32_t(status);nezha_gpu_state=status?4:3;
    if(status)IOLog("NEZHA A840: stopped at stage %u, status %#x; resources retained; no reset\n",
        native_adreno840_stage,unsigned(status));
    thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
}
extern "C" void nezha_adreno840_start(void)
{
    static bool attempted=false;
    if(attempted)return;
    attempted=true;
    int enabled=0;PE_parse_boot_argn("nezha-gpu",&enabled,sizeof enabled);
    if(!enabled)return;
    native_adreno_diag=1; /* Preserve bounded GPU failure/timeline diagnostics. */
    thread_t thread=nullptr;
    if(kernel_thread_start(worker,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    else checkpoint(1,kIOReturnNoResources,"worker could not start");
}
