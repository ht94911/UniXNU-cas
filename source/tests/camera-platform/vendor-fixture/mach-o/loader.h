#pragma once
#include <stdint.h>
#define MH_MAGIC_64 0xfeedfacf
#define LC_UUID 0x1b
struct mach_header {uint32_t magic;};
struct mach_header_64 {uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct load_command {uint32_t cmd,cmdsize;};
struct uuid_command {uint32_t cmd,cmdsize;uint8_t uuid[16];};
