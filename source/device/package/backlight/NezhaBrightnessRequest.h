// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board brightness wire policy. No MMIO, service publication or completion.
// Inputs: nezha stock DT bl_ctrl_dcs, max0x3fff, bl-inverted-dbv; original
// SM8850 dsi_ctrl_copy_and_pad_cmd and Linux DCS brightness byte convention.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { NEZHA_BRIGHTNESS_MAX=16383, NEZHA_BRIGHTNESS_PACKET_BYTES=8 };

// MSM embedded command header is WC low/high, data-ID, flags. The stock
// inverted DBV swaps Linux's low/high value before its DCS write, giving
// command51/high/low on this panel. The final padding byte is not payload.
static inline bool nezha_brightness_packet(uint32_t level,unsigned channel,
                                          uint8_t *out,size_t capacity) {
    if(!out || capacity<NEZHA_BRIGHTNESS_PACKET_BYTES ||
       level>NEZHA_BRIGHTNESS_MAX || channel>3)return false;
    const uint8_t packet[8]={3,0,(uint8_t)(0x39|(channel<<6)),0xc0,
                             0x51,(uint8_t)(level>>8),(uint8_t)level,0xff};
    for(unsigned i=0;i<8;++i)out[i]=packet[i];
    return true;
}

// One request in flight plus one replaceable newest target. The caller owns
// serialization on its IOWorkLoop. A queued target never changes the last
// acknowledged level. A failed or uncertain transfer requires an explicit
// new hardware lease; no automatic retransmission or fabricated completion.
struct nezha_brightness_queue {
    uint64_t generation,inflight;
    uint32_t acknowledged,target,sending;
    bool acknowledged_valid,pending,blocked;
};
static inline bool nezha_brightness_offer(struct nezha_brightness_queue *q,
                                         uint32_t level) {
    if(!q || q->blocked || level>NEZHA_BRIGHTNESS_MAX)return false;
    q->target=level;
    q->pending=q->inflight || !q->acknowledged_valid || q->acknowledged!=level;
    return true;
}
static inline uint64_t nezha_brightness_take(struct nezha_brightness_queue *q,
                                            uint32_t *level) {
    if(!q || !level || q->blocked || q->inflight || !q->pending)return 0;
    if(q->generation==UINT64_MAX){q->blocked=true;return 0;}
    q->inflight=++q->generation;q->sending=q->target;q->pending=false;
    *level=q->sending;return q->inflight;
}
static inline bool nezha_brightness_finish(struct nezha_brightness_queue *q,
                                           uint64_t generation,bool confirmed) {
    if(!q || q->blocked || !generation || generation!=q->inflight)return false;
    if(!confirmed){q->blocked=true;q->pending=false;q->inflight=0;return true;}
    q->acknowledged=q->sending;q->acknowledged_valid=true;q->inflight=0;
    q->pending=q->target!=q->acknowledged;
    return true;
}
