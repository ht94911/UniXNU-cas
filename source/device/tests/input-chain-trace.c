#include <stdint.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
static uint64_t now;
static uint64_t print_ticks;
static unsigned printed;
static uint32_t printed_codes[16384];
static uint64_t printed_tids[16384];
static uint64_t printed_arguments[16384], printed_ticks[16384];
static void (*clock_interruption)(void);
static uint64_t mach_absolute_time(void) {
    if(clock_interruption) {
        void (*interrupt)(void)=clock_interruption;
        clock_interruption=NULL;
        interrupt();
    }
    return ++now;
}
static int captured_printf(const char *format, ...) {
    (void)format;
    va_list args;
    va_start(args,format);
    assert(printed<16384);
    printed_codes[printed]=va_arg(args,unsigned);
    printed_tids[printed]=va_arg(args,unsigned long long);
    printed_ticks[printed]=va_arg(args,unsigned long long);
    printed_arguments[printed]=va_arg(args,unsigned long long);
    va_end(args);
    ++printed;
    now+=print_ticks;
    return 0;
}
#define printf captured_printf
#include "../package/nezha27-input/InputChainTrace.h"

static void expect_ignored_end(uint32_t code, uintptr_t tid) {
    const unsigned head=nezha_chain_head, drops=nezha_chain_drops;
    nezha_input_chain_trace(code,0,0,0,0,tid);
    assert(nezha_chain_head==head && nezha_chain_drops==drops);
}

static void fill_ring(void) {
    while (nezha_chain_head-nezha_chain_tail<4096)
        nezha_input_chain_trace(0x05230004,1,2,3,4,5);
}

static void drain_ring(void) {
    while (nezha_input_chain_flush(UINT64_MAX)) {}
    assert(nezha_chain_head==nezha_chain_tail);
    printed=0;
}

static void lose_record_during_admission(void) {
    nezha_input_chain_trace(0x2b8a0258,1,2,3,4,599);
}

