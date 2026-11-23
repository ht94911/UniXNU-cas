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
 uint64_t bsd[32];
 unsigned state,hint;int pri,runq,last,bound,chosen;
};
// Reuse scalar BSD wait data copied by the existing fatal snapshot helper.
// The old node may be outside its owner's current sync frame, so a current
// TSD pointer alone cannot identify who created the pending stack node.
enum {BTQ_PEER_PORT=1u,BTQ_PEER_STACK=2u};
struct btq_peer {
 struct btq_thread thread;
 uint64_t dsc,dsc_after,words[16],frames[8];
 unsigned match,valid,failed;
};
struct btq_record {
 struct btq_thread thread;
 uint64_t ticks,finished;
 unsigned kind,valid,failed;
 uint64_t queue,queue_after,dsc,dsc_after,enqueuer,enqueuer_after;
 uint64_t slot,tail_slot,anchor,value,tail_value,value_after,tail_after;
 uint64_t lane,lane_words[16],node,node_words[16];
 struct btq_peer peers[2];
 unsigned peer_count,peer_candidates,peer_omitted;
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
  // The inspected LP64 lane label is at +0x48; +0x60 is sref count.
  const uint64_t label=r->lane_words[9];
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
static void btq_capture_peers(struct btq_record *r,const struct btq_thread *threads,
                             const struct bt_entry *entries,unsigned count,
                             bt_reader read,void *context) {
 if(!(r->kind&BTQ_WAIT) || !(r->valid&BTQ_NODE))return;
 const unsigned port=(unsigned)(r->node_words[12]>>32);
 for(unsigned i=0;i<count && i<BT_MAX_THREADS;++i) {
  const struct btq_thread *t=&threads[i];
  // BSD copied port/stack scalars for every stable waiting thread. Its
  // uu_save fields remain gated by bsd[13]: an owner may already have
  // returned to an idle wait while the old node is still queued.
  if((t->state&0x35u)!=1u)continue;
  const uint64_t base=t->bsd[2];
  unsigned match=0;
  if(port && port==(unsigned)t->bsd[3])match|=BTQ_PEER_PORT;
  if(base && r->node>=base && bt_user(t->tsd,8) &&
     r->node<t->tsd && t->tsd-r->node>=sizeof(r->node_words))match|=BTQ_PEER_STACK;
  if(!match)continue;
  ++r->peer_candidates;
  if(r->peer_count>=2) {++r->peer_omitted;continue;}
  struct btq_peer *p=&r->peers[r->peer_count++];
  memset(p,0,sizeof(*p));
  p->thread=*t;p->match=match;
  memcpy(p->frames,entries[i].frames,sizeof(p->frames));
  // These reads follow the scheduler/task unlock, just like the lane read.
  // Preserve changed pointers and failed reads instead of joining samples.
  if(bt_user(t->tsd,0x3d0) && !(t->tsd&7)) {
   if(!read(context,t->tsd+0x3c8,&p->dsc,8))p->valid|=1u;
   else p->failed|=1u;
   if((p->valid&1u) && !(p->dsc&15) && p->dsc>=t->sp &&
      p->dsc>=base && p->dsc<t->tsd && t->tsd-p->dsc>=sizeof(p->words) &&
      bt_user(p->dsc,sizeof(p->words))) {
    if(!read(context,p->dsc,p->words,sizeof(p->words)))p->valid|=2u;
    else p->failed|=2u;
   } else p->failed|=2u;
   if(!read(context,t->tsd+0x3c8,&p->dsc_after,8))p->valid|=4u;
   else p->failed|=4u;
  } else p->failed|=7u;
 }
}
static unsigned btq_rows(const struct btq_record *r,unsigned pid,char rows[][120],unsigned n,unsigned max) {
 const struct btq_thread *t=&r->thread;
 // Reserve a complete record. Partial fields must not masquerade as a
 // successful read or crowd out the existing CPU/wait records.
 const unsigned needed=10+((r->kind&BTQ_WAIT)?6:0)+((r->kind&BTQ_ENQUEUE)?1:0)+12*r->peer_count;
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
 snprintf(rows[n++],120,"QOWNER tid=%llx flags=%llx target=%llx label=%llx",t->tid,r->lane_words[10],r->lane_words[3],r->lane_words[9]);
 snprintf(rows[n++],120,"QLABEL tid=%llx %.48s",t->tid,r->label);
 snprintf(rows[n++],120,"QNODE tid=%llx base=%llx flags=%llx next=%llx context=%llx owner=%llx",t->tid,r->node,r->node_words[0],r->node_words[2],r->node_words[4],r->node_words[7]);
 if(r->kind&BTQ_WAIT) {
  snprintf(rows[n++],120,"QWAIT tid=%llx slot=%llx tail_slot=%llx anchor=%llx",t->tid,r->slot,r->tail_slot,r->anchor);
  snprintf(rows[n++],120,"QVALUE tid=%llx before=%llx/%llx after=%llx/%llx",t->tid,r->value,r->tail_value,r->value_after,r->tail_after);
  for(unsigned w=4;w<16;w+=4)
   snprintf(rows[n++],120,"QNRAW tid=%llx word=%u %llx %llx %llx %llx",t->tid,w,r->node_words[w],r->node_words[w+1],r->node_words[w+2],r->node_words[w+3]);
  snprintf(rows[n++],120,"QPEERS tid=%llx port=%x candidates=%u kept=%u omitted=%u sampled=1",t->tid,(unsigned)(r->node_words[12]>>32),r->peer_candidates,r->peer_count,r->peer_omitted);
 }
 if(r->kind&BTQ_ENQUEUE)
  snprintf(rows[n++],120,"QPUSH tid=%llx x1=%llx x8=%llx x9=%llx x10=%llx",t->tid,t->x1,t->x8,t->x9,t->x10);
 for(unsigned i=0;i<r->peer_count;++i) {
  const struct btq_peer *p=&r->peers[i];const struct btq_thread *q=&p->thread;
  snprintf(rows[n++],120,"QPEER tid=%llx waiter=%llx match=%u st=%x hint=%u syscall=%llu",t->tid,q->tid,p->match,q->state,q->hint,q->bsd[0]);
  snprintf(rows[n++],120,"QPSTACK tid=%llx base=%llx port=%llx tsd=%llx",q->tid,q->bsd[2],q->bsd[3],q->tsd);
  snprintf(rows[n++],120,"QPSP tid=%llx sp=%llx fp=%llx pc=%llx lr=%llx",q->tid,q->sp,q->fp,q->pc,q->lr);
  snprintf(rows[n++],120,"QPDSC tid=%llx before=%llx after=%llx valid=%x failed=%x",q->tid,p->dsc,p->dsc_after,p->valid,p->failed);
  for(unsigned w=0;w<16;w+=4)
   snprintf(rows[n++],120,"QDRAW tid=%llx word=%u %llx %llx %llx %llx",q->tid,w,p->words[w],p->words[w+1],p->words[w+2],p->words[w+3]);
  snprintf(rows[n++],120,"QPBSD tid=%llx typed=%llu event=%llx flags=%llx lane=%llx fflags=%llx",q->tid,q->bsd[13],q->bsd[14],q->bsd[15],q->bsd[16],q->bsd[17]);
  snprintf(rows[n++],120,"QPBSDX tid=%llx addr=%llx mask=%llx value=%llx output=%llx",q->tid,q->bsd[20],q->bsd[21],q->bsd[22],q->bsd[26]);
  for(unsigned w=0;w<8;w+=4)
   snprintf(rows[n++],120,"QPFRAME tid=%llx word=%u %llx %llx %llx %llx",q->tid,w,p->frames[w],p->frames[w+1],p->frames[w+2],p->frames[w+3]);
 }
 return n;
}
#endif
