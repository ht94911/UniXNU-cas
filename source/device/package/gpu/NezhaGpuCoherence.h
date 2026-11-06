/* SPDX-License-Identifier: MIT */
#pragma once
/* Adapted from UniXNU-sheng ce0c600, package/GpuPowerProbe.cpp, coherenceProbe.
 * Run before publication, on four owned pages in the boot VM. Data coherence
 * and table-walk coherence are separate: this does not change the table walk,
 * owned-BO policy, A840 single ring, frequency policy, or client lifecycle.
 * A failed/uncertain GPU operation retains the mapping and its backing pages.
 */
static NativeAdrenoMemory coherenceMemory;
static uint8_t *coherenceBuffer;
static uint64_t coherenceHandle;
static constexpr uint64_t kCoherenceIova=0x18000000; // unused boot-VM range, below client VA 4 GiB
static constexpr size_t kCoherenceBytes=4*4096;

static bool coherenceDramPage(uint64_t physical,uint32_t out[1024])
{
    if((physical&4095) || physical<gPhysBase || physical-gPhysBase>=gPhysSize ||
        4096>gPhysSize-(physical-gPhysBase))return false;
    const vm_offset_t page=ml_io_map_unmappable(vm_offset_t(physical),4096,0x7 /* VM_WIMG_IO */);
    if(!page)return false;
    const volatile uint32_t *mapped=reinterpret_cast<const volatile uint32_t *>(page);
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned i=0;i<1024;++i)out[i]=mapped[i];
    __asm__ volatile("dmb oshld":::"memory");
    ml_io_unmap(page,4096);
    return true;
}

static IOReturn coherenceCopyPage(uint64_t source,uint64_t destination)
{
    uint32_t words[7]{},fence=0,observed=0;
    // CP_MEM_TO_MEM and CP_WAIT_FOR_ME, the same memory proof as sheng.
    if(AdrenoPM4Type7(0x75,5,&words[0])!=AdrenoPM4Success ||
        AdrenoPM4Type7(0x12,0,&words[6])!=AdrenoPM4Success)return kIOReturnBadArgument;
    words[1]=1024;words[2]=uint32_t(source);words[3]=uint32_t(source>>32);
    words[4]=uint32_t(destination);words[5]=uint32_t(destination>>32);
    IOReturn status=ring.submit(words,7,&fence);
    if(status)return status;
    if(ring.wait(fence,&clock,1000000,&observed)!=NativeAdrenoRingFenceComplete)return kIOReturnTimeout;
    // Fence visibility alone is insufficient for releasing any DMA backing.
    return ring.idle(&clock,100000);
}

static IOReturn coherenceRetain(IOReturn status,const char *reason)
{
    native_adreno_io_coherent=0;
    native_gpu_coherence[6]=uint32_t(status);native_gpu_coherence[7]=0;
    native_adreno840_coherence_retained=coherenceBuffer?1:0;
    snprintf(native_gpu_coherence_row,sizeof native_gpu_coherence_row,
        "H stage%u s%x retained -> off",native_gpu_coherence[0],uint32_t(status));
    IOLog("NEZHA A840 COHERENCE: %s (%s); resources retained; no publication\n",native_gpu_coherence_row,reason);
    return status;
}

