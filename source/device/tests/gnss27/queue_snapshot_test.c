/* SPDX-License-Identifier: MIT */
/* Exercise the actual bounded reader against a queue changing between reads. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define uint64_t unsigned long long
#define BT_USER_ADDRESS_BITS 47
#define BT_STRIP_USER_PC(v) ((v)&UINT64_C(0x00007fffffffffff))
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#include "../../package/BootUiTraceCore.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "../../package/BootUiTraceQueue.h"
static unsigned reads,writes,fail_read,change_read;
static unsigned char memory[0x10000];
static void put(uint64_t at,uint64_t value) {assert(at+8<=sizeof(memory));memcpy(memory+at,&value,8);}
static int reader(void *context,uint64_t at,void *out,unsigned size) {
 (void)context;++reads;
 if(reads==fail_read || at>sizeof(memory) || size>sizeof(memory)-at)return 1;
 if(reads==change_read) {put(0x23c8,0x6000);put(0x23d0,0x5030);put(0x4040,0x8000);}
 memcpy(out,memory+at,size);return 0;
}
static void reset(void) {
 memset(memory,0,sizeof(memory));reads=writes=fail_read=change_read=0;
 put(0x20a0,0x4000);put(0x23c8,0x9000);put(0x23d0,0);
 put(0x4030,0x7000);put(0x4068,0);put(0x4060,0x4800);
 memcpy(memory+0x4800,"CLSuggestedActionManagerSilo",29);
 put(0x4038,0x1234);put(0x4000,0x10000000);
 put(0x4030,0x7000);
 // Node at 0x4030 has next=0x4040; its remaining bytes are arbitrary.
 put(0x4040,0);
}
static struct btq_thread thread(void) {
 return (struct btq_thread){.tid=2459,.tsd=0x2000,.sp=0x9000,.fp=0x9100,
 .x19=0x4030,.x20=0x4040,.state=4,.hint=2,.pri=31,.runq=-1,.last=2,.bound=-1,.chosen=2};
}
int main(void) {
 struct btq_record r;struct btq_thread t=thread();struct bt_entry e={0};
 reset();e.pc=BTQ_WAIT_RETURN;t.pc=e.pc;t.lr=BTQ_WAIT_START;
 assert(btq_capture(&r,&t,&e,0,reader,0));
 assert(r.kind==BTQ_WAIT && r.slot==0x4040 && r.tail_slot==0x4030);
 assert(r.lane==0x4000 && r.value==0 && r.tail_value==0x7000 && r.value_after==0);
 assert((r.valid&(BTQ_FRAME|BTQ_NODE|BTQ_LANE|BTQ_LABEL))==(BTQ_FRAME|BTQ_NODE|BTQ_LANE|BTQ_LABEL));
 assert(r.node==0x4030 && !r.failed && writes==0);
 unsigned first_reads=reads;assert(first_reads<16);
 // During thread_switch the current x19/x20 belong to its caller. Recover
 // only the saved pair beneath a frame returning to the original wait loop.
 reset();e.pc=0x230000000;e.frames[0]=BTQ_WAIT_RETURN+0x2738000;
 put(0x9100,0x9200);put(0x9108,UINT64_C(0xa001000000000000)|(BTQ_WAIT_RETURN+0x2738000));
 put(0x90f0,0x4040);put(0x90f8,0x4030);t.x19=17;t.x20=18;
 assert(btq_capture(&r,&t,&e,0x2738000,reader,0));
 assert(r.anchor==0x9100 && r.slot==0x4040 && r.tail_slot==0x4030);
 // A chain sampled after the waiter returned is incomplete, never guessed.
 reset();put(0x9108,0x240000000);put(0x9100,0x9100);
 assert(btq_capture(&r,&t,&e,0x2738000,reader,0));
 assert(r.kind==BTQ_WAIT && !(r.valid&BTQ_FRAME) && (r.failed&BTQ_FRAME));
 assert(!r.slot && !r.tail_slot && !r.node);
 // A producer is selected by its real TSD marker; another thread's current
 // queue need not be the queue to which it is adding work.
 reset();memset(&e,0,sizeof(e));e.pc=BTQ_LIBDISPATCH_START+0x100;put(0x23d0,0x4030);
 assert(btq_capture(&r,&t,&e,0,reader,0) && r.kind==BTQ_ENQUEUE);
 assert(r.lane==0x4000 && r.enqueuer==0x4030 && !r.slot);
 // No marker and no original waiter frame means no extra record.
 reset();assert(!btq_capture(&r,&t,&e,0,reader,0));assert(reads==2);
 put(0x23d0,0x4030);e.pc=0x230000000;
 assert(!btq_capture(&r,&t,&e,0,reader,0));
 // Record read failures and changes independently. Neither a zero from a
 // failed read nor two different snapshots become successful queue values.
 reset();e.pc=BTQ_WAIT_RETURN;t=thread();change_read=5;
 assert(btq_capture(&r,&t,&e,0,reader,0));
 assert(r.value==0 && r.value_after==0x8000 && r.dsc!=r.dsc_after && r.enqueuer!=r.enqueuer_after);
 assert(!r.failed && writes==0);
 for(unsigned failed=1;failed<=first_reads;++failed) {
  reset();fail_read=failed;
  assert(btq_capture(&r,&t,&e,0,reader,0));
  assert(r.failed && reads<16 && writes==0);
 }
 reset();t.x20=UINT64_C(0x800000000000);t.tsd=0;
 assert(btq_capture(&r,&t,&e,0,reader,0));assert(r.failed&BTQ_VALUE);
 // Row truncation is explicit, and every complete row is terminated within
 // the existing 120-byte buffer even at maximum-width pointer values.
 char rows[16][120];memset(rows,0xa5,sizeof(rows));
 r.ticks=UINT64_MAX;r.finished=UINT64_MAX;
 unsigned n=btq_rows(&r,259,rows,0,1);assert(n==1 && !strncmp(rows[0],"QMISS ",6));
 n=btq_rows(&r,259,rows,0,16);assert(n==12);
 for(unsigned i=0;i<n;++i)assert(memchr(rows[i],0,120));
 assert(!writes);
 puts("PASS: original waiter frame, producer TSD, concurrent changes, failed reads, bounded complete rows");
 return 0;
}
