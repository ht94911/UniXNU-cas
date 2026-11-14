// SPDX-License-Identifier: MIT
// Copy the original tablet worker's published samples. No new task traversal,
// VM reads, thread suspension, or Mach port lookup is performed by this adapter.
#ifndef UMI_WAIT_TRACE_H
#define UMI_WAIT_TRACE_H
#if UMI_RUNTIME_TRACE
extern uint64_t boot_progress_epoch,boot_progress_frequency;
static unsigned umi_wait_started,umi_wait_published_start,umi_wait_published_end,umi_wait_generation;
static unsigned umi_wait_now(void) {
#ifdef UMI_WAIT_HOST
 return umi_wait_host_seconds;
#else
 uint64_t c;__asm__ volatile("isb\n\tmrs %0,cntpct_el0":"=r"(c));
 return boot_progress_frequency?(unsigned)((c-boot_progress_epoch)/boot_progress_frequency):0;
#endif
}
static void umi_wait_capture_begin(void){umi_wait_started=umi_wait_now();}
// Called by the original publisher while it holds trace_lock.
static void umi_wait_publish(void) {
 umi_wait_published_start=umi_wait_started;umi_wait_published_end=umi_wait_now();++umi_wait_generation;
}
static void umi_wait_group(uint64_t *v,const struct bt_entry *e) {
 const uint64_t meta[]={e->index,e->count,e->error,(uint32_t)e->peer,e->peer_code,
  e->pc,e->lr,e->fp,e->trap,e->regs[1],e->regs[3],e->regs[4],e->regs[5],(unsigned char)e->role};
 memcpy(v,meta,sizeof(meta));memcpy(v+14,e->frames,8*sizeof(uint64_t));
}
// All eight records use one published generation. A busy lock is retried by
// the caller, without changing sequence numbers or serializing partial data.
int umi_ui_wait_snapshot(unsigned first,unsigned seconds,uint64_t rows[8][64]) {
 if(!first || first>248 || __atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return 0;
 memset(rows,0,8*64*sizeof(uint64_t));
 for(unsigned role=0;role<4;++role) {
  const struct bt_main *m=&published_main[role];const struct bt_entry *e=&m->entry;
  unsigned groups=0;char letter=bt_role_char(role,m);
  for(unsigned i=0;i<published.count;++i)if(published.entries[i].role==letter && published.entries[i].index!=1)++groups;
  for(unsigned kind=0;kind<2;++kind) {
   uint64_t *v=rows[role*2+kind];
   const uint64_t header[]={1,first+role*2+kind,seconds,umi_wait_published_start,umi_wait_published_end,
    umi_wait_generation,role,kind+1,published_pid[role],m->pid!=0};
   memcpy(v,header,sizeof(header));memcpy(v+10,published_names[role],32);
   v[14]=published_busy;v[15]=published_omitted;v[16]=groups;v[17]=published.threads;
  }
  uint64_t *v=rows[role*2];
  const uint64_t main[]={e->pc,e->lr,e->fp,e->trap,(uint32_t)e->peer,e->peer_code,
   m->error,m->image_error,m->image,m->slide,e->regs[1],e->regs[3],e->regs[4],e->regs[5]};
  memcpy(v+18,main,sizeof(main));memcpy(v+32,m->frames,sizeof(m->frames));
  // Two representative non-main groups per role: known outgoing Mach peers
  // first, then remaining groups in original order. Report the total and
  // selected counts explicitly; never claim this is every thread's stack.
  v=rows[role*2+1];unsigned n=0;
  for(unsigned priority=0;priority<2;++priority)for(unsigned i=0;i<published.count && n<2;++i) {
   const struct bt_entry *g=&published.entries[i];
   if(g->role!=letter || g->index==1)continue;
   unsigned rank=g->peer_code==0 && g->peer>0?0:1;
   if(rank!=priority)continue;
   umi_wait_group(v+20+n*22,g);++n;
  }
  v[18]=n;v[19]=groups>n?groups-n:0;
 }
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);return 1;
}
#else
static void umi_wait_capture_begin(void){}
static void umi_wait_publish(void){}
#endif
#endif
