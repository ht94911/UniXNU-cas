/* SPDX-License-Identifier: MIT */
#include "NezhaCameraAk7316.h"
/* Reuse the exercised CCI queue/IRQ/FIFO model for the original ordered
 * actuator messages. This tests the actual CCI implementation too. */
#define main camera_cci_baseline_main
#include "camera-cci.c"
#undef main

struct Transfer { bool read; uint32_t message, read_command, fifo; };
struct Actuator {
    struct Model cci;
    struct Transfer transfer[8];
    unsigned count, next, fail_at;
    enum NezhaCameraCciStatus fault;
};

static uint32_t actuator_read(void *context, uint32_t offset)
{
    return read32(&((struct Actuator *)context)->cci, offset);
}

static void actuator_write(void *context, uint32_t offset, uint32_t value)
{
    struct Actuator *a=context;
    if (offset==0x008) {
        assert(a->next<a->count);
        const struct Transfer *t=&a->transfer[a->next];
        assert(value==(t->read ? 8u : 4u));
        a->cci.expected_slave=0xc1;
        a->cci.expected_write=t->read ? t->message & ~2u : t->message;
        a->cci.expected_read=t->read_command;
        a->cci.fifo_word=t->fifo;
        a->cci.data_words=1;
        a->cci.address_result=M1AddressReport;
        a->cci.data_result=M1DataReport|M1ReadDone;
        if (a->next==a->fail_at) {
            if (t->read) a->cci.data_result=a->fault==NezhaCciNack ? M1DataNack : 0;
            else a->cci.address_result=a->fault==NezhaCciNack ? M1AddressNack : 0;
        }
        ++a->next;
    }
    write32(&a->cci,offset,value);
}

static void actuator_wait(void *context,unsigned ms)
{
    assert(ms==1 || ms==2);
    ((struct Actuator *)context)->cci.waits+=ms;
}

static struct Actuator init_script(void)
{
    return (struct Actuator){.cci=initial(),.count=4,.fail_at=99,
        .transfer={{false,0x00000229,0,0}, {true,0x00000219,0x1a,0x21},
                   {false,0x60540039,0,0}, {true,0x00000019,0x2a,0x00605421}}};
}

int main(void)
{
    struct Actuator a=init_script();
    const struct NezhaCameraRegisterIo io={&a,actuator_read,actuator_write,actuator_wait};
    struct NezhaCameraAk7316Result result;
    assert(nezha_camera_ak7316_initialize(&io,&result)==NezhaCciOk);
    assert(a.next==4 && result.completed_writes==2 && result.completed_reads==2);
    assert(result.position_read && result.register_value==0x5460 && result.position_dac==1350);
    assert(a.cci.waits==2 && !result.transfer.needs_recovery);
    for (unsigned kind=0;kind<2;++kind) for (unsigned stop=0;stop<4;++stop) {
        a=init_script();a.fail_at=stop;a.fault=kind ? NezhaCciTimeout : NezhaCciNack;
        assert(nezha_camera_ak7316_initialize(&io,&result)==a.fault);
        assert(a.next==stop+1 && a.cci.submitted==stop+1);
        assert(result.transfer.needs_recovery && !result.position_read);
    }
    a=init_script();
    a.count=6;
    for (unsigned i=1;i<6;++i) a.transfer[i]=(struct Transfer){true,0x00000219,0x1a,0x121};
    assert(nezha_camera_ak7316_initialize(&io,&result)==NezhaCciTimeout);
    assert(a.next==6 && result.completed_writes==1 && result.completed_reads==5);
    assert(!result.position_read && result.control==1 && a.cci.waits==7);
    const unsigned dac[]={0,100,540,1023,3965,4095};
    for (unsigned i=0;i<sizeof(dac)/sizeof(*dac);++i) {
        const uint16_t value=(uint16_t)(dac[i]<<4);
        a=(struct Actuator){.cci=initial(),.count=2,.fail_at=99};
        a.transfer[0]=(struct Transfer){false,0x39u | (uint32_t)(value>>8)<<16 | (uint32_t)(value&255)<<24,0,0};
        a.transfer[1]=(struct Transfer){true,0x19,0x2a,0x21u | (uint32_t)(value>>8)<<8 | (uint32_t)(value&255)<<16};
        assert(nezha_camera_ak7316_set_position(&io,(uint16_t)dac[i],&result)==NezhaCciOk);
        assert(result.position_read && result.position_dac==dac[i] && a.next==2);
    }
    a=(struct Actuator){.cci=initial(),.count=2,.fail_at=99,
        .transfer={{false,0x40210039,0,0},{true,0x19,0x2a,0x00605421}}};
    assert(nezha_camera_ak7316_set_position(&io,532,&result)==NezhaCciFifo);
    assert(result.position_read && result.position_dac==1350 && a.next==2);
    a=(struct Actuator){.cci=initial(),.count=1,.fail_at=99,
        .transfer={{true,0x19,0x2a,0x00615421}}};
    assert(nezha_camera_ak7316_read_position(&io,&result)==NezhaCciFifo);
    assert(!result.position_read && result.completed_reads==1);
    a=(struct Actuator){.cci=initial(),.count=1,.fail_at=99,
        .transfer={{false,0x00000229,0,0}}};
    assert(nezha_camera_ak7316_deinitialize(&io,&result)==NezhaCciOk);
    assert(result.completed_writes==1 && a.next==1 && a.cci.waits==2);
    a=(struct Actuator){.cci=initial()};
    assert(nezha_camera_ak7316_set_position(&io,4096,&result)==NezhaCciInvalid);
    assert(!a.cci.writes && !a.cci.submitted);
    assert(nezha_camera_ak7316_initialize(NULL,&result)==NezhaCciInvalid);
    assert(nezha_camera_ak7316_initialize(&io,NULL)==NezhaCciInvalid);
    puts("AK7316 original init/poll/position/deinit, six DAC readbacks and first-error stop passed");
    return 0;
}
