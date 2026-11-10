// SPDX-License-Identifier: BSD-2-Clause-Patent
// Source-only bounded embedded command FIFO transport. Register protocol:
// OnePlusOSS SM8850 display-drivers5ab2a689ff87d7d28c511f1762cf41c1b90d965a
// dsi_ctrl_hw_cmn.c,dsi_ctrl_reg.h. No physical addresses or clock/reset votes.
// A platform adapter must prove the inherited controller state and exclusive
// command ownership before supplying callbacks and a nonzero lease epoch.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SM_DSI_HW=0x000,SM_DSI_CTRL=0x004,SM_DSI_STATUS=0x008,
    SM_DSI_DMA_CTRL=0x03c,SM_DSI_LENGTH=0x04c,SM_DSI_TRIGGER=0x090,
    SM_DSI_INT=0x110,SM_DSI_TPG=0x15c,SM_DSI_FIFO=0x17c,
    SM_DSI_FIFO_RESET=0x1ec,SM_DSI_DMA_CTRL1=0x390
};
enum sm_dsi_result {
    SM_DSI_REFUSED=-1,SM_DSI_UNCERTAIN=-2,SM_DSI_TIMEOUT=-3,
    SM_DSI_WAIT=0,SM_DSI_SENT=1,SM_DSI_COMPLETE=2
};
struct sm_dsi_io {
    void *context;
    bool (*read32)(void *,uint32_t,uint32_t *);
    bool (*write32)(void *,uint32_t,uint32_t);
    void (*barrier)(void *);
    void (*delay_one_us)(void *);
};
struct sm_dsi_proof {
    uint64_t lease_epoch;
    uint32_t hardware_version,control,dma_control;
    bool route_qualified,clocks_owned,exclusive_command_owner;
    bool use_lpm;
};
struct sm_dsi_transfer {
    struct sm_dsi_io io;
    uint64_t lease_epoch,began_us;
    uint32_t dma_control,dma_control1,length,interrupt_masks,tpg;
    bool active,poisoned,prepared,use_lpm;
};
static inline bool sm_dsi_read(struct sm_dsi_transfer *t,uint32_t reg,uint32_t *v) {
    return t->io.read32(t->io.context,reg,v);
}
static inline bool sm_dsi_write(struct sm_dsi_transfer *t,uint32_t reg,uint32_t v) {
    return t->io.write32(t->io.context,reg,v);
}
static inline int sm_dsi_uncertain(struct sm_dsi_transfer *t,int result) {
    t->poisoned=true;t->active=false;t->prepared=false;return result;
}
// Read both controllers before writing either in a broadcast transaction.
static inline int sm_dsi_preflight(struct sm_dsi_transfer *t,
                               const struct sm_dsi_proof *p,
                               const uint8_t packet[8],uint64_t now_us) {
    if(!t || !p || !packet || t->active || t->poisoned || t->prepared ||
       !t->io.read32 || !t->io.write32 || !t->io.barrier || !t->io.delay_one_us ||
       !p->lease_epoch || !p->hardware_version || p->hardware_version==UINT32_MAX ||
       !p->route_qualified || !p->clocks_owned || !p->exclusive_command_owner)
        return SM_DSI_REFUSED;
    if(packet[0]!=3 || packet[1] || (packet[2]&63)!=0x39 || packet[3]!=0xc0 || packet[4]!=0x51)
        return SM_DSI_REFUSED;
    uint32_t version,control,status,ints,tpg,dma,dma1,length;
    if(!sm_dsi_read(t,SM_DSI_HW,&version) || !sm_dsi_read(t,SM_DSI_CTRL,&control) ||
       !sm_dsi_read(t,SM_DSI_STATUS,&status) || !sm_dsi_read(t,SM_DSI_INT,&ints) ||
       !sm_dsi_read(t,SM_DSI_TPG,&tpg) || !sm_dsi_read(t,SM_DSI_DMA_CTRL,&dma) ||
       !sm_dsi_read(t,SM_DSI_DMA_CTRL1,&dma1) || !sm_dsi_read(t,SM_DSI_LENGTH,&length))
        return SM_DSI_REFUSED;
    // Idle and clear-done are preconditions;never consume another owner's
    // completion,disable its engine or clear its error in order to start.
    if(version!=p->hardware_version || control!=p->control || !(control&1) ||
       dma!=p->dma_control || (status&(UINT32_C(0x8000000e))) ||
       (ints&UINT32_C(0x01000001)) || tpg || (dma1&2) ||
       (dma&UINT32_C(0xc0000000)))return SM_DSI_REFUSED;
    t->lease_epoch=p->lease_epoch;t->began_us=now_us;t->dma_control=dma;
    t->dma_control1=dma1;t->length=length;t->tpg=tpg;
    t->interrupt_masks=ints&UINT32_C(0xaaaaaaaa);
    t->use_lpm=p->use_lpm;t->prepared=true;return SM_DSI_WAIT;
}
static inline int sm_dsi_program(struct sm_dsi_transfer *t,const uint8_t packet[8],
                                 uint32_t broadcast_bits) {
    if(!t || !packet || !t->prepared || t->active || t->poisoned ||
       (broadcast_bits!=0 && broadcast_bits!=UINT32_C(0x80000000) &&
        broadcast_bits!=UINT32_C(0xc0000000)))return SM_DSI_REFUSED;
    uint32_t words[2]={0,0};
    for(unsigned i=0;i<8;++i)words[i/4]|=(uint32_t)packet[i]<<(8*(i%4));
    // Preserve unrelated IRQ masks;write no W1C status bits. Poll our DMA
    // completion without introducing an unowned interrupt handler.
    if(!sm_dsi_write(t,SM_DSI_INT,t->interrupt_masks&~UINT32_C(2)) ||
       !sm_dsi_write(t,SM_DSI_TPG,UINT32_C(0x30006)) ||
       !sm_dsi_write(t,SM_DSI_FIFO,words[0]) || !sm_dsi_write(t,SM_DSI_FIFO,words[1]) ||
       !sm_dsi_write(t,SM_DSI_DMA_CTRL,(t->dma_control&~UINT32_C(0xc4000000))|
                     UINT32_C(0x10000000)|broadcast_bits|(t->use_lpm?UINT32_C(0x04000000):0)) ||
       !sm_dsi_write(t,SM_DSI_DMA_CTRL1,t->dma_control1&~UINT32_C(2)) ||
       !sm_dsi_write(t,SM_DSI_LENGTH,8))return sm_dsi_uncertain(t,SM_DSI_UNCERTAIN);
    t->io.barrier(t->io.context);
    return SM_DSI_WAIT;
}
static inline int sm_dsi_trigger(struct sm_dsi_transfer *t) {
    if(!t || !t->prepared || t->poisoned || t->active)return SM_DSI_REFUSED;
    if(!sm_dsi_write(t,SM_DSI_TRIGGER,1))return sm_dsi_uncertain(t,SM_DSI_UNCERTAIN);
    t->prepared=false;t->active=true;return SM_DSI_SENT;
}
static inline int sm_dsi_begin(struct sm_dsi_transfer *t,
                               const struct sm_dsi_proof *p,
                               const uint8_t packet[8],uint64_t now_us) {
    int r=sm_dsi_preflight(t,p,packet,now_us);if(r<0)return r;
    r=sm_dsi_program(t,packet,0);return r<0?r:sm_dsi_trigger(t);
}
// Workloop/timer polling. Caller supplies the monotonic timestamp, budget and
// began_us origin; the transport never samples a clock or extends a deadline.
// Timeout/error/lease loss never becomes completion or an automatic retry.
static inline int sm_dsi_observe_budget(struct sm_dsi_transfer *t,uint64_t lease_epoch,
                              uint64_t now_us,uint64_t budget_us) {
    if(!t || t->poisoned || !t->active)return SM_DSI_REFUSED;
    if(!lease_epoch || lease_epoch!=t->lease_epoch)
        return sm_dsi_uncertain(t,SM_DSI_UNCERTAIN);
    uint32_t ints,status;
    if(!sm_dsi_read(t,SM_DSI_INT,&ints) || !sm_dsi_read(t,SM_DSI_STATUS,&status) ||
       (ints&UINT32_C(0x01000000)) || (status&UINT32_C(0x80000000)))
        return sm_dsi_uncertain(t,SM_DSI_UNCERTAIN);
    // Exact OEM IRQ and atomic polling paths complete on DMA_DONE alone.
    // STATUS is still read for errors; DMA_BUSY is a preflight condition.
    if(ints&1)return SM_DSI_COMPLETE;
    if(now_us-t->began_us>=budget_us)return sm_dsi_uncertain(t,SM_DSI_TIMEOUT);
    return SM_DSI_WAIT;
}
static inline int sm_dsi_observe(struct sm_dsi_transfer *t,uint64_t lease_epoch,
                              uint64_t now_us) {
    return sm_dsi_observe_budget(t,lease_epoch,now_us,UINT64_C(5000));
}
// Caller must have observed actual completion immediately before cleanup.
static inline int sm_dsi_cleanup(struct sm_dsi_transfer *t) {
        if(!t || !t->active || t->poisoned)return SM_DSI_REFUSED;
        // Stop only the FIFO mode we acquired;reset that command FIFO after
        // actual completion,then restore the inherited DMA configuration.
        if(!sm_dsi_write(t,SM_DSI_TPG,0) || !sm_dsi_write(t,SM_DSI_FIFO_RESET,1))
            return sm_dsi_uncertain(t,SM_DSI_UNCERTAIN);
        t->io.barrier(t->io.context);t->io.delay_one_us(t->io.context);
        if(!sm_dsi_write(t,SM_DSI_FIFO_RESET,0) ||
           !sm_dsi_write(t,SM_DSI_DMA_CTRL,t->dma_control) ||
           !sm_dsi_write(t,SM_DSI_DMA_CTRL1,t->dma_control1) ||
           !sm_dsi_write(t,SM_DSI_LENGTH,t->length) ||
           !sm_dsi_write(t,SM_DSI_TPG,t->tpg))return sm_dsi_uncertain(t,SM_DSI_UNCERTAIN);
        t->io.barrier(t->io.context);
        // Acknowledge our done bit only. Unrelated W1C status remains set.
        if(!sm_dsi_write(t,SM_DSI_INT,t->interrupt_masks|1))
            return sm_dsi_uncertain(t,SM_DSI_UNCERTAIN);
        uint32_t dma,dma1,length,tpg,ints;
        if(!sm_dsi_read(t,SM_DSI_DMA_CTRL,&dma) || dma!=t->dma_control ||
           !sm_dsi_read(t,SM_DSI_DMA_CTRL1,&dma1) || dma1!=t->dma_control1 ||
           !sm_dsi_read(t,SM_DSI_LENGTH,&length) || length!=t->length ||
           !sm_dsi_read(t,SM_DSI_TPG,&tpg) || tpg!=t->tpg ||
           !sm_dsi_read(t,SM_DSI_INT,&ints) || (ints&UINT32_C(0xaaaaaaaa))!=t->interrupt_masks ||
           (ints&UINT32_C(0x01000000)))return sm_dsi_uncertain(t,SM_DSI_UNCERTAIN);
        t->active=false;return SM_DSI_COMPLETE;
}
static inline int sm_dsi_poll_budget(struct sm_dsi_transfer *t,uint64_t lease_epoch,
                              uint64_t now_us,uint64_t budget_us) {
    int r=sm_dsi_observe_budget(t,lease_epoch,now_us,budget_us);return r==SM_DSI_COMPLETE?sm_dsi_cleanup(t):r;
}
static inline int sm_dsi_poll(struct sm_dsi_transfer *t,uint64_t lease_epoch,
                              uint64_t now_us) {
    return sm_dsi_poll_budget(t,lease_epoch,now_us,UINT64_C(5000));
}
static inline int sm_dsi_pair_fail(struct sm_dsi_transfer *master,
                                   struct sm_dsi_transfer *slave,int r) {
    sm_dsi_uncertain(master,r);sm_dsi_uncertain(slave,r);return r;
}
// SameSoC reference order: stage both,trigger slave,trigger master. A pair
// lease is global;both contexts must refer to independently mapped controllers.
static inline int sm_dsi_pair_begin(struct sm_dsi_transfer *master,
                                    const struct sm_dsi_proof *mp,
                                    struct sm_dsi_transfer *slave,
                                    const struct sm_dsi_proof *sp,
                                    const uint8_t packet[8],uint64_t now_us) {
    if(!master || !slave || master==slave || master->io.context==slave->io.context ||
       !mp || !sp || !mp->lease_epoch || mp->lease_epoch!=sp->lease_epoch ||
       mp->use_lpm!=sp->use_lpm)return SM_DSI_REFUSED;
    int r=sm_dsi_preflight(master,mp,packet,now_us);if(r<0)return r;
    r=sm_dsi_preflight(slave,sp,packet,now_us);
    if(r<0){master->prepared=false;return r;}
    r=sm_dsi_program(master,packet,UINT32_C(0xc0000000));
    if(r<0)return sm_dsi_pair_fail(master,slave,r);
    r=sm_dsi_program(slave,packet,UINT32_C(0x80000000));
    if(r<0)return sm_dsi_pair_fail(master,slave,r);
    r=sm_dsi_trigger(slave);if(r<0)return sm_dsi_pair_fail(master,slave,r);
    r=sm_dsi_trigger(master);return r<0?sm_dsi_pair_fail(master,slave,r):SM_DSI_SENT;
}
static inline int sm_dsi_pair_poll(struct sm_dsi_transfer *master,
                                   struct sm_dsi_transfer *slave,
                                   uint64_t epoch,uint64_t now_us) {
    if(!master || !slave || master==slave || master->poisoned || slave->poisoned ||
       !master->active || !slave->active)return SM_DSI_REFUSED;
    int a=sm_dsi_observe(master,epoch,now_us),b=sm_dsi_observe(slave,epoch,now_us);
    if(a<0 || b<0)return sm_dsi_pair_fail(master,slave,a<0?a:b);
    if(a!=SM_DSI_COMPLETE || b!=SM_DSI_COMPLETE)return SM_DSI_WAIT;
    a=sm_dsi_cleanup(slave);if(a<0)return sm_dsi_pair_fail(master,slave,a);
    a=sm_dsi_cleanup(master);return a<0?sm_dsi_pair_fail(master,slave,a):SM_DSI_COMPLETE;
}