static void render_message_contract(void) {
    const uint32_t messages[]={0x0120000c,0x01200010,0x01200014};
    const uint32_t ends[]={0x010c007e,0x010c0082,0x010c00be};
    const uint32_t render_sites[]={0x31ca000d,0x31ca000e,0x31ca023a};
    for(unsigned detail=0;detail<2;++detail) {
        nezha_chain_ca_details=detail;
        for(unsigned i=0;i<3;++i) {
            const unsigned head=nezha_chain_head;
            nezha_input_chain_trace(render_sites[i],1,2,3,4,5);
            assert(nezha_chain_head==head+1);
            assert(nezha_chain_records[head%4096].code==render_sites[i]);
            assert(nezha_input_chain_flush(UINT64_MAX)==1);
        }
    }
    const uint32_t unrelated[]={0x31ca000c,0x31ca000f,0x31ca0238,
        0x31ca0239,0x31ca023b,0x0120002a};
    for(unsigned i=0;i<sizeof(unrelated)/sizeof(unrelated[0]);++i) {
        const unsigned head=nezha_chain_head, drops=nezha_chain_drops;
        nezha_input_chain_trace(unrelated[i],1,2,40002,4,5);
        assert(nezha_chain_head==head && nezha_chain_drops==drops);
    }
    for(unsigned i=0;i<3;++i) {
        const uintptr_t rejected[]={0,40001,40006,UINTPTR_MAX};
        for(unsigned j=0;j<4;++j) {
            const unsigned head=nezha_chain_head, drops=nezha_chain_drops;
            nezha_input_chain_trace(messages[i],1,2,rejected[j],4,51);
            assert(nezha_chain_head==head && nezha_chain_drops==drops);
            expect_ignored_end(ends[i],51);
        }
        for(uintptr_t id=40002;id<=40005;++id)
            for(unsigned j=0;j<3;++j) {
                const uintptr_t tid=60+i*16+(id-40002)*3+j;
                const unsigned head=nezha_chain_head;
                nezha_input_chain_trace(ends[j]-1,1,2,3,4,tid);
                assert(nezha_chain_head==head);
                nezha_input_chain_trace(messages[i],0xabcdef,0x80000013,id,0x55,tid);
                const struct nezha_chain_record *r=&nezha_chain_records[head%4096];
                assert(nezha_chain_head==head+1 && r->code==messages[i] && r->tid==tid);
                assert(r->argument[0]==0xabcdef && r->argument[1]==0x80000013 &&
                    r->argument[2]==id && r->argument[3]==0x55);
                expect_ignored_end(ends[j],tid+1000);
                nezha_input_chain_trace(ends[j],j?0:0x10000003,0,0,0,tid);
                r=&nezha_chain_records[(head+1)%4096];
                assert(nezha_chain_head==head+2 && r->code==ends[j] && r->tid==tid);
                assert(r->argument[0]==(j?0:0x10000003));
                expect_ignored_end(ends[j],tid);
                const unsigned before=printed;
                assert(nezha_input_chain_flush(UINT64_MAX)==2);
                assert(printed_codes[before]==messages[i] && printed_tids[before]==tid);
                assert(printed_codes[before+1]==ends[j] && printed_tids[before+1]==tid);
            }
    }
    // A combined Mach call has one end even with both send and receive.
    for(unsigned variant=0;variant<3;++variant) {
        const uintptr_t tid=200+variant;
        const unsigned head=nezha_chain_head;
        nezha_input_chain_trace(0x010c00bd,1,2,3,4,tid);
        nezha_input_chain_trace(messages[0],1,2,variant==1?40102:40002,4,tid);
        nezha_input_chain_trace(messages[2],1,2,variant==2?40102:40005,4,tid);
        nezha_input_chain_trace(ends[2],0,0,0,0,tid);
        assert(nezha_chain_head==head+(variant?2:3));
        expect_ignored_end(ends[2],tid);
        drain_ring();
    }
    // A missing end cannot survive the next call's start or unrelated end.
    for(unsigned kind=0;kind<4;++kind) {
        const uintptr_t tid=300+kind;
        const unsigned head=nezha_chain_head;
        nezha_input_chain_trace(messages[0],1,2,40002,4,tid);
        nezha_input_chain_trace(kind<3?ends[kind]-1:0x010c00a1,1,2,3,4,tid);
        assert(nezha_chain_head==head+1);
        expect_ignored_end(ends[kind%3],tid);
        drain_ring();
    }
    nezha_input_chain_trace(messages[1],1,2,40003,4,350);
    expect_ignored_end(0x010c00a2,350);
    expect_ignored_end(ends[1],350);
    drain_ring();
    // Ring overflow invalidates every old marker, and a rejected message
    // never creates one. The lost end must also consume its marker.
    for(unsigned lost_end=0;lost_end<2;++lost_end) {
        const unsigned drops=nezha_chain_drops, head=nezha_chain_head;
        nezha_input_chain_trace(messages[0],1,2,40002,4,400);
        fill_ring();
        nezha_input_chain_trace(lost_end?ends[0]:messages[1],1,2,40002,4,
            lost_end?400:401);
        assert(nezha_chain_head==head+4096 && nezha_chain_drops==drops+1);
        drain_ring();
        expect_ignored_end(ends[0],400);
        expect_ignored_end(ends[1],401);
    }
    // A busy writer losing a message, an end, a new start, or another
    // admitted event cannot leave a marker that qualifies a later return.
    const uint32_t contended[]={0x0120000c,0x010c00be,0x010c00bd,0x2b8a0258};
    for(unsigned kind=0;kind<4;++kind) {
        const uintptr_t tid=500+kind;
        const unsigned drops=nezha_chain_drops, head=nezha_chain_head;
        if(kind)nezha_input_chain_trace(messages[0],1,2,40002,4,tid);
        nezha_chain_lock=1;
        nezha_input_chain_trace(contended[kind],1,2,40002,4,tid);
        assert(nezha_chain_drops==drops+1 && nezha_chain_head==head+(kind!=0));
        nezha_chain_lock=0;
        expect_ignored_end(ends[2],tid);
        drain_ring();
    }
    // A competing loss between the generation snapshot and message admission
    // must not let the admitted message acquire the new loss generation.
    const unsigned interrupted_head=nezha_chain_head;
    const unsigned interrupted_drops=nezha_chain_drops;
    clock_interruption=lose_record_during_admission;
    nezha_input_chain_trace(messages[0],1,2,40002,4,590);
    assert(!clock_interruption && nezha_chain_head==interrupted_head+1);
    assert(nezha_chain_drops==interrupted_drops+1);
    expect_ignored_end(ends[2],590);
    drain_ring();
    // An unrelated return/message does not acquire the writer lock or count
    // a drop, even while a valid render call is pending on another thread.
    nezha_input_chain_trace(messages[0],1,2,40002,4,600);
    const unsigned drops=nezha_chain_drops, head=nezha_chain_head;
    nezha_chain_lock=1;
    expect_ignored_end(ends[0],601);
    nezha_input_chain_trace(messages[0],1,2,40001,4,600);
    assert(nezha_chain_drops==drops && nezha_chain_head==head);
    nezha_chain_lock=0;
    nezha_input_chain_trace(ends[0],0,0,0,0,600);
    assert(nezha_chain_head==head+1);
    drain_ring();
    // Exhausting the bounded association table rejects a message and reports
    // a loss; it cannot evict an old marker and mislabel a future return.
    for(unsigned i=0;i<NEZHA_CHAIN_PENDING_MAX;++i)
        nezha_input_chain_trace(messages[0],1,2,40002,4,700+i);
    const unsigned full_head=nezha_chain_head, full_drops=nezha_chain_drops;
    nezha_input_chain_trace(messages[0],1,2,40002,4,800);
    assert(nezha_chain_head==full_head && nezha_chain_drops==full_drops+1);
    for(unsigned i=0;i<NEZHA_CHAIN_PENDING_MAX;++i)expect_ignored_end(ends[0],700+i);
    expect_ignored_end(ends[0],800);
    nezha_input_chain_trace(messages[0],1,2,40002,4,800);
    nezha_input_chain_trace(ends[0],0,0,0,0,800);
    assert(nezha_chain_head==full_head+2);
    drain_ring();
    puts("render message ids, Mach returns, combined calls and loss invalidation passed");
}

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
        assert(nezha_chain_head==prior && nezha_chain_drops==drops);
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
    drain_ring();
    render_message_contract();
}
