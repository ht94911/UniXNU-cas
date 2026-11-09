// SPDX-License-Identifier: BSD-2-Clause-Patent
// Finite synthetic UI actions for the opt-in brightness test, input-test=2.
// Frames still pass through the original report3/consumer2 HID implementation.
#pragma once
#include "NezhaInput.h"
#include <string.h>

enum nzui_kind {NZUI_TAP=1,NZUI_SWIPE=2,NZUI_KEY=3};
enum nzui_phase {NZUI_IDLE,NZUI_QUEUED,NZUI_RUNNING,NZUI_LIFTING,
                 NZUI_DONE,NZUI_CANCELLED,NZUI_FAULT};
enum nzui_read {NZUI_PASS,NZUI_WAIT,NZUI_FRAME};
struct nzui_action {
    uint64_t token;
    uint32_t kind,duration_ms,key;
    uint16_t x0,y0,x1,y1;
};
struct nzui_state {
    struct nzui_action action;
    uint64_t created,started,hold;
    uint32_t phase,serial,pending,pending_mask,emitted,acked,succeeded;
    uint16_t x,y;
    int32_t result;
    bool cancelled,release_confirmed;
    uint32_t last_wire_mask;
};

static inline bool nzui_decode(uint32_t kind,uint32_t arg,uint64_t cursor,
                               uint64_t token,struct nzui_action *out) {
    if(!out || !token || kind<NZUI_TAP || kind>NZUI_KEY)return false;
    struct nzui_action a;memset(&a,0,sizeof(a));
    a.kind=kind;a.token=token;a.duration_ms=(uint32_t)(cursor>>32);
    if(kind==NZUI_KEY) {
        if((uint32_t)cursor || (arg!=NEZHA_KEY_HOME && arg!=NEZHA_KEY_POWER) ||
           a.duration_ms<80 || a.duration_ms>300)return false;
        a.key=arg;
    } else {
        a.x0=(uint16_t)(arg>>16);a.y0=(uint16_t)arg;
        a.x1=(uint16_t)(cursor>>16);a.y1=(uint16_t)cursor;
        if(a.x0>=NEZHA_PANEL_W || a.x1>=NEZHA_PANEL_W ||
           a.y0>=NEZHA_PANEL_H || a.y1>=NEZHA_PANEL_H)return false;
        if(kind==NZUI_TAP) {
            if(a.x0!=a.x1 || a.y0!=a.y1 || a.duration_ms<80 || a.duration_ms>300)return false;
        } else if(a.duration_ms<100 || a.duration_ms>1200 ||
                  (a.x0==a.x1 && a.y0==a.y1))return false;
    }
    *out=a;return true;
}

