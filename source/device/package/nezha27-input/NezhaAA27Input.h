/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "NezhaInput.h"

/* One explicit gesture, paced by the existing16ms HID owner timer. This core
 * has no device, allocation or dispatch operations. USB only queues it. */
enum aa27_input_phase { AA27_IDLE=0, AA27_QUEUED=1, AA27_ACTIVE=2,
    AA27_DONE=3, AA27_FAILED=4, AA27_EXPIRED=5 };
enum aa27_input_result { AA27_OK=0, AA27_BUSY=2, AA27_INVALID=3, AA27_ERROR=4 };
struct aa27_input_state {
    uint32_t ready,phase,kind,step,last,commits,dispatches,result,sequence;
    uint32_t x0,y0,x1,y1;
    uint64_t action,deadline;
};
static inline bool aa27_position_valid(uint64_t packed) {
    return packed<=UINT32_MAX && (packed>>16)<NEZHA_PANEL_W &&
        (packed&65535u)<NEZHA_PANEL_H;
}
static inline unsigned aa27_input_submit(struct aa27_input_state *s,
    unsigned kind,uint32_t from,uint64_t to,uint64_t action,uint64_t deadline) {
    if(!action || action<=s->action || (kind!=1 && kind!=2) ||
       !aa27_position_valid(from) || (kind==1 && to) ||
       (kind==2 && (!aa27_position_valid(to) || to==from)))return AA27_INVALID;
    if(!s->ready || s->phase==AA27_QUEUED || s->phase==AA27_ACTIVE)return AA27_BUSY;
    if(s->phase==AA27_FAILED)return AA27_ERROR;
    s->action=action;s->kind=kind;s->deadline=deadline;
    s->x0=from>>16;s->y0=from&65535u;
    s->x1=kind==1?s->x0:(uint32_t)(to>>16);
    s->y1=kind==1?s->y0:(uint32_t)(to&65535u);
    s->step=0;s->last=kind==1?8:17;s->commits=0;s->dispatches=0;s->result=0;
    s->phase=AA27_QUEUED;return AA27_OK;
}
static inline void aa27_input_begin(struct aa27_input_state *s,uint64_t now,bool idle) {
    if(s->phase!=AA27_QUEUED)return;
    if(now>=s->deadline){s->phase=AA27_EXPIRED;return;}
    if(idle && s->ready)s->phase=AA27_ACTIVE;
}
static inline bool aa27_input_frame(const struct aa27_input_state *s,struct nezha_touch_frame *out) {
    if(s->phase!=AA27_ACTIVE)return false;
    struct nezha_touch_frame frame={0};
    frame.sequence=s->sequence+1;frame.active=s->step<s->last?1:0;
    unsigned position=s->step<s->last?s->step:s->last-1;
    unsigned steps=s->last-1;
    frame.x[0]=(uint16_t)((int64_t)s->x0+((int64_t)s->x1-s->x0)*position/steps);
    frame.y[0]=(uint16_t)((int64_t)s->y0+((int64_t)s->y1-s->y0)*position/steps);
    *out=frame;return true;
}
static inline bool aa27_input_ack(struct aa27_input_state *s,uint32_t sequence,
    uint32_t result,bool dispatched) {
    if(s->phase!=AA27_ACTIVE || sequence!=s->sequence+1)return false;
    if(dispatched)++s->dispatches;
    if(result){s->result=result;s->phase=AA27_FAILED;return true;}
    s->sequence=sequence;++s->commits;
    if(s->step==s->last)s->phase=AA27_DONE;
    else ++s->step;
    return true;
}
