/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Compile/run after the human selects the package. Directly exercises the
 * production queue: session reuse, timed segments, malformed data and cancel. */
#include "../NezhaHaptics.h"
#include <assert.h>
#include <string.h>
#include <sys/errno.h>
int main(void) {
    NzHapticsQueue q;uint32_t epoch=0;nz_haptics_queue_open(&q,7);
    assert(!nz_haptics_queue_start(&q,7,&epoch) && epoch==1);
    NzHapticsPacket p={NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,NzHapticsPCM,192,7,epoch,1,0,0};
    int16_t pcm[192];for(unsigned i=0;i<192;++i)pcm[i]=i<96?INT16_MIN:0;
    assert(!nz_haptics_queue_pcm(&q,&p,pcm,1000,2000));
    assert(q.count==2 && q.segments[0].amplitude==1000 && !q.segments[1].amplitude);
    assert(nz_haptics_queue_pcm(&q,&p,pcm,1000,2000)==-ESTALE); /* duplicate */
    NzHapticsSegment s;assert(!nz_haptics_queue_next(&q,1000,&s));
    assert(nz_haptics_queue_next(&q,2000,&s)==1 && s.end_us==4000);
    assert(!nz_haptics_queue_stop(&q,7,epoch) && !q.running && q.count==0 && q.stopping);
    q.stopping=0;assert(!nz_haptics_queue_start(&q,7,&epoch));
    assert(nz_haptics_queue_pcm(&q,&p,pcm,1000,2000)==-ESTALE); /* old epoch */
    p.epoch=epoch;p.sequence=1;p.count=NZ_HAPTICS_MAX_FRAMES+1;
    assert(nz_haptics_queue_pcm(&q,&p,pcm,1000,2000)==-22);
    p.count=192;assert(nz_haptics_queue_pcm(&q,&p,pcm,1000,UINT64_MAX-1)==-34);
    assert(!nz_haptics_queue_pcm(&q,&p,pcm,1000,2000));
    assert(!nz_haptics_queue_next(&q,6001,&s) && q.underruns==2);
    q.count=NZ_HAPTICS_QUEUE;p.sequence=2;
    assert(nz_haptics_queue_pcm(&q,&p,pcm,1000,7000)==-16);
    nz_haptics_queue_close(&q);assert(!q.session && !q.ready);
    nz_haptics_queue_open(&q,8);assert(!nz_haptics_queue_start(&q,8,&epoch));
    assert(nz_haptics_queue_pcm(&q,&p,pcm,1000,7000)==-ESTALE); /* old client */
    return 0;
}
