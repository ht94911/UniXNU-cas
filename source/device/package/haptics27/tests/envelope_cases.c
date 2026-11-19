/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual queue admission and timing with the reused native-drive envelope. */
#include "../NezhaHaptics.h"
#include <assert.h>
#include <stdio.h>

static NzHapticsQueue queue;
static NzHapticsPacket packet;
static int16_t samples[96];
static unsigned push(int sample) {
    uint32_t epoch=0;nz_haptics_queue_open(&queue,7);
    assert(!nz_haptics_queue_start(&queue,7,&epoch));
    packet=(NzHapticsPacket){NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,NzHapticsPCM,96,7,epoch,1,0,0};
    for(unsigned i=0;i<96;++i)samples[i]=(int16_t)sample;
    assert(!nz_haptics_queue_pcm(&queue,&packet,samples,1000,2000));
    assert(queue.count==1 && queue.segments[0].begin_us==2000 && queue.segments[0].end_us==4000);
    assert(queue.next_sequence==2 && queue.requests==1 && queue.running && queue.ready);
    return queue.segments[0].amplitude;
}
int main(void) {
    assert(push(0)==0 && push(127)==0 && push(-127)==0);
    assert(push(128)>=625 && push(-128)==push(128));
    /* Actual018 onset levels: selection about31permille, impact about22.
     * The native carrier now gets a useful fraction of its verified2V cap. */
    assert(push(1000)>=650 && push(1000)<700);
    assert(push(721)>=640 && push(721)<680);
    assert(push(14207)==1000 && push(32767)==1000 && push(-32768)==1000);
    unsigned previous=0;
    for(unsigned peak=0;peak<=32768;++peak) {
        const int sample=peak==32768?-32768:(int)peak;
        unsigned actual=push(sample);
        assert(actual>=previous && actual<=1000);
        assert((uint64_t)2000*actual/1000<=2000);
        if(peak<32768)assert(push(-(int)peak)==actual);
        previous=actual;
    }
    assert(push(1000)>0);
    assert(!nz_haptics_queue_stop(&queue,7,queue.epoch));
    assert(!queue.count && !queue.running && queue.stopping);
    NzHapticsSegment out;
    assert(!nz_haptics_queue_next(&queue,3000,&out));
    assert(push(14207)==1000);
    nz_haptics_queue_close(&queue);
    assert(!queue.count && !queue.running && !queue.ready && queue.stopping);
    assert(push(14207)==1000);
    assert(!nz_haptics_queue_next(&queue,4000,&out) && !queue.count && queue.underruns==1);
    /* The normalized curve changes no late-buffer or rest policy. */
    uint32_t epoch=0;nz_haptics_queue_open(&queue,7);assert(!nz_haptics_queue_start(&queue,7,&epoch));
    packet.epoch=epoch;packet.sequence=1;queue.rest_until_us=10000;
    assert(!nz_haptics_queue_pcm(&queue,&packet,samples,3000,4000));
    assert(!queue.count && queue.next_sequence==2 && queue.running && !queue.error);
    puts("PASS production envelope: silent threshold, signed symmetry, monotonic stock-drive range, real018 soft onsets, cap, timing and cancellation");
}
