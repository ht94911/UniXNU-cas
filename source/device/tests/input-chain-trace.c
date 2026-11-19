#include <stdint.h>
#include <assert.h>
#include <stdarg.h>
static uint64_t now;
static unsigned printed;
static uint64_t mach_absolute_time(void) { return ++now; }
static int captured_printf(const char *format, ...) {
    (void)format;
    ++printed;
    return 0;
}
#define printf captured_printf
#include "../package/nezha27-input/InputChainTrace.h"
int main(void) {
    nezha_input_chain_trace(0x01000000,1,2,3,4,5);
    assert(nezha_chain_head == 0);
    nezha_chain_lock=1;
    nezha_input_chain_trace(0x2b8a0258,1,2,3,4,5);
    assert(nezha_chain_drops==1 && nezha_chain_head==0);
    nezha_chain_lock=0;
    for (unsigned i=0;i<4097;++i)
        nezha_input_chain_trace(0x2b8a0258,i,2,3,4,5);
    assert(nezha_chain_head==4096 && nezha_chain_drops==2);
    for (unsigned batch=0;batch<4;++batch) nezha_input_chain_flush();
    assert(printed==4096 && nezha_chain_tail==4096);
    for (unsigned i=0;i<10000;++i) {
        nezha_input_chain_trace(0x05230004,i,2,3,4,5);
        assert(nezha_chain_records[(nezha_chain_head-1)%4096].argument[0]==i);
        nezha_input_chain_flush();
    }
    assert(printed==14096 && nezha_chain_head==nezha_chain_tail);
}
