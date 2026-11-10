// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "NativeGOPTiming.h"

// nezha's retained stock DT: mdp_phys 0x09800000/0x1c8800, DPU13,
// interface types DP/DSI/DSI/DP, offsets 18e000/18f000/190000/191000.
// Register meanings: fixed SM8850 display-driver revision
// 5ab2a689ff87d7d28c511f1762cf41c1b90d965a, sde_hw_intf.c.
// No SM8550 address, IRQ bit or video-only counter is used as a board fact.
enum {NEZHA_DPU_BASE=0x09800000, NEZHA_DPU_BYTES=0x1c8800,
      NEZHA_DPU_INTF_BYTES=0x4bc, NEZHA_DPU_ATTEMPTS=3};
static const uint32_t nezha_dpu_intf_offsets[2]={0x18f000,0x190000};
static const uint16_t nezha_dpu_timing_offsets[NGP_TIMING_WORDS]={
    0x004, // INTF_CONFIG
    0x26c, // INTF_STATUS
    0x000, // INTF_TIMING_ENGINE_EN
    0x0a8, // INTF_FRAME_LINE_COUNT_EN
    0x084, // INTF_DSI_CMD_MODE_TRIGGER_EN
    0x210, // INTF_VSYNC_TIMESTAMP_CTRL
    0x284, // INTF_TEAR_TEAR_CHECK_EN
    0x288, // INTF_TEAR_SYNC_CONFIG_VSYNC
    0x28c, // INTF_TEAR_SYNC_CONFIG_HEIGHT
    0x2b4, // INTF_TEAR_AUTOREFRESH_CONFIG
    0x2c0, // INTF_TEAR_AUTOREFRESH_STATUS
    0x0a4, // INTF_MDP_FRAME_COUNT (autorefresh, DPU >= A00)
    0x0ac, // INTF_FRAME_COUNT (retained separately, never substituted silently)
    0x298, // INTF_TEAR_INT_COUNT_VAL: read frame in upper 16 bits
    0x2dc, // INTF_TEAR_INT_COUNT_VAL_EXT: high read-line bits, DPU >= A00
    0x2b0, // INTF_TEAR_LINE_COUNT: write line
    0x2ac, // INTF_TEAR_OUT_LINE_COUNT
    0x268, // INTF_UNDERRUN_COUNT
    0x180, // INTF_MISR_CTRL: observation only, never enabled/cleared here
    0x184, // INTF_MISR_SIGNATURE: raw and invalid when its control is disabled
    0x214, // INTF_VSYNC_TIMESTAMP0: panel event low
    0x218, // INTF_VSYNC_TIMESTAMP1: panel event high
    0x21c, // INTF_MDP_VSYNC_TIMESTAMP0
    0x220, // INTF_MDP_VSYNC_TIMESTAMP1
    0x290, // INTF_TEAR_SYNC_WRCOUNT: source reports write-frame count in high 16 bits
    0x2e4  // INTF_TEAR_SYNC_WRCOUNT_EXT
};

struct NezhaDpuTimingIo {
    void *cookie;
    // Reads relative to the board's MDP physical resource. The adapter owns
    // read-only mappings and calls only while the inherited display is active.
    int (*read32)(void *,uint32_t offset,uint32_t *);
    uint64_t (*now_us)(void *);
    uint64_t (*counter)(void *);
};

static inline int nezha_dpu_read(const struct NezhaDpuTimingIo *io,
    uint32_t offset,uint32_t *value) {
    if((offset&3u) || offset>NEZHA_DPU_BYTES-4u)return 1;
    return io->read32(io->cookie,offset,value);
}

static inline int nezha_dpu_intf_read(const struct NezhaDpuTimingIo *io,
    unsigned intf,uint32_t offset,uint32_t *value) {
    if(intf>=NGP_TIMING_INTERFACES || offset>NEZHA_DPU_INTF_BYTES-4u)return 1;
    return nezha_dpu_read(io,nezha_dpu_intf_offsets[intf]+offset,value);
}

