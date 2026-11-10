// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../include/Sm8850DsiFifo.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct model {
    uint32_t reg[256],fifo[2],written_reg[32],written_value[32];
    unsigned reads,writes,fail_read,fail_write,fifo_words,barriers,delays,triggers,trigger_order;
};
static unsigned sequence;
static bool rd(void *ctx,uint32_t reg,uint32_t *value) {
    struct model *m=ctx;assert(!(reg&3)&&reg<sizeof(m->reg));
    if(++m->reads==m->fail_read)return false;
    *value=m->reg[reg/4];return true;
}
static bool wr(void *ctx,uint32_t reg,uint32_t value) {
    struct model *m=ctx;assert(!(reg&3)&&reg<sizeof(m->reg)&&m->writes<32);
    m->written_reg[m->writes]=reg;m->written_value[m->writes++]=value;
    if(m->writes==m->fail_write)return false;
    if(reg==SM_DSI_INT) {
        uint32_t status=m->reg[reg/4]&UINT32_C(0x55555555);
        m->reg[reg/4]=(status&~value)|(value&UINT32_C(0xaaaaaaaa));
    } else if(reg==SM_DSI_FIFO) {
        assert(m->fifo_words<2);m->fifo[m->fifo_words++]=value;
    } else if(reg==SM_DSI_TRIGGER) {
        assert(m->fifo_words==2&&m->barriers&&value==1);++m->triggers;m->trigger_order=++sequence;
    } else m->reg[reg/4]=value;
    return true;
}
static void barrier(void *ctx){++((struct model *)ctx)->barriers;}
static void delay(void *ctx){struct model *m=ctx;assert(m->reg[SM_DSI_FIFO_RESET/4]==1&&m->barriers);++m->delays;}
static void setup(struct model *m,struct sm_dsi_transfer *t,struct sm_dsi_proof *p) {
    memset(m,0,sizeof(*m));memset(t,0,sizeof(*t));memset(p,0,sizeof(*p));
    // Model-only identities;not measured hardware values.
    m->reg[SM_DSI_HW/4]=UINT32_C(0x200a0000);m->reg[SM_DSI_CTRL/4]=5;
    m->reg[SM_DSI_DMA_CTRL/4]=0x400;m->reg[SM_DSI_DMA_CTRL1/4]=0x10;
    m->reg[SM_DSI_LENGTH/4]=128;
    m->reg[SM_DSI_INT/4]=UINT32_C(0x02020202)|UINT32_C(0x100);
    t->io=(struct sm_dsi_io){m,rd,wr,barrier,delay};
    *p=(struct sm_dsi_proof){42,UINT32_C(0x200a0000),5,0x400,true,true,true,false};
}
static const uint8_t packet[8]={3,0,0x39,0xc0,0x51,4,0x59,0xff};
static void after_failure(struct sm_dsi_transfer *t,struct model *m,struct sm_dsi_proof *p) {
    unsigned n=m->writes;assert(t->poisoned&&!t->active);
    assert(sm_dsi_poll(t,p->lease_epoch,9000)==SM_DSI_REFUSED);
    assert(sm_dsi_begin(t,p,packet,9000)==SM_DSI_REFUSED&&m->writes==n);
}
int main(void) {
    struct model m;struct sm_dsi_transfer t;struct sm_dsi_proof p;unsigned cases=0;
    setup(&m,&t,&p);uint32_t before[256];memcpy(before,m.reg,sizeof(before));
    assert(sm_dsi_begin(&t,&p,packet,100)==SM_DSI_SENT);
    assert(m.triggers==1&&m.fifo[0]==UINT32_C(0xc0390003)&&m.fifo[1]==UINT32_C(0xff590451));
    assert(sm_dsi_poll(&t,42,200)==SM_DSI_WAIT);
    unsigned n=m.writes;assert(sm_dsi_begin(&t,&p,packet,200)==SM_DSI_REFUSED&&n==m.writes);
    m.reg[SM_DSI_INT/4]|=1;m.reg[SM_DSI_STATUS/4]=2;
    assert(sm_dsi_poll(&t,42,201)==SM_DSI_COMPLETE&&!t.active&&!t.poisoned&&m.delays==1);
    assert(m.reg[SM_DSI_STATUS/4]==2); // DMA_DONE completion does not rewrite DMA_BUSY.
    assert(m.reg[SM_DSI_INT/4]==before[SM_DSI_INT/4]);
    for(unsigned i=0;i<256;++i)if(i!=SM_DSI_FIFO/4&&i!=SM_DSI_TRIGGER/4&&i!=SM_DSI_STATUS/4)assert(m.reg[i]==before[i]);
    ++cases;
    for(unsigned guard=0;guard<16;++guard) {
        setup(&m,&t,&p);
        switch(guard) {
        case 0:p.lease_epoch=0;break;
        case 1:p.route_qualified=false;break;
        case 2:p.clocks_owned=false;break;
        case 3:p.exclusive_command_owner=false;break;
        case 4:p.hardware_version=0;break;
        case 5:p.hardware_version=UINT32_MAX;break;
        case 6:++p.hardware_version;break;
        case 7:p.control=7;break;
        case 8:m.reg[SM_DSI_CTRL/4]=p.control=0;break;
        case 9:++p.dma_control;break;
        case 10:m.reg[SM_DSI_STATUS/4]=4;break;
        case 11:m.reg[SM_DSI_INT/4]|=1;break;
        case 12:m.reg[SM_DSI_INT/4]|=UINT32_C(0x01000000);break;
        case 13:m.reg[SM_DSI_TPG/4]=1;break;
        case 14:m.reg[SM_DSI_DMA_CTRL1/4]|=2;break;
        case 15:m.reg[SM_DSI_DMA_CTRL/4]=p.dma_control=UINT32_C(0x80000400);break;
        }
        assert(sm_dsi_begin(&t,&p,packet,0)==SM_DSI_REFUSED&&!m.writes);++cases;
    }
    for(unsigned i=1;i<=8;++i) {
        setup(&m,&t,&p);m.fail_read=i;
        assert(sm_dsi_begin(&t,&p,packet,0)==SM_DSI_REFUSED&&!m.writes&&!t.poisoned);++cases;
    }
    for(unsigned i=1;i<=8;++i) {
        setup(&m,&t,&p);m.fail_write=i;
        assert(sm_dsi_begin(&t,&p,packet,0)==SM_DSI_UNCERTAIN&&m.writes==i);after_failure(&t,&m,&p);++cases;
    }
    for(unsigned i=9;i<=16;++i) {
        setup(&m,&t,&p);m.fail_write=i;assert(sm_dsi_begin(&t,&p,packet,0)==SM_DSI_SENT);
        m.reg[SM_DSI_INT/4]|=1;
        assert(sm_dsi_poll(&t,42,1)==SM_DSI_UNCERTAIN&&m.writes==i);after_failure(&t,&m,&p);++cases;
    }
    for(unsigned fault=0;fault<5;++fault) {
        setup(&m,&t,&p);assert(sm_dsi_begin(&t,&p,packet,0)==SM_DSI_SENT);n=m.writes;
        if(fault==0)m.reg[SM_DSI_INT/4]|=UINT32_C(0x01000000);
        if(fault==1)m.reg[SM_DSI_STATUS/4]|=UINT32_C(0x80000000);
        if(fault==2)m.fail_read=m.reads+1;
        int r=sm_dsi_poll(&t,fault==3?41:42,fault==4?5000:1);
        assert(r==(fault==4?SM_DSI_TIMEOUT:SM_DSI_UNCERTAIN)&&m.writes==n);
        after_failure(&t,&m,&p);++cases;
    }
    setup(&m,&t,&p);uint8_t bad[8];memcpy(bad,packet,8);bad[3]=0xe0;
    assert(sm_dsi_begin(&t,&p,bad,0)==SM_DSI_REFUSED&&!m.reads&&!m.writes);++cases;
    for(unsigned step=1;step<=5;++step) {
        setup(&m,&t,&p);assert(sm_dsi_begin(&t,&p,packet,0)==SM_DSI_SENT);
        m.reg[SM_DSI_INT/4]|=1;m.fail_read=m.reads+2+step;
        assert(sm_dsi_poll(&t,42,1)==SM_DSI_UNCERTAIN);after_failure(&t,&m,&p);++cases;
    }
    struct model slave;struct sm_dsi_transfer st;struct sm_dsi_proof sp;
    setup(&m,&t,&p);setup(&slave,&st,&sp);
    assert(sm_dsi_pair_begin(&t,&p,&st,&sp,packet,0)==SM_DSI_SENT);
    assert(slave.trigger_order<m.trigger_order&&slave.triggers==1&&m.triggers==1);
    assert((m.reg[SM_DSI_DMA_CTRL/4]&UINT32_C(0xc0000000))==UINT32_C(0xc0000000));
    assert((slave.reg[SM_DSI_DMA_CTRL/4]&UINT32_C(0xc0000000))==UINT32_C(0x80000000));
    m.reg[SM_DSI_INT/4]|=1;n=m.writes;unsigned sn=slave.writes;
    assert(sm_dsi_pair_poll(&t,&st,42,100)==SM_DSI_WAIT&&m.writes==n&&slave.writes==sn);
    slave.reg[SM_DSI_INT/4]|=1;
    assert(sm_dsi_pair_poll(&t,&st,42,101)==SM_DSI_COMPLETE&&!t.active&&!st.active);
    assert(m.reg[SM_DSI_DMA_CTRL/4]==0x400&&slave.reg[SM_DSI_DMA_CTRL/4]==0x400);++cases;
    for(unsigned step=1;step<=8;++step) {
        setup(&m,&t,&p);setup(&slave,&st,&sp);slave.fail_read=step;
        assert(sm_dsi_pair_begin(&t,&p,&st,&sp,packet,0)==SM_DSI_REFUSED&&!m.writes&&!slave.writes&&!t.prepared);++cases;
    }
    for(unsigned who=0;who<2;++who)for(unsigned step=1;step<=8;++step) {
        setup(&m,&t,&p);setup(&slave,&st,&sp);(who?&slave:&m)->fail_write=step;
        assert(sm_dsi_pair_begin(&t,&p,&st,&sp,packet,0)==SM_DSI_UNCERTAIN);
        assert(t.poisoned&&st.poisoned&&!m.triggers);
        assert(slave.triggers==(who==0&&step==8?1u:0u));++cases;
    }
    for(unsigned who=0;who<2;++who)for(unsigned step=9;step<=16;++step) {
        setup(&m,&t,&p);setup(&slave,&st,&sp);(who?&slave:&m)->fail_write=step;
        assert(sm_dsi_pair_begin(&t,&p,&st,&sp,packet,0)==SM_DSI_SENT);
        m.reg[SM_DSI_INT/4]|=1;slave.reg[SM_DSI_INT/4]|=1;
        assert(sm_dsi_pair_poll(&t,&st,42,100)==SM_DSI_UNCERTAIN&&t.poisoned&&st.poisoned);++cases;
    }
    setup(&m,&t,&p);setup(&slave,&st,&sp);sp.lease_epoch=43;
    assert(sm_dsi_pair_begin(&t,&p,&st,&sp,packet,0)==SM_DSI_REFUSED&&!m.reads&&!slave.reads);++cases;
    setup(&m,&t,&p);setup(&slave,&st,&sp);
    assert(sm_dsi_pair_begin(&t,&p,&st,&sp,packet,0)==SM_DSI_SENT);n=m.writes;sn=slave.writes;
    m.reg[SM_DSI_INT/4]|=1;
    assert(sm_dsi_pair_poll(&t,&st,42,5000)==SM_DSI_TIMEOUT&&t.poisoned&&st.poisoned);
    assert(m.writes==n&&slave.writes==sn);++cases;
    for(unsigned low_power=0;low_power<2;++low_power) {
        setup(&m,&t,&p);p.dma_control=m.reg[SM_DSI_DMA_CTRL/4]=UINT32_C(0x14000000);p.use_lpm=low_power!=0;
        assert(sm_dsi_begin(&t,&p,packet,0)==SM_DSI_SENT);
        assert(((m.reg[SM_DSI_DMA_CTRL/4]>>26)&1)==low_power);
        m.reg[SM_DSI_INT/4]|=1;assert(sm_dsi_poll(&t,42,100)==SM_DSI_COMPLETE);
        assert(m.reg[SM_DSI_DMA_CTRL/4]==UINT32_C(0x14000000));++cases;
    }
    setup(&m,&t,&p);setup(&slave,&st,&sp);sp.use_lpm=true;
    assert(sm_dsi_pair_begin(&t,&p,&st,&sp,packet,0)==SM_DSI_REFUSED&&!m.reads&&!slave.reads);++cases;
    printf("PASS %u actual-source controller model cases;no hardware qualification\n",cases);
    return 0;
}
