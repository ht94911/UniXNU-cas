/* SPDX-License-Identifier: MIT */
#pragma once
/* A840 coherent-DMA qualification, derived from sheng ce0c600.
 * The downstream gen8-2-1 entry declares ADRENO_IOCOHERENT and this board's GPU
 * SMMU declares dma-coherent. On this exact chip the NC/outer-shareable mapping
 * also sees both directions coherently; it is not a non-snooping DRAM witness.
 * Keep that observation, but do not require documented coherent DMA to fail.
 * Require the exact board contract plus two fresh WB trials in both directions,
 * with observed old CPU source/target values before each transfer. These are
 * finite functional checks, not a measurement of physical DRAM/cache residency.
 * Table walks and owned BO policy remain unchanged. All pages are private.
 */
static NativeAdrenoMemory coherenceMemory;
static uint8_t *coherenceBuffer;
static uint64_t coherenceHandle,coherencePhysical[4];
static uint64_t coherenceCpuPar,coherenceTargetPar,coherenceCtr,coherenceSctlr;
static unsigned coherenceSetupReason;
static constexpr uint64_t kCoherenceIova=0x18000000;
static constexpr size_t kCoherenceBytes=4*4096;
struct CoherenceObservation {
    unsigned readNew,readOld,readOther,cachedNew,cachedOld,cachedOther,freshNew,freshOld,freshOther;
    unsigned sourceOld,targetOld;
};
static CoherenceObservation coherenceControl[2],coherenceCached[2];

static uint64_t coherenceTranslation(uintptr_t address)
{
    uint64_t mask,saved,result;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #15\nmrs %1, par_el1\nat s1e1r, %3\nisb\nmrs %2, par_el1\nmsr par_el1, %1\nmsr daif, %0"
        :"=&r"(mask),"=&r"(saved),"=&r"(result):"r"(address):"memory");
    return result;
}
static void coherenceCpuRegisters(void)
{
    __asm__ volatile("mrs %0, ctr_el0":"=r"(coherenceCtr));
    __asm__ volatile("mrs %0, sctlr_el1":"=r"(coherenceSctlr));
}
static bool coherenceTranslationMatches(uint64_t par,uint64_t physical)
{
    return !(par&1) && (par&UINT64_C(0x0000fffffffff000))==physical;
}
static bool coherenceUncachedTranslation(uint64_t par)
{
    const unsigned attributes=unsigned(par>>56);
    return !(par&1) && (attributes==0x00 || attributes==0x04 || attributes==0x08 ||
        attributes==0x0c || attributes==0x44);
}

