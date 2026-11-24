#include <stdint.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
static uint64_t now;
static uint64_t print_ticks;
static unsigned printed;
static uint64_t printed_arguments[16384], printed_ticks[16384];
static uint64_t mach_absolute_time(void) { return ++now; }
static int captured_printf(const char *format, ...) {
    (void)format;
    va_list args;
    va_start(args,format);
    (void)va_arg(args,unsigned);
    (void)va_arg(args,unsigned long long);
    assert(printed<16384);
    printed_ticks[printed]=va_arg(args,unsigned long long);
    printed_arguments[printed]=va_arg(args,unsigned long long);
    va_end(args);
    ++printed;
    now+=print_ticks;
    return 0;
}
#define printf captured_printf
#include "../package/nezha27-input/InputChainTrace.h"
int main(void) {
    assert(sizeof(struct nezha_chain_record)==56);
    nezha_input_chain_trace(0x01000000,1,2,3,4,5);
    assert(nezha_chain_head == 0);
    const uint32_t ui_sites[]={0x2b8701e4,0x2b8701e8,0x2b870210};
    for(unsigned detail=0;detail<2;++detail) {
        nezha_chain_ca_details=detail;
        for(unsigned flag=0;flag<4;++flag) {
            for(unsigned i=0;i<sizeof(ui_sites)/sizeof(ui_sites[0]);++i) {
                const unsigned prior=nezha_chain_head;
                nezha_input_chain_trace(ui_sites[i]|flag,1,2,3,4,5);
                assert(nezha_chain_head==prior+1);
                assert(nezha_chain_records[prior%4096].code==(ui_sites[i]|flag));
                assert(nezha_input_chain_flush(UINT64_MAX)==1);
            }
        }
        const unsigned prior=nezha_chain_head;
        nezha_input_chain_trace(0x2b870214,1,2,3,4,5);
        assert(nezha_chain_head==prior);
    }
    nezha_chain_ca_details=0;
    const uint32_t critical[]={0x31ca0028,0x31ca0004,0x31ca0210,
        0x31ca0214,0x31ca0218,0x31ca0260,0x31ca0264,
        0x31ca0270,0x31ca0224,
        0x2b8a0258,0x2b280000,0x05230004};
    for(unsigned flag=0;flag<4;++flag) {
        for(unsigned i=0;i<sizeof(critical)/sizeof(critical[0]);++i) {
            const unsigned prior=nezha_chain_head;
            nezha_input_chain_trace(critical[i]|flag,1,2,3,4,5);
            assert(nezha_chain_head==prior+1);
            assert(nezha_input_chain_flush(UINT64_MAX)==1);
        }
        const unsigned prior=nezha_chain_head, drops=nezha_chain_drops;
        nezha_input_chain_trace(0x31ca01dc|flag,1,2,3,4,5);
        nezha_input_chain_trace(0x14ca001c|flag,1,2,3,4,5);
        const unsigned expected=flag==1 || flag==2;
        assert(nezha_chain_head==prior+expected && nezha_chain_drops==drops);
        if(expected)assert(nezha_input_chain_flush(UINT64_MAX)==1);
    }
    // Keep only the two original send boundaries in either profile. No
    // event is generated here; the original sender's debug guard is unchanged.
    for(unsigned detail=0;detail<2;++detail) {
        nezha_chain_ca_details=detail;
        for(unsigned flag=0;flag<4;++flag) {
            const unsigned prior=nezha_chain_head, drops=nezha_chain_drops;
            nezha_input_chain_trace(0x31ca00bc|flag,0x1234567812345678ULL,2,3,4,5);
            const unsigned expected=flag==1 || flag==2;
            assert(nezha_chain_head==prior+expected && nezha_chain_drops==drops);
            if(expected) {
                const struct nezha_chain_record *r=&nezha_chain_records[prior%4096];
                assert(r->code==(0x31ca00bc|flag) && r->tid==5);
                assert(r->argument[0]==0x1234567812345678ULL && r->argument[1]==2 && r->argument[2]==3 && r->argument[3]==4);
                assert(nezha_input_chain_flush(UINT64_MAX)==1);
            }
        }
    }
    nezha_chain_ca_details=1;
    for(unsigned code=0;code<4;++code) {
        const uint32_t wanted[]={0x31ca0028,0x31ca0004,0x31ca0210,0x14ca001c,
            0x31ca01dc,0x31ca0214,0x31ca0218,0x31ca0260,0x31ca0264,
            0x31ca0270,0x31ca0224};
        for(unsigned i=0;i<sizeof(wanted)/sizeof(wanted[0]);++i) {
            const unsigned prior=nezha_chain_head;
            nezha_input_chain_trace(wanted[i]|code,1,2,3,4,5);
            assert(nezha_chain_head==prior+1 && nezha_chain_records[prior%4096].code==(wanted[i]|code));
            assert(nezha_input_chain_flush(UINT64_MAX)==1);
        }
    }
    unsigned prior=nezha_chain_head;
    nezha_input_chain_trace(0x31ca0268,1,2,3,4,5);
    assert(nezha_chain_head==prior);
    nezha_chain_head=nezha_chain_tail=printed=0;
    nezha_chain_lock=1;
    nezha_input_chain_trace(0x2b8a0258,1,2,3,4,5);
    assert(nezha_chain_drops==1 && nezha_chain_head==0);
    nezha_chain_lock=0;
    for (unsigned i=0;i<4097;++i)
        nezha_input_chain_trace(0x2b8a0258,i,2,3,4,5);
    assert(nezha_chain_head==4096 && nezha_chain_drops==2);
    for (unsigned batch=0;batch<16;++batch)
        assert(nezha_input_chain_flush(UINT64_MAX)==NEZHA_CHAIN_FLUSH_MAX);
    assert(printed==4096 && nezha_chain_tail==4096);
    for(unsigned i=0;i<4096;++i) {
        assert(printed_arguments[i]==i);
        if(i)assert(printed_ticks[i]>printed_ticks[i-1]);
    }
    for (unsigned i=0;i<10000;++i) {
        nezha_input_chain_trace(0x05230004,i,2,3,4,5);
        assert(nezha_chain_records[(nezha_chain_head-1)%4096].argument[0]==i);
        assert(nezha_input_chain_flush(UINT64_MAX)==1);
    }
    assert(printed==14096 && nezha_chain_head==nezha_chain_tail);
    // An expired budget and a busy consumer leave the queued record intact.
    nezha_input_chain_trace(0x05230004,123,2,3,4,5);
    unsigned tail=nezha_chain_tail, drops=nezha_chain_drops;
    assert(nezha_input_chain_flush(now)==0 && nezha_chain_tail==tail);
    nezha_chain_lock=1;
    assert(nezha_input_chain_flush(UINT64_MAX)==0 && nezha_chain_tail==tail);
    assert(nezha_chain_drops==drops);
    nezha_chain_lock=0;
    assert(nezha_input_chain_flush(UINT64_MAX)==1);
    // Stop between printf calls; the next batch resumes the same FIFO.
    for(unsigned i=0;i<32;++i)nezha_input_chain_trace(0x2b8a0258,i,2,3,4,5);
    const unsigned before=printed;
    print_ticks=5;
    assert(nezha_input_chain_flush(now+14)==3);
    assert(nezha_chain_head-nezha_chain_tail==29);
    print_ticks=0;
    assert(nezha_input_chain_flush(UINT64_MAX)==29);
    for(unsigned i=0;i<32;++i)assert(printed_arguments[before+i]==i);
    // One slow print may exceed the soft budget, but no second one starts.
    nezha_input_chain_trace(0x2b8a0258,100,2,3,4,5);
    nezha_input_chain_trace(0x2b8a0258,101,2,3,4,5);
    print_ticks=1000;
    assert(nezha_input_chain_flush(now+100)==1);
    assert(nezha_chain_head-nezha_chain_tail==1);
    print_ticks=0;
    assert(nezha_input_chain_flush(UINT64_MAX)==1);
    assert(nezha_chain_head==nezha_chain_tail && nezha_chain_drops==drops);
    puts("input-chain profiles, FIFO, overflow and timed flush passed");
}
