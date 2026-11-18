/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_HAPTICS_PROTOCOL_H
#define NEZHA_HAPTICS_PROTOCOL_H
#include <stdint.h>
#ifdef __APPLE__
#include <sys/ioccom.h>
#else
#include <sys/ioctl.h>
#endif
#define NZ_HAPTICS_DEVICE "/dev/nezha-haptics27"
#define NZ_HAPTICS_MAGIC 0x4e5a4850u
#define NZ_HAPTICS_ABI 1u
#define NZ_HAPTICS_MAX_FRAMES 2048u
#define NZ_HAPTICS_RATE 48000u
enum { NzHapticsStart=1, NzHapticsStop, NzHapticsPCM, NzHapticsPulse, NzHapticsEffect };
typedef struct {
    uint32_t magic,version,operation,count;
    uint32_t session,epoch,sequence,value;
    uint64_t host_time;
} NzHapticsPacket;
typedef struct {
    uint32_t version,session,epoch,next_sequence;
    uint32_t ready,active,queued,error;
    uint32_t completed,cancelled,requests,underruns;
} NzHapticsStatus;
#define NZ_HAPTICS_GET_STATUS _IOR('H',1,NzHapticsStatus)
/* No payload or caller-supplied address. The kernel validates the current
 * audio server and exact24A446 CoreAudio image before aligning its clock. */
#define NZ_HAPTICS_FIX_HOST_CLOCK _IO('H',2)
#endif