static IOReturn coherenceRetain(IOReturn status,const char *reason)
{
    native_adreno_io_coherent=0;
    native_gpu_coherence[6]=uint32_t(status);native_gpu_coherence[7]=0;
    native_adreno840_coherence_retained=coherenceBuffer?1:0;
    snprintf(native_gpu_coherence_row,sizeof native_gpu_coherence_row,
        "H2 stage%u s%x retained -> off",native_gpu_coherence[0],uint32_t(status));
    IOLog("NEZHA A840 COHERENCE: %s (%s); resources retained; no publication\n",native_gpu_coherence_row,reason);
    return status;
}
static IOReturn coherenceWait(uint32_t fence)
{
    uint32_t observed=0;
    if(ring.wait(fence,&clock,1000000,&observed)!=NativeAdrenoRingFenceComplete)return kIOReturnTimeout;
    return ring.idle(&clock,100000);
}
static IOReturn coherenceGpuBoundary(void)
{
    // The normal framed prologue flushes/invalidates GPU caches and waits idle.
    // Do it before CPU preparation and before releasing each test mapping.
    uint32_t word=0,fence=0;
    if(AdrenoPM4Type7(0x12,0,&word)!=AdrenoPM4Success)return kIOReturnBadArgument;
    IOReturn status=ring.submitFramed(&word,1,&fence);
    return status?status:coherenceWait(fence);
}
static IOReturn coherenceCopyPage(uint64_t source,uint64_t destination)
{
    uint32_t words[7]{},fence=0;
    if(AdrenoPM4Type7(0x75,5,&words[0])!=AdrenoPM4Success ||
        AdrenoPM4Type7(0x12,0,&words[6])!=AdrenoPM4Success)return kIOReturnBadArgument;
    words[1]=1024;words[2]=uint32_t(source);words[3]=uint32_t(source>>32);
    words[4]=uint32_t(destination);words[5]=uint32_t(destination>>32);
    // Ordinary Submit would publish/clean the source. Only ring storage is
    // published here; the probe source and primed target get no CPU maintenance.
    IOReturn status=ring.submit(words,7,&fence);
    return status?status:coherenceWait(fence);
}
static IOReturn coherenceTrial(NativeAdrenoVm *vm,bool wb,unsigned trial,CoherenceObservation &out)
{
    native_gpu_coherence[0]=wb?5:4;
    native_adreno840_coherence_attempts=trial+1;
    IOReturn status=vm->map(&coherenceMemory,0,kCoherenceIova,3,wb?1:0,wb?3:2,&coherenceHandle);
    if(status)return coherenceRetain(status,"control mapping");
    status=coherenceGpuBoundary();
    if(status)return coherenceRetain(status,"preparation GPU cache boundary");
    volatile uint32_t *S=reinterpret_cast<volatile uint32_t *>(coherenceBuffer),*D=S+1024,*R=S+2048,*T=S+3072;
    // Every trial has a distinct pattern, so a previous GPU cache entry cannot
    // impersonate this trial's old-value control.
    const uint32_t salt=(trial+1)<<20,oldS=0x11000000u^salt,newS=0x22000000u^salt;
    const uint32_t newT=0x33000000u^salt,oldT=0x44000000u^salt;
    for(unsigned i=0;i<1024;++i){S[i]=oldS|i;D[i]=0;R[i]=newT|i;T[i]=oldT|i;}
    status=NativeAdrenoMaintainOwnedPages(coherenceBuffer,kCoherenceBytes,NativeAdrenoCleanInvalidate);
    if(status)return coherenceRetain(status,"initial CPU publication");
    disable_preemption();
    for(unsigned i=0;i<1024;++i){const uint32_t old=S[i];if(old==(oldS|i))++out.sourceOld;S[i]=old^(oldS^newS);}
    __asm__ volatile("dsb sy":::"memory");
    enable_preemption();
    status=coherenceCopyPage(kCoherenceIova,kCoherenceIova+4096);
    if(status)return coherenceRetain(status,"read-direction GPU copy");
    // D is a whole private page, has no dirty CPU lines, and DMA is quiescent.
    // Invalidate only D: never clean or inspect S through another CPU alias.
    status=NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(D),4096,NativeAdrenoInvalidate);
    if(status)return coherenceRetain(status,"read result invalidate");
    for(unsigned i=0;i<1024;++i){const uint32_t value=D[i];
        if(value==(newS|i))++out.readNew;else if(value==(oldS|i))++out.readOld;else ++out.readOther;}
    // Observe the old CPU target immediately before the GPU write. The normal
    // WB trial must see the new data without any intervening CPU maintenance.
    status=NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(T),4096,NativeAdrenoClean);
    if(status)return coherenceRetain(status,"target clean");
    for(unsigned i=0;i<1024;++i)if(T[i]==(oldT|i))++out.targetOld;
    __asm__ volatile("dsb sy":::"memory");
    status=coherenceCopyPage(kCoherenceIova+8192,kCoherenceIova+12288);
    if(status)return coherenceRetain(status,"write-direction GPU copy");
    for(unsigned i=0;i<1024;++i){const uint32_t value=T[i];
        if(value==(newT|i))++out.cachedNew;else if(value==(oldT|i))++out.cachedOld;else ++out.cachedOther;}
    status=NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(T),4096,NativeAdrenoInvalidate);
    if(status)return coherenceRetain(status,"fresh target invalidate");
    for(unsigned i=0;i<1024;++i){const uint32_t value=T[i];
        if(value==(newT|i))++out.freshNew;else if(value==(oldT|i))++out.freshOld;else ++out.freshOther;}
    status=coherenceGpuBoundary();
    if(status)return coherenceRetain(status,"retirement GPU cache boundary");
    status=vm->unmap(coherenceHandle);
    if(status)return coherenceRetain(status,"trial unmap");
    coherenceHandle=0;
    IOLog("NEZHA A840 COHERENCE CONTROL: trial%u wb%u read=%u/%u/%u cached=%u/%u/%u fresh=%u/%u/%u warm=%u/%u\n",
        trial,wb?1u:0u,out.readNew,out.readOld,out.readOther,out.cachedNew,out.cachedOld,out.cachedOther,
        out.freshNew,out.freshOld,out.freshOther,out.sourceOld,out.targetOld);
    return kIOReturnSuccess;
}
static IOReturn coherenceProbe(NativeAdrenoVm *vm,unsigned output_bits)
{
    unsigned wanted=1;PE_parse_boot_argn("sheng-gpu-coherent",&wanted,sizeof wanted);
    native_adreno_io_coherent=0;
    // The platform's public ID includes the enabled slice count in bits4..7:
    // raw gen8-2-1 ID0x44050a01 plus three slices is0x44050a31 on this board.
    // A different chip keeps the maintained path until it has its own evidence.
    const bool declaredCoherent=platform.chip_id==0x44050a31u;
    if(!wanted || !vm || !declaredCoherent){
        snprintf(native_gpu_coherence_row,sizeof native_gpu_coherence_row,"H3 skip arg%u vm%d contract%d -> off",wanted,vm!=nullptr,declaredCoherent);
        IOLog("NEZHA A840 COHERENCE: %s\n",native_gpu_coherence_row);return kIOReturnSuccess;
    }
    IOReturn status=ring.idle(&clock,100000);
    if(status)return coherenceRetain(status,"initial idle");
    native_gpu_coherence[0]=1;
    coherenceBuffer=static_cast<uint8_t *>(IOMallocAligned(kCoherenceBytes,4096));
    if(!coherenceBuffer){
        native_gpu_coherence[6]=uint32_t(kIOReturnNoMemory);
        snprintf(native_gpu_coherence_row,sizeof native_gpu_coherence_row,"H2 nomem -> off");
        IOLog("NEZHA A840 COHERENCE: %s\n",native_gpu_coherence_row);return kIOReturnSuccess;
    }
    native_adreno840_coherence_retained=1;
    status=coherenceMemory.importTask(kernel_task,uint64_t(uintptr_t(coherenceBuffer)),kCoherenceBytes,output_bits,kCoherenceBytes);
    if(status)return coherenceRetain(status,"import");
    native_gpu_coherence[0]=2;
    for(size_t i=0;i<coherenceMemory.spanCount();++i){
        const auto &span=coherenceMemory.spans()[i];
        if((span.offset|span.physical|span.bytes)&4095 || span.offset>=kCoherenceBytes || span.bytes>kCoherenceBytes-span.offset)
            return coherenceRetain(kIOReturnBadArgument,"physical span bounds");
        for(size_t offset=0;offset<span.bytes;offset+=4096)coherencePhysical[(span.offset+offset)/4096]=span.physical+offset;
    }
    for(unsigned i=0;i<4;++i){const uint64_t pa=coherencePhysical[i];
        if(!pa || (pa&4095) || pa<gPhysBase || pa-gPhysBase>=gPhysSize || 4096>gPhysSize-(pa-gPhysBase))
            return coherenceRetain(kIOReturnBadArgument,"physical page bounds");
        for(unsigned j=0;j<i;++j)if(pa==coherencePhysical[j])return coherenceRetain(kIOReturnBadArgument,"duplicate physical page");
    }
    native_gpu_coherence[0]=3;coherenceCpuRegisters();
    coherenceCpuPar=coherenceTranslation(uintptr_t(coherenceBuffer));
    coherenceTargetPar=coherenceTranslation(uintptr_t(coherenceBuffer+3*4096));
    if(!(coherenceSctlr&4) || !coherenceTranslationMatches(coherenceCpuPar,coherencePhysical[0]) ||
        coherenceUncachedTranslation(coherenceCpuPar) || !coherenceTranslationMatches(coherenceTargetPar,coherencePhysical[3]) ||
        coherenceUncachedTranslation(coherenceTargetPar)){coherenceSetupReason=1;status=kIOReturnUnsupported;}
    bool controls=true,positives=true;unsigned pairsDone=0,ncSnooping=0;
    for(unsigned pair=0;pair<2 && !status;++pair){
        status=coherenceTrial(vm,false,pair*2,coherenceControl[pair]);if(status)return status;
        status=coherenceTrial(vm,true,pair*2+1,coherenceCached[pair]);if(status)return status;
        const auto &n=coherenceControl[pair];const auto &w=coherenceCached[pair];
        // NC is diagnostic: either old or new values are legitimate here. A
        // corrupt transfer, invalid initial values, or absent GPU write fails.
        controls=controls && n.sourceOld==1024 && n.targetOld==1024 &&
            n.readNew+n.readOld==1024 && !n.readOther && n.cachedNew+n.cachedOld==1024 && !n.cachedOther &&
            n.freshNew==1024 && !n.freshOld && !n.freshOther;
        if(n.readNew==1024 && n.cachedNew==1024)++ncSnooping;
        positives=positives && w.sourceOld==1024 && w.targetOld==1024 &&
            w.readNew==1024 && !w.readOld && !w.readOther && w.cachedNew==1024 &&
            !w.cachedOld && !w.cachedOther && w.freshNew==1024 && !w.freshOld && !w.freshOther;
        native_gpu_coherence[1]=0; // no independent physical-DRAM/dirty-line witness is claimed
        native_gpu_coherence[2]=w.readNew;native_gpu_coherence[3]=w.readOld;
        native_gpu_coherence[4]=w.cachedNew;native_gpu_coherence[5]=w.freshNew;
        native_gpu_coherence[0]=6;
        ++pairsDone;
    }
    controls=controls && pairsDone==2;positives=positives && pairsDone==2;
    const bool pass=!status && native_gpu_coherence[0]==6 && controls && positives;
    native_gpu_coherence[6]=uint32_t(status);
    // Every trial has flushed GPU caches, retired its fence, idled and unmapped.
    IOReturn cleanup=NativeAdrenoMaintainOwnedPages(coherenceBuffer,kCoherenceBytes,NativeAdrenoCleanInvalidate);
    if(cleanup)return coherenceRetain(cleanup,"final CPU maintenance");
    cleanup=coherenceMemory.release();if(cleanup)return coherenceRetain(cleanup,"descriptor release");
    IOFreeAligned(coherenceBuffer,kCoherenceBytes);coherenceBuffer=nullptr;native_adreno840_coherence_retained=0;
    native_gpu_coherence[7]=pass?1:0;native_adreno_io_coherent=pass?1:0;
    snprintf(native_gpu_coherence_row,sizeof native_gpu_coherence_row,"H3 contract1 ctrl%u wb%u nc%u n%u r%u/%u w%u/%u s%x -> %s",
        controls?1:0,positives?1:0,ncSnooping,native_adreno840_coherence_attempts,native_gpu_coherence[2],native_gpu_coherence[3],
        native_gpu_coherence[4],native_gpu_coherence[5],uint32_t(status),pass?"on":"off");
    IOLog("NEZHA A840 COHERENCE: %s; GPU NC/WB controls; table/owned-BO policy unchanged\n",native_gpu_coherence_row);
    IOLog("NEZHA A840 COHERENCE SETUP: reason%u cpu-par=%llx target-par=%llx ctr=%llx sctlr=%llx; no CPU DRAM alias\n",
        coherenceSetupReason,(unsigned long long)coherenceCpuPar,(unsigned long long)coherenceTargetPar,
        (unsigned long long)coherenceCtr,(unsigned long long)coherenceSctlr);
    return kIOReturnSuccess;
}
