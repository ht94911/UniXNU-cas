/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Source-only tests. Synthetic bytes, no proprietary tuning data or hardware. */
#include "../platform/Tas25xxProgram.h"
#include <assert.h>
#include <string.h>
typedef struct {unsigned writes,reads;uint8_t address,last;int fail;} FakeIo;
static int read_byte(void *ctx,uint64_t lease,uint8_t addr,uint32_t reg,uint8_t *out) {
    FakeIo *f=ctx;assert(lease==42 && reg==3);++f->reads;f->address=addr;*out=0xa0;return 0;
}
static int write_byte(void *ctx,uint64_t lease,uint8_t addr,uint32_t reg,const uint8_t *p,uint32_t n) {
    FakeIo *f=ctx;assert(lease==42 && (reg==2 || reg==3) && n==1);++f->writes;
    f->address=addr;f->last=*p;return f->fail ? NA_IO:0;
}
int main(void) {
    const uint8_t bytes[]={18,0,0,0, 0,2,0,0,0,1, 3,20,0,0,0, 2,3,0,0,0,3,2};
    NaTasBytes p={bytes,sizeof bytes};NaTasProgramInfo info;
    assert(!na_tas_validate(p,&info) && info.commands==3 && info.delay_ms==20);
    FakeIo f={0};NaTasIo io={&f,read_byte,write_byte};NaTasRun run={0};
    assert(!na_tas_begin(&run,p,&io,42,0));
    assert(na_tas_poll(&run,100)==NA_PENDING && f.writes==1 && f.address==0x4a);
    assert(na_tas_poll(&run,100)==NA_PENDING && f.writes==1);
    assert(na_tas_begin(&run,p,&io,42,1)==NA_BUSY);
    assert(na_tas_poll(&run,119)==NA_PENDING && !f.reads);
    assert(!na_tas_poll(&run,120) && f.reads==1 && f.last==0xa2 && run.finished);
    assert(!na_tas_poll(&run,200) && f.writes==2); /* No command replay. */
    assert(!na_tas_begin(&run,p,&io,42,1));f.fail=1;
    assert(na_tas_poll(&run,200)==NA_IO && f.address==0x48);
    unsigned count=f.writes;assert(na_tas_poll(&run,300)==NA_IO && f.writes==count);
    for (unsigned n=0;n<sizeof bytes;++n) {
        NaTasBytes short_input={bytes,n};assert(na_tas_validate(short_input,&info));
    }
    uint8_t bad[sizeof bytes];memcpy(bad,bytes,sizeof bad);bad[10]=0xff;
    NaTasBytes wrong={bad,sizeof bad};NaTasRun fresh={0};
    assert(na_tas_begin(&fresh,wrong,&io,42,0)==NA_WIRE && f.writes==count);
    const uint8_t overflow[]={9,0,0,0,1,127,0,0,0,0xff,0xff,0xff,0xff};
    assert(na_tas_validate((NaTasBytes){overflow,sizeof overflow},&info)==NA_WIRE);
    return 0;
}
