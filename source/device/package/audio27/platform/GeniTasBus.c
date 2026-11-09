/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "GeniTasBus.h"
#include <limits.h>
#include <string.h>
static int finish_transfer(NaGeniTasBus *b) {
    uint64_t now=b->now_us(b->clock_context);
    if (now>UINT64_MAX-20000) {b->failed=1;return NA_OVERFLOW;}
    uint64_t deadline=now+20000;unsigned cancels=0;
    /* Both monotonic deadline and iteration bound: a stopped time source must
     * not strand this worker. Cancel/abort use the existing GENI core. */
    for (unsigned i=0;i<1000;++i) {
        int result=geni_i2c_poll(&b->engine);
        if (result==GeniDone) return b->failed ? NA_TIMEOUT : NA_OK;
        if (result==GeniFailed) {b->failed=1;return b->engine.error==GeniErrTimeout ? NA_TIMEOUT:NA_IO;}
        now=b->now_us(b->clock_context);
        if (now>=deadline || i>=400+100*cancels) {
            result=geni_i2c_timeout(&b->engine);
            if (result==GeniDone) return b->failed ? NA_TIMEOUT:NA_OK;
            if (result==GeniFailed) {b->failed=1;return b->engine.error==GeniErrTimeout ? NA_TIMEOUT:NA_IO;}
            const int recovering=b->engine.phase==GeniCancelling || b->engine.phase==GeniAborting;
            if (recovering) {
                b->failed=1; /* Only an issued cancel/abort poisons the transaction; never replay it. */
                if (++cancels>=3) return NA_TIMEOUT;
            }
            /* A completion may advance the write/read phase instead of
             * issuing cancellation. Keep its ordinary20ms bound; recovery
             * retains5ms and the existing total iteration bound. */
            now=b->now_us(b->clock_context);
            uint32_t wait=recovering ? 5000:20000;
            if (now>UINT64_MAX-wait) {b->failed=1;return NA_OVERFLOW;}
            deadline=now+wait;
        }
        b->pause_us(b->clock_context,50);
    }
    b->failed=1;return NA_TIMEOUT;
}
static int byte_write(NaGeniTasBus *b,uint8_t reg,uint8_t value) {
    if (geni_i2c_start_write(&b->engine,reg,value)) {b->failed=1;return NA_IO;}
    return finish_transfer(b);
}
static int bank(NaGeniTasBus *b,uint64_t lease,uint8_t address,uint32_t reg) {
    if (!b || !b->ready || b->failed || b->lease!=lease || !lease || reg>=0x800000 ||
        (address!=na_nezha_facts.amp_address[0] && address!=na_nezha_facts.amp_address[1])) return NA_OWNERSHIP;
    if (geni_i2c_busy(&b->engine)) return NA_BUSY;
    b->engine.address=address;
    /* No cached page/book assumption survives reset or a raw tuning command.
     * Book select is defined only on page0, then page select precedes data. */
    int r=byte_write(b,0,0);
    if (!r) r=byte_write(b,127,(uint8_t)(reg>>15));
    if (!r) r=byte_write(b,0,(uint8_t)(reg>>7));
    return r;
}
int na_tas_bus_prepare(NaGeniTasBus *b,const GeniIo *io,void *context,
                        uint64_t (*now)(void *),void (*pause)(void *,uint32_t),uint64_t lease) {
    if (!b || !io || !io->read || !io->write || !now || !pause || !lease) return NA_ARGUMENT;
    memset(b,0,sizeof *b);b->lease=lease;b->clock_context=context;b->now_us=now;b->pause_us=pause;
    uint32_t config;
    if (geni_i2c_prepare(&b->engine,io,(uint8_t)na_nezha_facts.amp_address[0],&config)) return NA_IO;
    b->ready=1;return NA_OK;
}
int na_tas_bus_read(void *context,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *value) {
    NaGeniTasBus *b=context;
    if (!value) return NA_ARGUMENT;
    int r=bank(b,lease,address,reg);if (r) return r;
    if (geni_i2c_start_read(&b->engine,(uint8_t)(reg&127))) {b->failed=1;return NA_IO;}
    r=finish_transfer(b);if (!r) *value=b->engine.data;return r;
}
int na_tas_bus_write(void *context,uint64_t lease,uint8_t address,uint32_t reg,const uint8_t *data,uint32_t bytes) {
    NaGeniTasBus *b=context;
    if (!data || !bytes || bytes>128-(reg&127)) return NA_ARGUMENT;
    int r=bank(b,lease,address,reg);if (r) return r;
    for (uint32_t offset=0;offset<bytes;) {
        uint8_t tx[8];uint32_t n=bytes-offset;if (n>7) n=7;
        tx[0]=(uint8_t)((reg&127)+offset);memcpy(tx+1,data+offset,n);
        if (geni_i2c_start_write_bytes(&b->engine,tx,n+1)) {b->failed=1;return NA_IO;}
        r=finish_transfer(b);if (r) return r;offset+=n;
    }
    return NA_OK;
}
