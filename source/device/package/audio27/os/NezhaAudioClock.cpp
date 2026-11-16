// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exact iPhone14,4/24A446 CoreAudio adaptation, derived from sheng's in-memory
// host-clock correction. No user supplied write address and no disk mutation.
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <sys/systm.h>
#include <mach-o/loader.h>
#include "../hal/AudioClock.h"
extern "C" vm_offset_t phystokv(uint64_t pa);
extern "C" uint64_t pmap_find_pa(pmap_t pmap,uint64_t va);
extern "C" int nezha_audio_current_slide(uint32_t *);
namespace {
constexpr uint64_t imageBase=0x229758000ULL;
constexpr uint64_t symbolAddress=0x229caef04ULL;
constexpr uint64_t literalAddress=0x229d1c3d8ULL;
constexpr uint8_t imageUuid[16]={0x89,0x5a,0x68,0xd2,0x37,0x52,0x3e,0x14,0x90,0x07,0xb0,0x7e,0x70,0x5a,0x21,0x78};
constexpr uint32_t instructions[3]={0xd0000368,0xfd41ed00,0xd65f03c0};
constexpr uint64_t appleClock=0x4176e36000000000ULL;
constexpr uint64_t convertersAddress=0x229caef18ULL;
constexpr uint32_t convertersOriginal[23]={
    0xd503237f,0xa9bf7bfd,0x910003fd,0x52800fa9,0x9b097c08,0x9bc97c01,
    0xaa0803e0,0x52800062,0xd2800003,0x9590bb51,0xa8c17bfd,0xd65f0fff,
    0xd503237f,0xa9bf7bfd,0x910003fd,0x52800068,0x9bc87c01,0x8b000400,
    0x52800fa2,0xd2800003,0x9590bb46,0xa8c17bfd,0xd65f0fff};
static uint32_t converter_word(unsigned i,bool nezha) {
    if (!nezha) return convertersOriginal[i];
    switch (i) {
    case 3:return 0x52804e29; /* mov w9,#625 */
    case 7:return 0x52800182; /* mov w2,#12 */
    case 15:return 0x52800188; /* mov w8,#12 */
    case 17:return 0x9b087c00; /* mul x0,x0,x8: low half must also multiply12 */
    case 18:return 0x52804e22; /* mov w2,#625 */
    default:return convertersOriginal[i];
    }
}
static bool converters_match(const uint32_t *code,bool nezha) {
    for (unsigned i=0;i<23;++i) if (code[i]!=converter_word(i,nezha)) return false;
    return true;
}
static void synchronize_code(void *address,size_t bytes) {
    uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
    const uintptr_t first=(uintptr_t)address,last=first+bytes;
    uintptr_t line=4ULL<<((ctr>>16)&15);
    for (uintptr_t p=first&~(line-1);p<last;p+=line) __asm__ volatile("dc cvau,%0"::"r"(p):"memory");
    __asm__ volatile("dsb ish":::"memory");line=4ULL<<(ctr&15);
    for (uintptr_t p=first&~(line-1);p<last;p+=line) __asm__ volatile("ic ivau,%0"::"r"(p):"memory");
    __asm__ volatile("dsb ish":::"memory");
    __asm__ volatile("isb":::"memory");
}
struct Pin {
    IOMemoryDescriptor *memory=nullptr;bool prepared=false;
    ~Pin() {if (memory) {if (prepared) memory->complete();memory->release();}}
    bool set(task_t task,uint64_t address,size_t bytes) {
        memory=IOMemoryDescriptor::withAddressRange(address,bytes,kIODirectionOut,task);
        if (!memory) return false;
        prepared=memory->prepare()==kIOReturnSuccess;return prepared;
    }
};
}
extern "C" IOReturn nezha_audio_fix_host_clock(task_t task) {
    uint32_t slide=0,confirmed=0;
    if (!task || task!=current_task() || !nezha_audio_current_slide(&slide)) return kIOReturnNotPermitted;
    mach_timebase_info_data_t tb;clock_timebase_info(&tb);
    if (!tb.numer || !tb.denom) return kIOReturnUnsupported;
    uint64_t hz=1000000000ULL*tb.denom/tb.numer;
    if (hz!=19200000 && hz!=24000000) return kIOReturnUnsupported;
    uint64_t expectedBits=hz==24000000 ? appleClock:0x41724f8000000000ULL;
    uint64_t base=imageBase+slide,symbol=symbolAddress+slide,literal=literalAddress+slide,converters=convertersAddress+slide;
    if ((slide&4095) || (literal&7)) return kIOReturnUnsupported;
    Pin headerPin,codePin,literalPin,convertersPin;
    if (!headerPin.set(task,base,16384) || !codePin.set(task,symbol,sizeof instructions) || !literalPin.set(task,literal,8) || !convertersPin.set(task,converters,sizeof convertersOriginal)) return kIOReturnNotReadable;
    struct mach_header_64 header;
    if (copyin(base,&header,sizeof header) || header.magic!=MH_MAGIC_64 || header.cputype!=CPU_TYPE_ARM64 ||
        (uint32_t)header.cpusubtype!=0x80000002u || header.ncmds>128 || header.sizeofcmds>16384-sizeof header) return kIOReturnUnsupported;
    uint32_t code[3];
    if (copyin(symbol,code,sizeof code) || memcmp(code,instructions,sizeof code)) return kIOReturnUnsupported;
    uint32_t at=sizeof header;bool found=false;
    for (unsigned i=0;i<header.ncmds;++i) {
        struct load_command command;
        if (at>sizeof header+header.sizeofcmds || sizeof header+header.sizeofcmds-at<sizeof command ||
            copyin(base+at,&command,sizeof command) || command.cmdsize<sizeof command ||
            command.cmdsize>sizeof header+header.sizeofcmds-at) return kIOReturnUnsupported;
        if (command.cmd==LC_UUID) {
            struct uuid_command uuid;
            if (found || command.cmdsize!=sizeof uuid || copyin(base+at,&uuid,sizeof uuid) || memcmp(uuid.uuid,imageUuid,16)) return kIOReturnUnsupported;
            found=true;
        }
        at+=command.cmdsize;
    }
    if (!found || at!=sizeof header+header.sizeofcmds || !nezha_audio_current_slide(&confirmed) || slide!=confirmed) return kIOReturnUnsupported;
    uint64_t seen=0;
    if (copyin(literal,&seen,8)) return kIOReturnNotReadable;
    if (seen!=expectedBits && seen!=appleClock) return kIOReturnUnsupported;
    uint32_t convertCode[23];
    if (copyin(converters,convertCode,sizeof convertCode)) return kIOReturnNotReadable;
    const bool corrected=converters_match(convertCode,hz==19200000);
    if (!corrected && !converters_match(convertCode,false)) return kIOReturnUnsupported;
    if (!corrected) {
        const uint64_t convertPa=pmap_find_pa(get_task_pmap(task),converters);
        if (!convertPa || (convertPa&3) || (convertPa&4095)+sizeof convertCode>4096) return kIOReturnNotReadable;
        auto *words=reinterpret_cast<uint32_t *>(phystokv(convertPa));
        if (!words) return kIOReturnNotReadable;
        /* This owned user-client gate serializes corrections; install before
         * device publication. Both complete routines and their PAC/branches
         * were checked above. Publish instruction changes after all stores. */
        for (unsigned i=0;i<23;++i) if (convertCode[i]!=converter_word(i,true))
            __atomic_store_n(words+i,converter_word(i,true),__ATOMIC_RELAXED);
        synchronize_code(words,sizeof convertCode);
        if (copyin(converters,convertCode,sizeof convertCode) || !converters_match(convertCode,true)) return kIOReturnIOError;
    }
    if (seen==expectedBits) return kIOReturnSuccess; /* all three APIs already fixed */
    uint64_t pa=pmap_find_pa(get_task_pmap(task),literal);
    if (!pa || (pa&7)) return kIOReturnNotReadable;
    auto *value=reinterpret_cast<uint64_t *>(phystokv(pa));
    if (!value) return kIOReturnNotReadable;
    if (!__atomic_compare_exchange_n(value,&seen,expectedBits,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST) && seen!=expectedBits) return kIOReturnUnsupported;
    __asm__ volatile("dsb sy" :::"memory");
    uint64_t verify=0;
    return !copyin(literal,&verify,8) && verify==expectedBits ? kIOReturnSuccess:kIOReturnIOError;
}
