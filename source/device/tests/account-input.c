/* Exercise the production gesture core and original HID coalescer. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "NezhaAA27Input.h"
#include "NezhaHidPacing.h"

struct reader {struct nezha_touch_frame frame;bool consumed;};
static bool read_frame(void *ctx,struct nezha_touch_frame *out) {
    struct reader *r=ctx;if(r->consumed)return false;
    r->consumed=true;*out=r->frame;return true;
}
static uint32_t point(unsigned x,unsigned y) {return x<<16|y;}

static void gesture(unsigned kind,uint32_t from,uint32_t to) {
    struct aa27_input_state s={.ready=1,.sequence=0x80000000u};
    struct nezha_hid_pacer pacer={0};
    assert(aa27_input_submit(&s,kind,from,to,11,300)==AA27_OK);
    assert(s.phase==AA27_QUEUED && s.commits==0 && s.dispatches==0);
    aa27_input_begin(&s,1,false);assert(s.phase==AA27_QUEUED);
    aa27_input_begin(&s,2,true);assert(s.phase==AA27_ACTIVE);
    unsigned reports=0,downs=0,ups=0,ticks=0;
    while(s.phase==AA27_ACTIVE) {
        struct reader reader={0};assert(aa27_input_frame(&s,&reader.frame));
        assert(reader.frame.x[0]<NEZHA_PANEL_W && reader.frame.y[0]<NEZHA_PANEL_H);
        assert(nezha_hid_pick(&pacer,read_frame,&reader));
        assert(reader.consumed && !pacer.has_lookahead && pacer.coalesced==0);
        bool dispatched=!nezha_hid_same_contacts(&pacer.delivered,&pacer.pending);
        if(dispatched){++reports;downs+=!pacer.delivered.active && pacer.pending.active;
            ups+=pacer.delivered.active && !pacer.pending.active;}
        assert(aa27_input_ack(&s,reader.frame.sequence,0,dispatched));
        nezha_hid_commit(&pacer);assert(++ticks<=18);
    }
    assert(s.phase==AA27_DONE && s.result==0 && downs==1 && ups==1);
    assert(s.commits==ticks && s.dispatches==reports && !pacer.delivered.active);
    assert(ticks==(kind==1?9:18) && reports==(kind==1?2:18));
    assert(pacer.delivered.x[0]==(kind==1?from>>16:to>>16));
    assert(pacer.delivered.y[0]==(kind==1?from&65535u:to&65535u));
    struct aa27_input_state saved=s;
    assert(aa27_input_submit(&s,kind,from,to,11,300)==AA27_INVALID);
    assert(!memcmp(&s,&saved,sizeof(s)));
    assert(aa27_input_submit(&s,kind,from,to,12,300)==AA27_OK);
}
int main(void) {
    gesture(1,point(0,0),0);gesture(1,point(1199,2607),0);
    gesture(2,point(10,2500),point(1100,100));
    gesture(2,point(1199,0),point(0,2607));
    struct aa27_input_state s={.ready=1};
    assert(aa27_input_submit(&s,0,0,0,1,3)==AA27_INVALID);
    assert(aa27_input_submit(&s,1,point(1200,0),0,1,3)==AA27_INVALID);
    assert(aa27_input_submit(&s,1,point(1,2608),0,1,3)==AA27_INVALID);
    assert(aa27_input_submit(&s,1,0,1,1,3)==AA27_INVALID);
    assert(aa27_input_submit(&s,2,0,1ull<<32,1,3)==AA27_INVALID);
    assert(aa27_input_submit(&s,2,1,1,1,3)==AA27_INVALID);
    assert(aa27_input_submit(&s,1,0,0,0,3)==AA27_INVALID);
    s.ready=0;assert(aa27_input_submit(&s,1,0,0,1,3)==AA27_BUSY);
    s.ready=1;assert(aa27_input_submit(&s,1,0,0,1,3)==AA27_OK);
    assert(aa27_input_submit(&s,1,0,0,2,3)==AA27_BUSY);
    aa27_input_begin(&s,3,true);assert(s.phase==AA27_EXPIRED && !s.commits);
    assert(aa27_input_submit(&s,1,0,0,2,10)==AA27_OK);
    aa27_input_begin(&s,4,true);
    struct nezha_touch_frame frame;assert(aa27_input_frame(&s,&frame));
    assert(!aa27_input_ack(&s,frame.sequence+1,0,true));
    assert(aa27_input_ack(&s,frame.sequence,0xe00002bc,true));
    assert(s.phase==AA27_FAILED && !s.commits && s.dispatches==1);
    assert(!aa27_input_frame(&s,&frame));
    assert(aa27_input_submit(&s,1,0,0,3,10)==AA27_ERROR);
    puts("PASS production tap/swipe pacing, all-up completion, bounds, busy, expiry and no replay after HID error");
}
