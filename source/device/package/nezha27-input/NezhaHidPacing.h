// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board contact queue policy;the actual HID descriptor/dispatch stays original27.
#pragma once
#include "NezhaInput.h"
#include <stddef.h>
struct nezha_hid_pacer {
    struct nezha_touch_frame delivered,pending,lookahead;
    bool ready,has_lookahead;
    uint64_t coalesced;
};
typedef bool (*nezha_hid_read_frame)(void *,struct nezha_touch_frame *);
static inline bool nezha_hid_same_contacts(const struct nezha_touch_frame *a,const struct nezha_touch_frame *b) {
    if(a->active!=b->active)return false;
    for(unsigned i=0;i<NEZHA_TOUCH_MAX;++i)if((a->active>>i)&1u)
        if(a->x[i]!=b->x[i] || a->y[i]!=b->y[i])return false;
    return true;
}
static inline bool nezha_hid_pick(struct nezha_hid_pacer *p,nezha_hid_read_frame read,void *ctx) {
    if(p->ready)return true;
    if(p->has_lookahead){p->pending=p->lookahead;p->has_lookahead=false;}
    else if(!read(ctx,&p->pending))return false;
    p->ready=true;
    // The board queue contains at most64 frames. Merge only position updates
    // for an unchanged set of contact IDs;retain the next down/up boundary.
    if(p->pending.active==p->delivered.active)for(unsigned i=1;i<64;++i) {
        struct nezha_touch_frame next;
        if(!read(ctx,&next))break;
        if(next.active!=p->pending.active){p->lookahead=next;p->has_lookahead=true;break;}
        p->pending=next;++p->coalesced;
    }
    return true;
}
static inline void nezha_hid_commit(struct nezha_hid_pacer *p) {
    p->delivered=p->pending;p->ready=false;
}
