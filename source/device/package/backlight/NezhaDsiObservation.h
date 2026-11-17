// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "Sm8850DsiFifo.h"

// Board-only, single-workloop record. It observes the existing read callback;
// it never owns a register, samples a clock or changes the FIFO state machine.
// Keep this outside both the Apple-derived class and NezhaDsi0's allocation.
struct nezha_dsi_observation {
    const void *owner;
    uint64_t generation,lease_epoch,began_us,first_poll_us,last_poll_us,max_gap_us;
    uint32_t request,sending,acknowledged,polls;
    uint32_t first_valid,first_ints,first_status,last_valid,last_ints,last_status;
    int result;
    bool polling,dumped,active,poisoned,prepared;
};
static struct nezha_dsi_observation nezha_dsi_observation_record={};

static inline void nezha_dsi_observation_begin(const void *owner,uint64_t generation,
        uint32_t request,uint32_t sending,uint32_t acknowledged,
        const struct sm_dsi_transfer *transfer) {
    struct nezha_dsi_observation record={};
    record.owner=owner;record.generation=generation;record.request=request;
    record.sending=sending;record.acknowledged=acknowledged;
    record.lease_epoch=transfer->lease_epoch;record.began_us=transfer->began_us;
    nezha_dsi_observation_record=record;
}
static inline void nezha_dsi_observation_poll_begin(const void *owner,uint64_t generation,
        uint64_t now_us,uint32_t acknowledged) {
    struct nezha_dsi_observation *record=&nezha_dsi_observation_record;
    if(record->owner!=owner || record->generation!=generation || record->dumped)return;
    const uint64_t gap=now_us-(record->polls?record->last_poll_us:record->began_us);
    if(gap>record->max_gap_us)record->max_gap_us=gap;
    if(!record->polls)record->first_poll_us=now_us;
    ++record->polls;record->last_poll_us=now_us;record->acknowledged=acknowledged;
    record->last_valid=record->last_ints=record->last_status=0;
    record->polling=true;
}
static inline void nezha_dsi_observation_read(const void *owner,uint32_t offset,uint32_t value) {
    struct nezha_dsi_observation *record=&nezha_dsi_observation_record;
    if(!record->polling || record->owner!=owner)return;
    const uint32_t bit=offset==SM_DSI_INT?1:offset==SM_DSI_STATUS?2:0;
    if(!bit || (record->last_valid&bit))return;
    // sm_dsi_poll may subsequently read INT during cleanup. Retain only the
    // first successful INT/STATUS pair from observe, including partial reads.
    if(bit==1)record->last_ints=value;else record->last_status=value;
    record->last_valid|=bit;
}
static inline void nezha_dsi_observation_poll_end(const void *owner,uint64_t generation,
        int result,const struct sm_dsi_transfer *transfer) {
    struct nezha_dsi_observation *record=&nezha_dsi_observation_record;
    if(!record->polling || record->owner!=owner || record->generation!=generation)return;
    record->polling=false;record->result=result;
    record->active=transfer->active;record->poisoned=transfer->poisoned;record->prepared=transfer->prepared;
    if(record->polls==1) {
        record->first_valid=record->last_valid;
        record->first_ints=record->last_ints;record->first_status=record->last_status;
    }
}
static inline bool nezha_dsi_observation_fault(const void *owner,uint64_t generation,int result,
        struct nezha_dsi_observation *snapshot) {
    struct nezha_dsi_observation *record=&nezha_dsi_observation_record;
    if(result!=SM_DSI_TIMEOUT || record->result!=result || record->owner!=owner || record->generation!=generation ||
       record->polling || record->dumped)return false;
    *snapshot=*record;record->dumped=true;return true;
}
