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
namespace {
constexpr uint64_t imageBase=0x229758000ULL;
constexpr uint64_t symbolAddress=0x229caef04ULL;
constexpr uint64_t literalAddress=0x229d1c3d8ULL;
constexpr uint8_t imageUuid[16]={0x89,0x5a,0x68,0xd2,0x37,0x52,0x3e,0x14,0x90,0x07,0xb0,0x7e,0x70,0x5a,0x21,0x78};
constexpr uint32_t instructions[3]={0xd0000368,0xfd41ed00,0xd65f03c0};
constexpr uint64_t appleClock=0x4176e36000000000ULL;
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
    if (!task || task!=current_task() || !task->shared_region) return kIOReturnNotPermitted;
    mach_timebase_info_data_t tb;clock_timebase_info(&tb);
    if (!tb.numer || !tb.denom) return kIOReturnUnsupported;
    uint64_t hz=1000000000ULL*tb.denom/tb.numer;
    if (hz!=19200000 && hz!=24000000) return kIOReturnUnsupported;
    uint64_t expectedBits=hz==24000000 ? appleClock:0x41724f8000000000ULL;
    uint32_t slide=__atomic_load_n(&task->task_shared_region_slide,__ATOMIC_ACQUIRE);
    uint64_t base=imageBase+slide,symbol=symbolAddress+slide,literal=literalAddress+slide;
    if ((slide&4095) || (literal&7)) return kIOReturnUnsupported;
    Pin headerPin,codePin,literalPin;
    if (!headerPin.set(task,base,16384) || !codePin.set(task,symbol,sizeof instructions) || !literalPin.set(task,literal,8)) return kIOReturnNotReadable;
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
    if (!found || at!=sizeof header+header.sizeofcmds || slide!=__atomic_load_n(&task->task_shared_region_slide,__ATOMIC_ACQUIRE)) return kIOReturnUnsupported;
    uint64_t seen=0;
    if (copyin(literal,&seen,8)) return kIOReturnNotReadable;
    if (seen==expectedBits) return kIOReturnSuccess; /* already fixed; never scale twice */
    if (seen!=appleClock) return kIOReturnUnsupported;
    uint64_t pa=pmap_find_pa(get_task_pmap(task),literal);
    if (!pa || (pa&7)) return kIOReturnNotReadable;
    auto *value=reinterpret_cast<uint64_t *>(phystokv(pa));
    if (!value) return kIOReturnNotReadable;
    if (!__atomic_compare_exchange_n(value,&seen,expectedBits,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST) && seen!=expectedBits) return kIOReturnUnsupported;
    __asm__ volatile("dsb sy" :::"memory");
    uint64_t verify=0;
    return !copyin(literal,&verify,8) && verify==expectedBits ? kIOReturnSuccess:kIOReturnIOError;
}