static IOReturn coherenceProbe(NativeAdrenoVm *vm,unsigned output_bits)
{
    unsigned wanted=1;
    PE_parse_boot_argn("sheng-gpu-coherent",&wanted,sizeof wanted);
    native_adreno_io_coherent=0;
    if(!wanted || !vm) {
        snprintf(native_gpu_coherence_row,sizeof native_gpu_coherence_row,"H skip arg%u vm%d -> off",wanted,vm!=nullptr);
        IOLog("NEZHA A840 COHERENCE: %s\n",native_gpu_coherence_row);
        return kIOReturnSuccess;
    }
    // The previous CP pixel test has retired; verify all GPU masters idle.
    IOReturn status=ring.idle(&clock,100000);
    if(status)return coherenceRetain(status,"pre-probe idle");
    native_gpu_coherence[0]=1;
    coherenceBuffer=static_cast<uint8_t *>(IOMallocAligned(kCoherenceBytes,4096));
    if(!coherenceBuffer) {
        native_gpu_coherence[6]=uint32_t(kIOReturnNoMemory);
        snprintf(native_gpu_coherence_row,sizeof native_gpu_coherence_row,"H nomem -> off");
        IOLog("NEZHA A840 COHERENCE: %s\n",native_gpu_coherence_row);
        return kIOReturnSuccess;
    }
    native_adreno840_coherence_retained=1;
    status=coherenceMemory.importTask(kernel_task,uint64_t(uintptr_t(coherenceBuffer)),kCoherenceBytes,output_bits,kCoherenceBytes);
    if(status)return coherenceRetain(status,"import");
    native_gpu_coherence[0]=2;
    status=vm->map(&coherenceMemory,0,kCoherenceIova,3,1,3,&coherenceHandle);
    if(status)return coherenceRetain(status,"cacheable inner-shareable map");
    uint64_t physical[4]{};
    for(size_t i=0;i<coherenceMemory.spanCount();++i) {
        const auto &span=coherenceMemory.spans()[i];
        if((span.offset|span.physical|span.bytes)&4095 || span.offset>=kCoherenceBytes ||
            span.bytes>kCoherenceBytes-span.offset)return coherenceRetain(kIOReturnBadArgument,"physical span bounds");
        for(size_t offset=0;offset<span.bytes;offset+=4096)
            physical[(span.offset+offset)/4096]=span.physical+offset;
    }
    for(unsigned i=0;i<4;++i)if(!physical[i])return coherenceRetain(kIOReturnBadArgument,"missing owned page");
    volatile uint32_t *S=reinterpret_cast<volatile uint32_t *>(coherenceBuffer),*D=S+1024,*R=S+2048,*T=S+3072;
    static uint32_t dram[1024];
    native_gpu_coherence[0]=3;
    bool dirty=false,pass=false;
    for(unsigned attempt=0;attempt<8 && !status && !dirty;++attempt) {
        native_adreno840_coherence_attempts=attempt+1;
        for(unsigned i=0;i<1024;++i) {S[i]=0x11110000u|i;D[i]=0;R[i]=0x33330000u|i;T[i]=0x44440000u|i;}
        status=NativeAdrenoMaintainOwnedPages(coherenceBuffer,kCoherenceBytes,NativeAdrenoCleanInvalidate);
        if(status)break;
        for(unsigned i=0;i<1024;++i)S[i]=0x22220000u|i;
        __asm__ volatile("dsb sy":::"memory");
        native_gpu_coherence[1]=0;
        if(!coherenceDramPage(physical[0],dram)) {status=kIOReturnIOError;break;}
        for(unsigned i=0;i<1024;i+=16)if(dram[i]==(0x11110000u|i))
            native_gpu_coherence[1]=native_gpu_coherence[1]+1;
        dirty=native_gpu_coherence[1]>=48;
    }
    if(!status && dirty) {
        native_gpu_coherence[0]=4;
        // Use the ring directly: ordinary Submit publishes mapped memory and
        // would clean S, invalidating the proof's dirty-cache precondition.
        status=coherenceCopyPage(kCoherenceIova,kCoherenceIova+4096);
        if(status)return coherenceRetain(status,"read-direction GPU copy");
        if(!coherenceDramPage(physical[1],dram))status=kIOReturnIOError;
        else {
            native_gpu_coherence[0]=5;
            for(unsigned i=0;i<1024;++i) {
                if(dram[i]==(0x22220000u|i))native_gpu_coherence[2]=native_gpu_coherence[2]+1;
                if(dram[i]==(0x11110000u|i))native_gpu_coherence[3]=native_gpu_coherence[3]+1;
            }
        }
    }
    if(!status && native_gpu_coherence[0]==5) {
        status=NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(T),4096,NativeAdrenoClean);
        uint32_t touch=0;
        for(unsigned i=0;i<1024;++i)touch+=T[i]; // clean resident CPU cache lines before the GPU overwrites them
        __asm__ volatile("dsb sy":::"memory");
        (void)touch;
        if(!status) {
            status=coherenceCopyPage(kCoherenceIova+8192,kCoherenceIova+12288);
            if(status)return coherenceRetain(status,"write-direction GPU copy");
            native_gpu_coherence[0]=6;
            for(unsigned i=0;i<1024;++i)if(T[i]==(0x33330000u|i))native_gpu_coherence[4]=native_gpu_coherence[4]+1;
            if(!coherenceDramPage(physical[3],dram))status=kIOReturnIOError;
            else for(unsigned i=0;i<1024;++i)if(dram[i]==(0x33330000u|i))native_gpu_coherence[5]=native_gpu_coherence[5]+1;
        }
    }
    pass=!status && native_gpu_coherence[0]==6 && native_gpu_coherence[1]>=48 &&
        native_gpu_coherence[2]==1024 && native_gpu_coherence[3]==0 &&
        native_gpu_coherence[4]==1024 && native_gpu_coherence[5]==1024;
    native_gpu_coherence[6]=uint32_t(status);
    // Every submitted copy has passed both its fence and RBBM/ring idle.
    IOReturn cleanup=vm->unmap(coherenceHandle);
    if(cleanup)return coherenceRetain(cleanup,"unmap");
    coherenceHandle=0;
    cleanup=NativeAdrenoMaintainOwnedPages(coherenceBuffer,kCoherenceBytes,NativeAdrenoCleanInvalidate);
    if(cleanup)return coherenceRetain(cleanup,"final cache maintenance");
    cleanup=coherenceMemory.release();
    if(cleanup)return coherenceRetain(cleanup,"descriptor release");
    IOFreeAligned(coherenceBuffer,kCoherenceBytes);coherenceBuffer=nullptr;
    native_adreno840_coherence_retained=0;
    native_gpu_coherence[7]=pass?1:0;
    native_adreno_io_coherent=pass?1:0;
    snprintf(native_gpu_coherence_row,sizeof native_gpu_coherence_row,"H d%u/64 a%u r%u/%u w%u/%u s%x k%u -> %s",
        native_gpu_coherence[1],native_adreno840_coherence_attempts,native_gpu_coherence[2],native_gpu_coherence[3],
        native_gpu_coherence[4],native_gpu_coherence[5],native_gpu_coherence[6],native_gpu_coherence[0],pass?"on":"off");
    IOLog("NEZHA A840 COHERENCE: %s; table walk and owned BO policy unchanged\n",native_gpu_coherence_row);
    return kIOReturnSuccess;
}