// Read a timestamp high/low/high; reject rollover or a concurrent latch rather
// than assembling unrelated halves into a plausible-looking physical time.
static inline int nezha_dpu_timestamp(const struct NezhaDpuTimingIo *io,
    unsigned intf,uint32_t low,uint32_t high,uint32_t *lo,uint32_t *hi) {
    uint32_t after;
    if(nezha_dpu_intf_read(io,intf,high,hi) ||
       nezha_dpu_intf_read(io,intf,low,lo) ||
       nezha_dpu_intf_read(io,intf,high,&after))return NGP_TIMING_IO;
    return after==*hi?NGP_TIMING_OK:NGP_TIMING_UNSTABLE;
}

// Keep one coherent raw sample of both interfaces. Stable only describes the
// sampled counter bracket: the host still must prove an enabled,
// advancing physical clock and a stable scanout version. A timer tick, inactive
// block, stale timestamp or MISR value alone never counts as a displayed frame.
static inline uint32_t nezha_dpu_sample(const struct NezhaDpuTimingIo *io,
    struct NGPTimingReply *out) {
    if(!out)return NGP_TIMING_ARGUMENT;
    out->version=NGP_TIMING_VERSION;out->flags=0;out->attempts=0;
    out->hardware_version=0;
    out->cpu_before_us=out->cpu_after_us=0;
    out->counter_before=out->counter_after=0;
    memset(out->interfaces,0,sizeof out->interfaces);
    if(!io || !io->read32 || !io->now_us || !io->counter)
        return out->status=NGP_TIMING_ARGUMENT;
    for(unsigned attempt=1;attempt<=NEZHA_DPU_ATTEMPTS;++attempt) {
        out->attempts=attempt;out->flags=0;
        out->cpu_before_us=io->now_us(io->cookie);
        out->counter_before=io->counter(io->cookie);
        if(nezha_dpu_read(io,0,&out->hardware_version))
            return out->status=NGP_TIMING_IO;
        for(unsigned intf=0;intf<NGP_TIMING_INTERFACES;++intf) {
            uint32_t before[4],after[4];
            const uint32_t anchor[4]={0x0a4,0x0ac,0x298,0x290};
            for(unsigned n=0;n<4;++n)
                if(nezha_dpu_intf_read(io,intf,anchor[n],&before[n]))
                    return out->status=NGP_TIMING_IO;
            uint32_t *words=out->interfaces[intf];
            for(unsigned n=0;n<NGP_TIMING_WORDS;++n) {
                if(n>=20 && n<24)continue;
                if(nezha_dpu_intf_read(io,intf,nezha_dpu_timing_offsets[n],&words[n]))
                    return out->status=NGP_TIMING_IO;
            }
            uint32_t panel=nezha_dpu_timestamp(io,intf,0x214,0x218,&words[20],&words[21]);
            uint32_t mdp=nezha_dpu_timestamp(io,intf,0x21c,0x220,&words[22],&words[23]);
            if(panel==NGP_TIMING_IO || mdp==NGP_TIMING_IO)
                return out->status=NGP_TIMING_IO;
            for(unsigned n=0;n<4;++n)
                if(nezha_dpu_intf_read(io,intf,anchor[n],&after[n]))
                    return out->status=NGP_TIMING_IO;
            if(!panel && !mdp && before[0]==after[0] && before[1]==after[1] &&
               (before[2]>>16)==(after[2]>>16) && words[11]==after[0] &&
               words[12]==after[1] && (words[13]>>16)==(after[2]>>16) &&
               (before[3]>>16)==(after[3]>>16) && (words[24]>>16)==(after[3]>>16))
                out->flags|=NGP_TIMING_INTF0_STABLE<<intf;
        }
        out->counter_after=io->counter(io->cookie);
        out->cpu_after_us=io->now_us(io->cookie);
        if(out->cpu_after_us<out->cpu_before_us || out->counter_after<out->counter_before)
            return out->status=NGP_TIMING_CLOCK;
        if((out->flags&(NGP_TIMING_INTF0_STABLE|NGP_TIMING_INTF1_STABLE))==
            (NGP_TIMING_INTF0_STABLE|NGP_TIMING_INTF1_STABLE)) {
            out->flags|=NGP_TIMING_READ_COMPLETE;
            return out->status=NGP_TIMING_OK;
        }
    }
    return out->status=NGP_TIMING_UNSTABLE;
}
