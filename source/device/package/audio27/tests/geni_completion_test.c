/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The production GENI/TAS path against a register oracle. The delayed case
 * reproduces035: TX watermark first, then CMD_DONE at the timeout read.
 * The byte returned here is synthetic and is not a physical TAS revision. */
#include "../platform/GeniTasBus.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {Works, BusError, EmptyRx, Stuck, LateDone, NoAck};
typedef struct {
    uint32_t regs[0x4000/4],status,operation,words,expected;
    unsigned behavior,commands,cancels,aborts,reads,writes,active,loaded,frozen;
    uint64_t now,io_us,complete_at;
} Fake;

static void advance(Fake *f) {
    if (!f->active) return;
    if (f->operation==GENI_OP_WRITE && !f->loaded) {
        f->status|=GENI_M_TX_WATERMARK;
    } else if (f->loaded && f->now>=f->complete_at && f->behavior!=Stuck &&
               f->behavior!=LateDone && f->behavior!=NoAck) {
        f->status|=GENI_M_CMD_DONE|GENI_M_TX_WATERMARK;
        if (f->behavior==BusError) f->status|=1u<<10;
        f->active=0;
    }
}
static void elapsed(Fake *f,uint64_t usec) {if (!f->frozen) f->now+=usec;}
static uint32_t read32(void *v,uint32_t reg) {
    Fake *f=v;assert(reg<0x4000 && !(reg&3));++f->reads;advance(f);
    uint32_t value=f->regs[reg/4];
    if (reg==GENI_M_IRQ_STATUS) value=f->status;
    if (reg==GENI_RX_FIFO_STATUS) value=f->behavior==EmptyRx ? 0:1;
    if (reg==GENI_RX_FIFO) value=0xa5;
    elapsed(f,f->io_us);return value;
}
static void write32(void *v,uint32_t reg,uint32_t value) {
    Fake *f=v;assert(reg<0x4000 && !(reg&3));++f->writes;advance(f);
    f->regs[reg/4]=value;
    if (reg==GENI_M_IRQ_CLEAR) f->status&=~value;
    if (reg==GENI_M_CMD0) {
        ++f->commands;f->operation=value>>27;f->active=1;f->words=0;
        assert(((value>>9)&127)==0x4a);
        f->expected=(f->regs[GENI_TX_LEN/4]+3)/4;
        f->loaded=f->operation==GENI_OP_READ;
        f->complete_at=f->now+250;
        f->status=f->loaded ? 0:GENI_M_TX_WATERMARK;
    }
    if (reg==GENI_TX_FIFO) {
        assert(f->active && f->operation==GENI_OP_WRITE);
        if (++f->words==f->expected) {f->loaded=1;f->complete_at=f->now+250;}
    }
    if (reg==GENI_M_CMD_CTRL) {
        f->active=0;
        if (value==GENI_CMD_CTRL_CANCEL) {
            ++f->cancels;
            f->status=f->behavior==NoAck ? 0:f->behavior==LateDone ?
                GENI_M_CMD_DONE:GENI_M_CMD_CANCEL|GENI_M_CMD_DONE;
        } else {
            assert(value==GENI_CMD_CTRL_ABORT);++f->aborts;
            f->status=f->behavior==NoAck ? 0:GENI_M_CMD_ABORT;
        }
    }
    elapsed(f,f->io_us);
}
static uint64_t now_us(void *v) {return ((Fake *)v)->now;}
static void pause_us(void *v,uint32_t usec) {elapsed(v,usec);}
static void init(Fake *f,NaGeniTasBus *bus,uint64_t cost,unsigned behavior) {
    memset(f,0,sizeof *f);f->io_us=cost;f->behavior=behavior;
    f->regs[GENI_FW_REVISION/4]=GENI_PROTO_I2C<<8;
    f->regs[GENI_CLK_CFG/4]=0x71;f->regs[GENI_SCL_COUNTERS/4]=0xa0301a;
    GeniIo io={read32,write32,f};
    assert(!na_tas_bus_prepare(bus,&io,f,now_us,pause_us,42));
}
static void read_case(uint64_t cost) {
    Fake f;NaGeniTasBus bus;init(&f,&bus,cost,Works);
    uint8_t byte=0;
    int result=na_tas_bus_read(&bus,42,0x4a,0x78,&byte);
    if (result) fprintf(stderr,"cost=%llu result=%d phase=%u error=%u status=%x commands=%u cancel=%u abort=%u\n",
        (unsigned long long)cost,result,bus.engine.phase,bus.engine.error,
        bus.engine.error_status,f.commands,f.cancels,f.aborts);
    assert(!result && byte==0xa5);
    assert(f.commands==5 && !f.cancels && !f.aborts && !bus.failed);
    assert(bus.engine.phase==GeniIdle && bus.engine.done==4 && !bus.engine.timeouts);
}
static void write_case(uint64_t cost) {
    Fake f;NaGeniTasBus bus;init(&f,&bus,cost,Works);
    const uint8_t bytes[8]={1,2,3,4,5,6,7,8};
    assert(!na_tas_bus_write(&bus,42,0x4a,0x20,bytes,sizeof bytes));
    assert(f.commands==5 && !f.cancels && !f.aborts && !bus.failed);
    assert(bus.engine.done==5 && !bus.engine.timeouts);
}
static void failure_case(unsigned behavior,int expected,unsigned frozen) {
    Fake f;NaGeniTasBus bus;init(&f,&bus,11000,behavior);f.frozen=frozen;
    uint8_t byte=0x33;
    assert(na_tas_bus_read(&bus,42,0x4a,0x78,&byte)==expected && byte==0x33);
    assert(bus.failed && f.reads+f.writes<5000);
    unsigned operations=f.reads+f.writes;
    assert(na_tas_bus_read(&bus,42,0x4a,0x78,&byte)==NA_OWNERSHIP);
    assert(f.reads+f.writes==operations); /* A failed transaction is never replayed. */
    if (expected==NA_TIMEOUT) assert(f.cancels==1);
    else assert(!f.cancels && !f.aborts);
    if (behavior==LateDone || behavior==NoAck) assert(f.aborts==1);
}
static void ownership_case(void) {
    Fake f;NaGeniTasBus bus;init(&f,&bus,0,Works);uint8_t byte=0;
    unsigned operations=f.reads+f.writes;
    assert(na_tas_bus_read(&bus,43,0x4a,0x78,&byte)==NA_OWNERSHIP);
    assert(na_tas_bus_read(&bus,42,0x49,0x78,&byte)==NA_OWNERSHIP);
    assert(f.reads+f.writes==operations);
}
int main(void) {
    read_case(0);read_case(11000);write_case(0);write_case(11000);
    failure_case(BusError,NA_IO,0);failure_case(EmptyRx,NA_IO,0);
    failure_case(Stuck,NA_TIMEOUT,0);failure_case(LateDone,NA_TIMEOUT,0);
    failure_case(NoAck,NA_TIMEOUT,0);failure_case(NoAck,NA_TIMEOUT,1);
    ownership_case();
    puts("PASS:11 production GENI/TAS completion, phase-progress, bus-error, cancel/abort, frozen-clock and ownership cases; no hardware identity claim");
}
