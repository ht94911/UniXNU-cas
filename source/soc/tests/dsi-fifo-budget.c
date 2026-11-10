// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the real existing model callbacks while recording every IO operation.
#define main legacy_dsi_model_main
#include "dsi-fifo.c"
#undef main

struct io_event {char kind;uint32_t offset,value;bool success;};
struct observed_model {
    struct model base;
    struct io_event events[128];
    unsigned count;
};
static void event(struct observed_model *m,char kind,uint32_t offset,uint32_t value,bool success) {
    assert(m->count<sizeof(m->events)/sizeof(m->events[0]));
    m->events[m->count++]=(struct io_event){kind,offset,value,success};
}
static bool observed_read(void *cookie,uint32_t offset,uint32_t *value) {
    struct observed_model *m=cookie;*value=0;
    bool success=rd(&m->base,offset,value);event(m,'r',offset,*value,success);return success;
}
static bool observed_write(void *cookie,uint32_t offset,uint32_t value) {
    struct observed_model *m=cookie;
    bool success=wr(&m->base,offset,value);event(m,'w',offset,value,success);return success;
}
static void observed_barrier(void *cookie) {
    struct observed_model *m=cookie;barrier(&m->base);event(m,'b',0,0,true);
}
static void observed_delay(void *cookie) {
    struct observed_model *m=cookie;delay(&m->base);event(m,'d',0,1,true);
}
static void observed_setup(struct observed_model *m,struct sm_dsi_transfer *t,struct sm_dsi_proof *p) {
    memset(m,0,sizeof(*m));setup(&m->base,t,p);
    t->io=(struct sm_dsi_io){m,observed_read,observed_write,observed_barrier,observed_delay};
}
static void sent(struct observed_model *m,struct sm_dsi_transfer *t,struct sm_dsi_proof *p,uint64_t began) {
    observed_setup(m,t,p);assert(sm_dsi_begin(t,p,packet,began)==SM_DSI_SENT);
}
static void same_io(const struct observed_model *a,const struct observed_model *b) {
    assert(a->count==b->count&&a->base.reads==b->base.reads&&a->base.writes==b->base.writes);
    assert(a->base.barriers==b->base.barriers&&a->base.delays==b->base.delays&&a->base.triggers==b->base.triggers);
    for(unsigned i=0;i<a->count;++i) {
        assert(a->events[i].kind==b->events[i].kind&&a->events[i].offset==b->events[i].offset);
        assert(a->events[i].value==b->events[i].value&&a->events[i].success==b->events[i].success);
    }
    assert(!memcmp(a->base.reg,b->base.reg,sizeof(a->base.reg)));
}
static void no_retry(struct observed_model *m,struct sm_dsi_transfer *t,struct sm_dsi_proof *p) {
    const unsigned count=m->count;
    assert(t->poisoned&&!t->active&&!t->prepared);
    assert(sm_dsi_poll_budget(t,42,UINT64_C(2000000),UINT64_C(1200000))==SM_DSI_REFUSED);
    assert(sm_dsi_begin(t,p,packet,UINT64_C(2000000))==SM_DSI_REFUSED);
    assert(m->count==count&&m->base.triggers==1);
}
int main(void) {
    struct observed_model m,other;struct sm_dsi_transfer t,ot;struct sm_dsi_proof p,op;
    unsigned cases=0;
    sent(&m,&t,&p,1000);sent(&other,&ot,&op,1000);
    assert(sm_dsi_observe(&t,42,5999)==SM_DSI_WAIT);
    assert(sm_dsi_observe_budget(&ot,42,5999,5000)==SM_DSI_WAIT);
    assert(sm_dsi_observe(&t,42,6000)==SM_DSI_TIMEOUT);
    assert(sm_dsi_observe_budget(&ot,42,6000,5000)==SM_DSI_TIMEOUT);
    same_io(&m,&other);assert(m.base.writes==8);no_retry(&m,&t,&p);++cases;
    sent(&m,&t,&p,1000);sent(&other,&ot,&op,1000);
    assert(sm_dsi_poll(&t,42,5999)==SM_DSI_WAIT);
    assert(sm_dsi_poll_budget(&ot,42,5999,5000)==SM_DSI_WAIT);
    assert(sm_dsi_poll(&t,42,6000)==SM_DSI_TIMEOUT);
    assert(sm_dsi_poll_budget(&ot,42,6000,5000)==SM_DSI_TIMEOUT);
    same_io(&m,&other);no_retry(&m,&t,&p);++cases;
    sent(&m,&t,&p,1000);sent(&other,&ot,&op,1000);
    m.base.reg[SM_DSI_INT/4]|=1;other.base.reg[SM_DSI_INT/4]|=1;
    m.base.reg[SM_DSI_STATUS/4]=other.base.reg[SM_DSI_STATUS/4]=2;
    const unsigned before=m.count;
    assert(sm_dsi_observe(&t,42,6000)==SM_DSI_COMPLETE);
    assert(sm_dsi_observe_budget(&ot,42,6000,5000)==SM_DSI_COMPLETE);
    assert(t.active&&!t.poisoned&&m.base.writes==8&&m.count==before+2);
    assert(m.events[before].offset==SM_DSI_INT&&m.events[before+1].offset==SM_DSI_STATUS);
    same_io(&m,&other);++cases;
    sent(&m,&t,&p,1000);sent(&other,&ot,&op,1000);
    m.base.reg[SM_DSI_INT/4]|=1;other.base.reg[SM_DSI_INT/4]|=1;
    m.base.reg[SM_DSI_STATUS/4]=other.base.reg[SM_DSI_STATUS/4]=2;
    assert(sm_dsi_poll(&t,42,1100)==SM_DSI_COMPLETE);
    assert(sm_dsi_poll_budget(&ot,42,1100,5000)==SM_DSI_COMPLETE);
    same_io(&m,&other);
    assert(!t.active&&!t.poisoned&&m.base.reads==15&&m.base.writes==16&&m.base.delays==1);
    assert(m.base.reg[SM_DSI_STATUS/4]==2);++cases;
    sent(&m,&t,&p,1000);
    const unsigned writes=m.base.writes;
    for(uint64_t elapsed=100000;elapsed<1200000;elapsed+=100000)
        assert(sm_dsi_poll_budget(&t,42,1000+elapsed,1200000)==SM_DSI_WAIT&&m.base.writes==writes);
    assert(sm_dsi_poll_budget(&t,42,1200999,1200000)==SM_DSI_WAIT&&m.base.writes==writes);
    assert(sm_dsi_poll_budget(&t,42,1201000,1200000)==SM_DSI_TIMEOUT&&m.base.writes==writes);
    assert(t.began_us==1000);no_retry(&m,&t,&p);++cases;
    sent(&m,&t,&p,1000);m.base.reg[SM_DSI_INT/4]|=1;m.base.reg[SM_DSI_STATUS/4]=2;
    assert(sm_dsi_poll_budget(&t,42,501000,1200000)==SM_DSI_COMPLETE);
    assert(m.base.reads==15&&m.base.writes==16&&!t.poisoned&&m.base.reg[SM_DSI_STATUS/4]==2);++cases;
    sent(&m,&t,&p,1000);m.base.reg[SM_DSI_INT/4]|=1;
    assert(sm_dsi_poll_budget(&t,42,1201000,1200000)==SM_DSI_COMPLETE&&!t.poisoned);++cases;
    sent(&m,&t,&p,1000);m.base.reg[SM_DSI_INT/4]|=1;m.base.reg[SM_DSI_STATUS/4]=UINT32_C(0x80000002);
    assert(sm_dsi_poll_budget(&t,42,1100,1200000)==SM_DSI_UNCERTAIN&&m.base.writes==8);
    assert(m.base.reads==10);no_retry(&m,&t,&p);++cases;
    sent(&m,&t,&p,1000);m.base.reg[SM_DSI_INT/4]|=UINT32_C(0x01000001);
    assert(sm_dsi_poll_budget(&t,42,1100,1200000)==SM_DSI_UNCERTAIN&&m.base.writes==8);
    assert(m.base.reads==10);no_retry(&m,&t,&p);++cases;
    sent(&m,&t,&p,1000);m.base.reg[SM_DSI_INT/4]|=1;m.base.fail_read=m.base.reads+1;
    assert(sm_dsi_poll_budget(&t,42,1100,1200000)==SM_DSI_UNCERTAIN&&m.base.writes==8);
    assert(m.base.reads==9);no_retry(&m,&t,&p);++cases;
    sent(&m,&t,&p,1000);m.base.reg[SM_DSI_INT/4]|=1;m.base.fail_read=m.base.reads+2;
    assert(sm_dsi_poll_budget(&t,42,1100,1200000)==SM_DSI_UNCERTAIN&&m.base.writes==8);
    assert(m.base.reads==10);no_retry(&m,&t,&p);++cases;
    observed_setup(&m,&t,&p);m.base.reg[SM_DSI_STATUS/4]=2;
    assert(sm_dsi_begin(&t,&p,packet,1000)==SM_DSI_REFUSED&&!m.base.writes&&!m.base.triggers);++cases;
    sent(&m,&t,&p,1000);const unsigned origin_count=m.count;
    t.began_us=2000;assert(m.count==origin_count); // Caller rebases after trigger without transport IO.
    assert(sm_dsi_poll_budget(&t,42,1201999,1200000)==SM_DSI_WAIT);
    assert(sm_dsi_poll_budget(&t,42,1202000,1200000)==SM_DSI_TIMEOUT);
    no_retry(&m,&t,&p);++cases;
    sent(&m,&t,&p,1000);
    assert(sm_dsi_poll_budget(&t,42,1000,0)==SM_DSI_TIMEOUT&&m.base.writes==8);
    no_retry(&m,&t,&p);++cases;
    sent(&m,&t,&p,1000);m.base.reg[SM_DSI_INT/4]|=1;
    assert(sm_dsi_poll_budget(&t,42,1000,0)==SM_DSI_COMPLETE);++cases;
    sent(&m,&t,&p,UINT64_MAX-20);
    assert(sm_dsi_poll_budget(&t,42,28,50)==SM_DSI_WAIT);
    assert(sm_dsi_poll_budget(&t,42,29,50)==SM_DSI_TIMEOUT);no_retry(&m,&t,&p);++cases;
    observed_setup(&m,&t,&p);observed_setup(&other,&ot,&op);
    assert(sm_dsi_pair_begin(&t,&p,&ot,&op,packet,1000)==SM_DSI_SENT);
    m.base.reg[SM_DSI_INT/4]|=1;other.base.reg[SM_DSI_INT/4]|=1;
    m.base.reg[SM_DSI_STATUS/4]=other.base.reg[SM_DSI_STATUS/4]=2;
    assert(sm_dsi_pair_poll(&t,&ot,42,6000)==SM_DSI_COMPLETE&&!t.active&&!ot.active);
    assert(m.base.reg[SM_DSI_STATUS/4]==2&&other.base.reg[SM_DSI_STATUS/4]==2);++cases;
    observed_setup(&m,&t,&p);observed_setup(&other,&ot,&op);
    assert(sm_dsi_pair_begin(&t,&p,&ot,&op,packet,1000)==SM_DSI_SENT);
    m.base.reg[SM_DSI_INT/4]|=1;const unsigned master_writes=m.base.writes,slave_writes=other.base.writes;
    assert(sm_dsi_pair_poll(&t,&ot,42,5999)==SM_DSI_WAIT);
    assert(sm_dsi_pair_poll(&t,&ot,42,6000)==SM_DSI_TIMEOUT&&t.poisoned&&ot.poisoned);
    assert(m.base.writes==master_writes&&other.base.writes==slave_writes);++cases;
    printf("PASS %u actual-source budget/DONE model cases;no hardware qualification\n",cases);
    return 0;
}
