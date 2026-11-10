/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Board sequencing from stock nezha DTS and sheng's TX SoundWire lifecycle.
 * GPIO/slew/VA physical windows are SM8850; do not copy SM8550 addresses. */
#include "NezhaAudioBoard.h"
#include <string.h>
#include <limits.h>
#define RESET (NA_TLMM_GPIO_BASE+161*4096)
#define LPI 0x07760000ULL
#define TX 0x06ae0000ULL
#define VA 0x07660000ULL
#define SWR 0x07630000ULL
static const unsigned pins[4]={0,1,2,14};
static int io_error(void *v) {return ((NaMicBoard *)v)->error ? NA_IO:NA_OK;}
static void latch_error(NaMicBoard *m) {
    if (!m->error && m->lease && m->snapshot_valid)
        m->io.update(m->io.context,m->lease,RESET+4,2,0);
    /* One reset assertion attempt, no replay of the failed bus transaction.
     * Keep the lease even if the independent GPIO write also failed. */
    m->error=1;
}
static int read_reg(NaMicBoard *m,uint64_t a,uint32_t *v) {
    if (m->error) return NA_IO;
    int r=m->io.read(m->io.context,m->lease,a,v);if (r) latch_error(m);return r;
}
static int update(NaMicBoard *m,uint64_t a,uint32_t mask,uint32_t v) {
    if (m->error) return NA_IO;
    int r=m->io.update(m->io.context,m->lease,a,mask,v);if (r) latch_error(m);return r;
}
static uint32_t swr_read(void *v,uint32_t reg) {NaMicBoard *m=v;uint32_t n=0;read_reg(m,SWR+reg,&n);return n;}
static void swr_write(void *v,uint32_t reg,uint32_t value) {NaMicBoard *m=v;update(m,SWR+reg,~0u,value);}
static uint32_t tx_read(void *v,uint32_t reg) {NaMicBoard *m=v;uint32_t n=0;read_reg(m,TX+reg,&n);return n;}
static void tx_write(void *v,uint32_t reg,uint32_t value) {NaMicBoard *m=v;update(m,TX+reg,~0u,value);}
static uint64_t now_us(void *v) {NaMicBoard *m=v;return m->io.now_us(m->io.context);}
static void pause_us(void *v,uint32_t n) {NaMicBoard *m=v;m->io.pause_us(m->io.context,n);}
static int failed(NaMicBoard *m,int r) {
    // GPIO161 remains independent of the clocked audio macro. This fallback
    // cuts capture power even when SoundWire cannot complete a control write.
    latch_error(m);
    return r<0 ? r:NA_IO;
}
int na_mic_board_init(NaMicBoard *m,const NaBoardIo *io,uint8_t bias,uint8_t gain) {
    if (!m || !io || !io->claim || !io->release || !io->read || !io->update || !io->mic_rails || !io->now_us || !io->pause_us ||
        bias!=1 || gain>10) return NA_ARGUMENT;
    memset(m,0,sizeof *m);m->io=*io;m->micbias=bias;m->adc_gain=gain;return NA_OK;
}
int na_mic_board_prepare(void *v,NaWcdCapture *capture) {
    NaMicBoard *m=v;if (!m || !capture || m->error) return NA_OWNERSHIP;
    uint64_t now=now_us(m);if (now>UINT64_MAX-5000000) return NA_OVERFLOW;
    if (!m->deadline) m->deadline=now+5000000;
    if (now>=m->deadline) return failed(m,NA_TIMEOUT);
    if (now<m->wait_until) return NA_PENDING;
    int r=0;uint32_t value=0;
    switch (m->stage) {
    case 0:
        if (!m->lease) {
            r=m->io.claim(m->io.context,NaBoardCapture,&m->lease);
            if (r==NA_BUSY) return NA_PENDING;
            if (r || !m->lease) {m->error=1;return r ? r:NA_OWNERSHIP;}
            if ((r=read_reg(m,RESET,&m->saved_reset))) return failed(m,r);
            for (unsigned i=0;i<4;++i) if ((r=read_reg(m,LPI+4096*pins[i],&m->saved_lpi[i]))) return failed(m,r);
            m->snapshot_valid=1;
        }
        m->rails=1; /* an asynchronous/error vote can already be applied */
        r=m->io.mic_rails(m->io.context,m->lease,1);
        if (r==NA_PENDING || r==NA_BUSY) return NA_PENDING;
        if (r) return failed(m,r);
        if ((r=update(m,RESET+4,2,0)) || (r=update(m,RESET,0x3ff,0x3c0))) return failed(m,r);
        m->stage=1;m->wait_until=now+80;return NA_PENDING;
    case 1:
        if ((r=update(m,RESET+4,2,2))) return failed(m,r);
        m->stage=2;m->wait_until=now+80;return NA_PENDING;
    case 2:
        for (unsigned i=0;i<4;++i) {
            /* func1,4mA,clock no-pull/data bus-hold,slew1 at stock bit11. */
            uint32_t cfg=4u|(1u<<6)|(1u<<11)|(i ? 2:0);
            if ((r=update(m,LPI+4096*pins[i],0x1bff,cfg))) return failed(m,r);
        }
        /* Logical regmap0x3000 is VA register0, after VA_START_OFFSET is
         * subtracted. Never write VA+0x3000. */
        m->macro_on=1; /* retain ownership even on partial initialization */
        if ((r=update(m,VA,1,1)) || (r=update(m,VA+4,3,3)) || (r=update(m,VA+4,3,1)) ||
            (r=update(m,VA+0x80,2,2)) || (r=update(m,VA+8,2,2)) || (r=update(m,VA+8,1,1)) ||
            (r=update(m,VA+8,2,0)) || (r=update(m,TX,1,1)) || (r=update(m,TX+4,1,1))) return failed(m,r);
        m->macro_on=1;
        if ((r=read_reg(m,SWR,&value))) return failed(m,r);
        /* Stock nezha DT selects the2.0 register layout; its driver logs a
         * differing hardware revision and continues.043 read2.2 at this base.
         * Keep the existing layout for these two identified revisions. */
        if (!na_swr_version_supported(value)) return failed(m,NA_MISSING);
        if ((r=update(m,SWR+8,~0u,1))) return failed(m,r);
        m->stage=3;m->wait_until=now+1000;return NA_PENDING;
    case 3: {
        const uint32_t registers[]={0x101c,0x500,0x1048,0x18,0x5060,0x314,0x4,0x4,0x5008,0x5004,0x4};
        const uint32_t values[]={15,1,0x3e0000,1,1,0x80000007,1,2,0x1dfdfd,0,3};
        for (unsigned i=0;i<sizeof registers/sizeof registers[0];++i) {
            uint32_t mask=registers[i]==0x1048 ? 0x3e0000:~0u;
            if ((r=update(m,SWR+registers[i],mask,values[i]))) return failed(m,r);
        }
        m->stage=4;return NA_PENDING;
    }
    case 4:
        if ((r=read_reg(m,SWR+0x5064,&value))) return failed(m,r);
        if (!(value&1)) return NA_PENDING;
        if ((r=read_reg(m,SWR+0x1090,&value))) return failed(m,r);
        if (!(value&0x3ffffcu)) return NA_PENDING;
        {
            NaSwrIo io={m,swr_read,swr_write,now_us,pause_us,io_error};
            r=na_swr_bind(&m->swr,&io,m->lease);
            if (r) return failed(m,r);
            NaTxIo tx={m,tx_read,tx_write,io_error};
            r=na_wcd_capture_init(capture,&m->swr,&tx,m->micbias,m->adc_gain);
            if (r || m->error) return failed(m,r ? r:NA_IO);
        }
        m->stage=5;return NA_OK;
    case 5:return NA_OK;
    default:return NA_OWNERSHIP;
    }
}
int na_mic_board_release(void *v) {
    NaMicBoard *m=v;if (!m) return NA_ARGUMENT;
    if (!m->lease) return NA_OK;
    /* A partial snapshot owns no valid restore image. Retain the lease;
     * never restore uninitialized zero fields over another pin state. */
    if (!m->snapshot_valid) return NA_OWNERSHIP;
    int r;
    /* Caller already stopped graph and DMA. Stop only the audio-owned master
     * before gating its clock; no shared LPASS reset or USBSS write. */
    if ((r=update(m,RESET+4,2,0))) return failed(m,r);
    if (m->macro_on) {
        if ((r=update(m,SWR+0x4,~0u,0)) || (r=update(m,SWR+0x5060,~0u,0))) return failed(m,r);
        if ((r=update(m,VA+8,1,0)) || (r=update(m,TX+4,1,0)) || (r=update(m,TX,1,0)) ||
            (r=update(m,VA+0x80,2,0)) || (r=update(m,VA+4,1,0)) || (r=update(m,VA,1,0))) return failed(m,r);
        m->macro_on=0;
    }
    if (m->rails) {
        r=m->io.mic_rails(m->io.context,m->lease,0);
        if (r==NA_PENDING || r==NA_BUSY) return NA_PENDING;
        if (r) return failed(m,r);m->rails=0;
    }
    for (unsigned i=0;i<4;++i) if ((r=update(m,LPI+4096*pins[i],0x1bff,m->saved_lpi[i]&0x1bff))) return failed(m,r);
    r=m->io.release(m->io.context,NaBoardCapture,m->lease);
    if (r) return failed(m,r);m->lease=0;m->stage=0;m->deadline=0;m->snapshot_valid=0;return NA_OK;
}
