// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../package/nezha27-input/NezhaTouchEdge.h"

static uint32_t event(struct nezha_touch_edge_state *s,unsigned id,uint32_t mask,
                      int touch,int32_t x,int32_t y) {
    return nezha_touch_edge_mask(s,id,mask,touch,x,y,1200,2608);
}
static void gesture(struct nezha_touch_edge_state *s,unsigned id,
                    int32_t start_x,int32_t start_y,int from_edge) {
    const uint32_t extra=from_edge?0x800:0;
    assert(event(s,id,7,1,start_x,start_y)==(7|extra));
    assert(event(s,id,4,1,32768,40000)==(4|extra));
    assert(event(s,id,4,1,32768,65500)==(4|extra));
    assert(event(s,id,3,0,32768,65500)==(3|extra));
    assert(s->origin[id]==0);
}
int main(void) {
    struct nezha_touch_edge_state s={0};
    // Complete Home strokes, ordinary application pans and all four edges.
    gesture(&s,1,32768,65000,1);
    gesture(&s,1,32768,32768,0);
    gesture(&s,2,100,30000,1);
    gesture(&s,3,65400,30000,1);
    gesture(&s,4,32768,100,1);
    // A contact beginning outside the48-pixel band stays an ordinary pan.
    gesture(&s,5,32768,64200,0);
    // Simultaneous fingers retain their independent origins through lifts.
    assert(event(&s,10,7,1,32000,65000)==0x807);
    assert(event(&s,11,7,1,32000,32000)==7);
    assert(event(&s,11,4,1,32000,65000)==4);
    assert(event(&s,10,3,0,32000,32000)==0x803);
    assert(event(&s,11,3,0,32000,65000)==3);
    // Native classifications win; no conversion of an EdgeRoll to EdgeTip.
    assert(event(&s,12,0x1007,1,32768,65000)==0x1007);
    assert(event(&s,12,4,1,32768,40000)==0x1004);
    assert(event(&s,12,0x2003,0,32768,40000)==0x2003);
    assert(s.origin[12]==0);
    // Invalid coordinates and IDs do not create a system edge gesture.
    gesture(&s,13,-1,65000,0);
    gesture(&s,14,32768,65537,0);
    assert(event(&s,256,7,1,32768,65000)==7);
    assert(event(NULL,0,7,1,32768,65000)==7);
    assert(nezha_touch_edge_mask(&s,0,7,1,0,0,0,2608)==7);
    // Reused IDs and missing prior releases recover at the next actual down.
    assert(event(&s,255,7,1,32000,65000)==0x807);
    assert(event(&s,255,7,1,32000,32000)==7);
    assert(event(&s,255,3,0,32000,32000)==3);
    for(unsigned id=0;id<NZ_EDGE_CONTACTS;++id)
        for(unsigned repeat=0;repeat<16;++repeat) {
            gesture(&s,id,32768,65000,1);
            gesture(&s,id,32768,32768,0);
        }
    for(unsigned id=0;id<NZ_EDGE_CONTACTS;++id)assert(!s.origin[id]);
    puts("PASS recorded-shape Home strokes, ordinary pans, four edges, multi-contact, native bits, invalid inputs and8192 full ID-reuse gestures");
}