static inline bool nzui_busy(const struct nzui_state *s) {
    return s->phase==NZUI_QUEUED || s->phase==NZUI_RUNNING || s->phase==NZUI_LIFTING;
}
static inline void nzui_begin(struct nzui_state *s,const struct nzui_action *a,
                              uint64_t now,uint64_t hold) {
    uint32_t serial=s->serial;memset(s,0,sizeof(*s));
    s->action=*a;s->created=now;s->hold=hold;s->phase=NZUI_QUEUED;s->serial=serial;
}
static inline void nzui_ack(struct nzui_state *s,uint32_t sequence,uint32_t wire_mask,int32_t result) {
    if(!sequence || sequence!=s->pending)return;
    s->pending=0;s->result=result;s->last_wire_mask=wire_mask;++s->acked;
    if(result){s->phase=NZUI_FAULT;return;}
    ++s->succeeded;
    if(s->phase==NZUI_LIFTING) {
        s->release_confirmed=wire_mask==0;
        s->phase=s->cancelled?NZUI_CANCELLED:NZUI_DONE;
    }
}
static inline uint32_t nzui_sequence(struct nzui_state *s,uint32_t mask) {
    if(s->serial>=0x0ffffffeU){s->phase=NZUI_FAULT;s->result=-1;return 0;}
    s->pending=0xe0000000U|++s->serial;s->pending_mask=mask;++s->emitted;return s->pending;
}
static inline bool nzui_start(struct nzui_state *s,uint64_t now,bool physical,bool ready) {
    if(s->phase!=NZUI_QUEUED)return true;
    if(physical){s->cancelled=true;s->release_confirmed=true;s->phase=NZUI_CANCELLED;return false;}
    if(!ready)return false;
    s->phase=NZUI_RUNNING;s->started=now;s->x=s->action.x0;s->y=s->action.y0;return true;
}
static inline enum nzui_read nzui_touch(struct nzui_state *s,uint64_t now,bool physical,
                                      bool ready,struct nezha_touch_frame *out) {
    if(!out || s->action.kind==NZUI_KEY || !nzui_busy(s))return NZUI_PASS;
    if(physical)s->cancelled=true;
    if(s->phase==NZUI_QUEUED && s->cancelled) {
        s->phase=NZUI_CANCELLED;s->release_confirmed=true;return NZUI_PASS;
    }
    bool first=s->phase==NZUI_QUEUED;
    if(!nzui_start(s,now,physical,ready))return nzui_busy(s)?NZUI_WAIT:NZUI_PASS;
    if(s->pending)return NZUI_WAIT;
    if(!ready)return NZUI_WAIT;
    uint32_t active=1;
    if(!first) {
        uint64_t elapsed=now-s->started;
        if(s->cancelled || (elapsed>=s->hold && s->x==s->action.x1 && s->y==s->action.y1)) {
            active=0;s->phase=NZUI_LIFTING;
        } else if(s->action.kind==NZUI_SWIPE) {
            if(elapsed>s->hold)elapsed=s->hold;
            int64_t dx=(int32_t)s->action.x1-(int32_t)s->action.x0;
            int64_t dy=(int32_t)s->action.y1-(int32_t)s->action.y0;
            uint16_t x=(uint16_t)((int64_t)s->action.x0+dx*(int64_t)elapsed/(int64_t)s->hold);
            uint16_t y=(uint16_t)((int64_t)s->action.y0+dy*(int64_t)elapsed/(int64_t)s->hold);
            if(x==s->x && y==s->y)return NZUI_WAIT;
            s->x=x;s->y=y;
        } else return NZUI_WAIT;
    }
    memset(out,0,sizeof(*out));out->sequence=nzui_sequence(s,active);
    if(!out->sequence)return NZUI_PASS;
    out->active=active;out->x[0]=s->x;out->y[0]=s->y;return NZUI_FRAME;
}
static inline bool nzui_key(struct nzui_state *s,uint64_t now,bool physical,bool ready,
                            uint32_t *mask,uint32_t *sequence) {
    *mask=0;*sequence=0;
    if(s->action.kind!=NZUI_KEY || !nzui_busy(s))return false;
    if(physical)s->cancelled=true;
    if(s->phase==NZUI_QUEUED && s->cancelled) {
        s->phase=NZUI_CANCELLED;s->release_confirmed=true;return false;
    }
    bool first=s->phase==NZUI_QUEUED;
    if(!nzui_start(s,now,physical,ready))return false;
    *mask=s->phase==NZUI_LIFTING?0:s->action.key;
    if(s->pending || !ready)return true;
    if(first)*sequence=nzui_sequence(s,*mask);
    else if(s->cancelled || now-s->started>=s->hold) {
        *mask=0;s->phase=NZUI_LIFTING;*sequence=nzui_sequence(s,0);
    }
    return nzui_busy(s);
}

#ifdef __cplusplus
extern "C" {
#endif
enum nzui_read nezha_brightness_ui_touch(struct nezha_touch_frame *out,bool physical,bool ready);
bool nezha_brightness_ui_keys(uint32_t physical,uint32_t sent,uint32_t *mask,uint32_t *sequence);
void nezha_brightness_ui_report(uint32_t sequence,uint32_t mask,int32_t result);
bool nezha_brightness_ui_key_faulted(void);
void nezha_brightness_ui_key_error(int32_t result);
#ifdef __cplusplus
}
#endif
