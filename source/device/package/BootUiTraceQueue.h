// SPDX-License-Identifier: MIT
// The existing locationd snapshot's queue fields. All user reads are samples;
// repeated pointers do not make a running queue or stack an atomic snapshot.
#ifndef SHENG_BOOT_UI_TRACE_QUEUE_H
#define SHENG_BOOT_UI_TRACE_QUEUE_H

#define BTQ_WAIT 1u
#define BTQ_ENQUEUE 2u
#define BTQ_WAIT_START UINT64_C(0x1aeb0cf70)
#define BTQ_WAIT_END UINT64_C(0x1aeb0cf8c)
#define BTQ_WAIT_RETURN UINT64_C(0x1aeb0cf80)
#define BTQ_LIBDISPATCH_START UINT64_C(0x1aeaf1000)
#define BTQ_LIBDISPATCH_END UINT64_C(0x1aeb37d18)
#define BTQ_LIBPTHREAD_START UINT64_C(0x18053a000)
#define BTQ_LIBPTHREAD_END UINT64_C(0x180546248)
// Inspected 24A446 libdispatch: current queue at TSD+0xa0, current sync
// context at +0x3c8, and the enqueuer's tail-slot address at +0x3d0.
// Its lane tail is +0x30, head +0x68, and a node's next pointer is +0x10.
enum {
 BTQ_QUEUE=1u, BTQ_TSD=2u, BTQ_QUEUE_REPEAT=4u, BTQ_TSD_REPEAT=8u,
 BTQ_FRAME=16u, BTQ_LANE=32u, BTQ_NODE=64u, BTQ_LABEL=128u,
 BTQ_VALUE=256u, BTQ_TAIL=512u, BTQ_VALUE_REPEAT=1024u, BTQ_TAIL_REPEAT=2048u
};
struct btq_thread {
 uint64_t tid,tsd,sp,fp,pc,lr,x19,x20,x21,x1,x8,x9,x10;
 unsigned state,hint;int pri,runq,last,bound,chosen;
};
struct btq_record {
 struct btq_thread thread;
 uint64_t ticks,finished;
 unsigned kind,valid,failed;
 uint64_t queue,queue_after,dsc,dsc_after,enqueuer,enqueuer_after;
 uint64_t slot,tail_slot,anchor,value,tail_value,value_after,tail_after;
 uint64_t lane,lane_words[16],node,node_words[10];
 char label[49];
};
static int btq_read(struct btq_record *r,unsigned bit,bt_reader read,void *context,
                    uint64_t address,void *out,unsigned size) {
 if(!bt_user(address,size) || (address&7) || read(context,address,out,size)) {
  r->failed|=bit;return 0;
 }
 r->valid|=bit;return 1;
}
static int btq_is_wait(const struct bt_entry *e,unsigned slide) {
 if(e->pc>=BTQ_WAIT_START+slide && e->pc<BTQ_WAIT_END+slide)return 1;
 for(unsigned i=0;i<BT_FRAMES;++i)
  if(e->frames[i]==BTQ_WAIT_RETURN+slide)return 1;
 return 0;
}
static int btq_capture(struct btq_record *r,const struct btq_thread *thread,
                      const struct bt_entry *entry,unsigned slide,
                      bt_reader read,void *context) {
 memset(r,0,sizeof(*r));r->thread=*thread;
 uint64_t tls[2]={0},repeat[2]={0};
 // Inspect a producer marker only while its saved PC/LR is in the selected
 // dispatch/pthread image. This avoids treating an obsolete stack slot from
 // a running application callback as an active enqueue.
 const int producer_context=(entry->pc>=BTQ_LIBDISPATCH_START+slide && entry->pc<BTQ_LIBDISPATCH_END+slide) ||
     (entry->lr>=BTQ_LIBDISPATCH_START+slide && entry->lr<BTQ_LIBDISPATCH_END+slide) ||
     (entry->pc>=BTQ_LIBPTHREAD_START+slide && entry->pc<BTQ_LIBPTHREAD_END+slide);
 if(bt_user(thread->tsd,0x3d8) && !(thread->tsd&7)) {
  btq_read(r,BTQ_QUEUE,read,context,thread->tsd+0xa0,&r->queue,8);
  if(btq_read(r,BTQ_TSD,read,context,thread->tsd+0x3c8,tls,sizeof(tls))) {
   r->dsc=tls[0];r->enqueuer=tls[1];
   if(r->enqueuer && producer_context)r->kind|=BTQ_ENQUEUE;
  }
 } else r->failed|=BTQ_QUEUE|BTQ_TSD;
 if(btq_is_wait(entry,slide)) {
  r->kind|=BTQ_WAIT;
  if(entry->pc>=BTQ_WAIT_START+slide && entry->pc<BTQ_WAIT_END+slide) {
   r->slot=thread->x20;r->tail_slot=thread->x19;
   r->anchor=entry->pc;r->valid|=BTQ_FRAME;
  } else {
   // _pthread_yield_to_enqueuer_4dispatch saves its caller's x20/x19
   // at fp-0x10. Use that frame only when its return is the inspected
   // _dispatch_wait_for_enqueuer call site; other frames are not a match.
   uint64_t fp=thread->fp;
   for(unsigned i=0;i<BT_FRAMES && fp;++i) {
    uint64_t pair[2]={0},saved[2]={0};
    if((fp&15) || !bt_user(fp,sizeof(pair)) || read(context,fp,pair,sizeof(pair)))break;
    if(BT_STRIP_USER_PC(pair[1])==BTQ_WAIT_RETURN+slide) {
     if(fp>=16 && btq_read(r,BTQ_FRAME,read,context,fp-16,saved,sizeof(saved))) {
      r->slot=saved[0];r->tail_slot=saved[1];r->anchor=fp;
     }
     break;
    }
    if(pair[0]<=fp || pair[0]-fp>1048576)break;
    fp=pair[0];
   }
   if(!(r->valid&BTQ_FRAME))r->failed|=BTQ_FRAME;
  }
 }
 if(!r->kind)return 0;
 if((r->kind&BTQ_WAIT) && (r->valid&BTQ_FRAME)) {
  btq_read(r,BTQ_VALUE,read,context,r->slot,&r->value,8);
  btq_read(r,BTQ_TAIL,read,context,r->tail_slot,&r->tail_value,8);
  if(r->tail_slot>=0x30)r->lane=r->tail_slot-0x30;
 } else if(r->kind&BTQ_ENQUEUE) {
  if(r->enqueuer>=0x30)r->lane=r->enqueuer-0x30;
 }
 if(btq_read(r,BTQ_LANE,read,context,r->lane,r->lane_words,sizeof(r->lane_words))) {
  const uint64_t label=r->lane_words[12];
  if(bt_user(label,48) && !read(context,label,r->label,48))r->valid|=BTQ_LABEL;
  else r->failed|=BTQ_LABEL;
  r->label[48]=0;
  for(unsigned i=0;i<48 && r->label[i];++i)
   if((unsigned char)r->label[i]<32 || (unsigned char)r->label[i]>=127)r->label[i]='.';
 }
 if((r->kind&BTQ_WAIT) && (r->valid&BTQ_FRAME) &&
    r->slot>=0x10 && r->slot!=r->lane+0x68) {
  r->node=r->slot-0x10;
  btq_read(r,BTQ_NODE,read,context,r->node,r->node_words,sizeof(r->node_words));
 }
 if((r->kind&BTQ_WAIT) && (r->valid&BTQ_FRAME)) {
  btq_read(r,BTQ_VALUE_REPEAT,read,context,r->slot,&r->value_after,8);
  btq_read(r,BTQ_TAIL_REPEAT,read,context,r->tail_slot,&r->tail_after,8);
 }
 if(bt_user(thread->tsd,0x3d8) && !(thread->tsd&7)) {
  btq_read(r,BTQ_QUEUE_REPEAT,read,context,thread->tsd+0xa0,&r->queue_after,8);
  if(btq_read(r,BTQ_TSD_REPEAT,read,context,thread->tsd+0x3c8,repeat,sizeof(repeat))) {
   r->dsc_after=repeat[0];r->enqueuer_after=repeat[1];
  }
 } else r->failed|=BTQ_QUEUE_REPEAT|BTQ_TSD_REPEAT;
 return 1;
}
static unsigned btq_rows(const struct btq_record *r,unsigned pid,char rows[][120],unsigned n,unsigned max) {
 const struct btq_thread *t=&r->thread;
 // Reserve a complete record. Partial fields must not masquerade as a
 // successful read or crowd out the existing CPU/wait records.
 const unsigned needed=10+((r->kind&BTQ_WAIT)?2:0)+((r->kind&BTQ_ENQUEUE)?1:0);
 if(max-n<needed) {
  if(n<max)snprintf(rows[n++],120,"QMISS p%u tid=%llx kind=%u rows=%u sampled=1",pid,t->tid,r->kind,needed);
  return n;
 }
 snprintf(rows[n++],120,"QTH p%u tid=%llx kind=%u valid=%x failed=%x ticks=%llx..%llx sampled=1",pid,t->tid,r->kind,r->valid,r->failed,r->ticks,r->finished);
 snprintf(rows[n++],120,"QSCHED tid=%llx st=%x hint=%u pri=%d runq=%d last=%d bound=%d chosen=%d",t->tid,t->state,t->hint,t->pri,t->runq,t->last,t->bound,t->chosen);
 snprintf(rows[n++],120,"QPC tid=%llx pc=%llx lr=%llx",t->tid,t->pc,t->lr);
 snprintf(rows[n++],120,"QREG tid=%llx sp=%llx fp=%llx x19=%llx x20=%llx x21=%llx",t->tid,t->sp,t->fp,t->x19,t->x20,t->x21);
 snprintf(rows[n++],120,"QTSD tid=%llx tsd=%llx q=%llx dsc=%llx enq=%llx",t->tid,t->tsd,r->queue,r->dsc,r->enqueuer);
 snprintf(rows[n++],120,"QREP tid=%llx q=%llx dsc=%llx enq=%llx",t->tid,r->queue_after,r->dsc_after,r->enqueuer_after);
 snprintf(rows[n++],120,"QLANE tid=%llx base=%llx tail=%llx head=%llx state=%llx",t->tid,r->lane,r->lane_words[6],r->lane_words[13],r->lane_words[7]);
 snprintf(rows[n++],120,"QOWNER tid=%llx flags=%llx target=%llx label=%llx",t->tid,r->lane_words[10],r->lane_words[3],r->lane_words[12]);
 snprintf(rows[n++],120,"QLABEL tid=%llx %.48s",t->tid,r->label);
 snprintf(rows[n++],120,"QNODE tid=%llx base=%llx flags=%llx next=%llx context=%llx owner=%llx",t->tid,r->node,r->node_words[0],r->node_words[2],r->node_words[4],r->node_words[7]);
 if(r->kind&BTQ_WAIT) {
  snprintf(rows[n++],120,"QWAIT tid=%llx slot=%llx tail_slot=%llx anchor=%llx",t->tid,r->slot,r->tail_slot,r->anchor);
  snprintf(rows[n++],120,"QVALUE tid=%llx before=%llx/%llx after=%llx/%llx",t->tid,r->value,r->tail_value,r->value_after,r->tail_after);
 }
 if(r->kind&BTQ_ENQUEUE)
  snprintf(rows[n++],120,"QPUSH tid=%llx x1=%llx x8=%llx x9=%llx x10=%llx",t->tid,t->x1,t->x8,t->x9,t->x10);
 return n;
}
#endif
