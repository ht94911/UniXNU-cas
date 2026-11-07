/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GPU015 integration: one read-only resource census after touch initialization.
 * No ADSP/PAS/GLINK/LPASS/TAS access, audio publication or shared register writes.
 * na=1 enables only this census; na=2 selects the separate explicit DSP/TAS
 * diagnostic. Other values leave audio hardware untouched. */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/clock_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <QcomUsbDiagnostic.h>
#include "../platform/AudioResourceProbe.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern volatile int nezha_touch_stage;
void nezha_audio27_start(void);
extern void nezha_audio27_dsp_probe_start(void);
static unsigned started,state; /* 1 waiting,2 reading,3 complete,4 failed */
static int native_error;
static NaResourceSnapshot snapshot;
static vm_offset_t maps[3];
static const uint32_t map_bytes[3]={0x100000,0x10000,0x1000};
static int read32(void *cookie,unsigned domain,uint32_t offset,uint32_t *v) {
    (void)cookie;
    if (domain>NaProbeGcc || !v || !maps[domain] || (offset&3) || offset>map_bytes[domain]-4) return -1;
    __asm__ volatile("dsb sy" ::: "memory");
    *v=*(const volatile uint32_t *)(maps[domain]+offset);
    __asm__ volatile("dsb sy" ::: "memory");return 0;
}
static int read64(void *cookie,unsigned domain,uint32_t offset,uint64_t *v) {
    (void)cookie;
    if (domain!=NaProbeSmmu || !v || !maps[domain] || (offset&7) || offset>map_bytes[domain]-8) return -1;
    __asm__ volatile("dsb sy" ::: "memory");
    *v=*(const volatile uint64_t *)(maps[domain]+offset);
    __asm__ volatile("dsb sy" ::: "memory");return 0;
}
struct writer {char *out;size_t capacity,used;unsigned failed;};
static void add(struct writer *w,const char *fmt,...) __attribute__((format(printf,2,3)));
static void add(struct writer *w,const char *fmt,...) {
    if (w->failed) return;
    va_list args;va_start(args,fmt);int n=vsnprintf(w->out+w->used,w->capacity-w->used,fmt,args);va_end(args);
    if (n<0 || (size_t)n>=w->capacity-w->used) {w->failed=1;return;}
    w->used+=(size_t)n;
}
static int finish(struct writer *w,size_t *bytes,unsigned *trunc) {
    *bytes=w->failed ? 0:w->used;*trunc=w->failed;return w->failed ? QUD_FAILED:QUD_OK;
}
static int status(void *cookie,char *out,size_t cap,size_t *bytes,unsigned *trunc) {
    (void)cookie;unsigned phase=__atomic_load_n(&state,__ATOMIC_ACQUIRE);
    struct writer w={out,cap,0,0};
    add(&w,"{\"v\":1,\"read_only\":true,\"ownership_granted\":false,\"state\":%u",phase);
    if (phase>=3) {
        const NaResourceSnapshot *s=&snapshot;
        add(&w,",\"error\":%d,\"phase\":%u,\"last_domain\":%u,\"last_offset\":%u,\"reads\":%u",
            native_error,s->phase,s->domain,s->offset,s->reads);
        add(&w,",\"smmu_ids\":[%u,%u,%u],\"streams\":%u,\"banks\":%u,\"stage2_banks\":%u,\"page_bytes\":%u",
            s->id[0],s->id[1],s->id[2],s->stream_count,s->bank_count,s->stage2_banks,s->page_bytes);
        add(&w,",\"rsc_id\":%u,\"rsc_config\":%u,\"gcc_vote\":%u,\"tcs\":[",s->rsc_id,s->rsc_config,s->gcc_vote);
        for (unsigned i=0;i<3;++i) add(&w,"%s[%u,%u,%u,%u]",i?",":"",s->tcs[i][0],s->tcs[i][1],s->tcs[i][2],s->tcs[i][3]);
        add(&w,"]");
    }
    add(&w,"}");return finish(&w,bytes,trunc);
}
static int history(void *cookie,const struct qud_diag_request *r,char *out,size_t cap,size_t *bytes,unsigned *trunc) {
    (void)cookie;
    if (r->op!=QUD_HISTORY) return QUD_UNSUPPORTED;
    if (r->arg0>1 || r->arg1 || r->experiment) return QUD_INVALID;
    unsigned phase=__atomic_load_n(&state,__ATOMIC_ACQUIRE);
    if (phase<3) return QUD_BUSY;
    /* Failed snapshots may be partial: do not label their zeroed rows as real
     * hardware data. The status still preserves the exact failed read. */
    if (phase!=3) return QUD_FAILED;
    unsigned row_count=r->arg0 ? snapshot.bank_count:snapshot.stream_count;
    if (r->cursor>row_count || (r->arg0 && r->cursor<snapshot.stage2_banks)) return QUD_INVALID;
    unsigned at=(unsigned)r->cursor,end=at+8;if (end>row_count) end=row_count;
    struct writer w={out,cap,0,0};
    add(&w,"{\"v\":1,\"kind\":%u,\"first\":%u,\"next\":%u,\"total\":%u,\"rows\":[",r->arg0,at,end,row_count);
    for (unsigned i=at;i<end;++i) {
        if (r->arg0) {
            const NaProbeBank *b=&snapshot.bank[i];
            add(&w,"%s[%u,%u,%u,%u,%u,%u,%u]",i==at?"":",",i,b->sctlr,b->actlr,b->cbar,b->cba2r,b->asid0,b->asid1);
        } else add(&w,"%s[%u,%u,%u]",i==at?"":",",i,snapshot.stream[i].smr,snapshot.stream[i].s2cr);
    }
    add(&w,"]}");return finish(&w,bytes,trunc);
}
/* ID8 is unused by GPU015 (GPU=7, USB=1). Registration fails closed on a
 * collision. The existing protocol owns session/sequence checks and budgets. */
static const struct qud_diag_provider provider={8,1,(1u<<QUD_SNAPSHOT)|(1u<<QUD_HISTORY),1,0,"nezhaaudio",NULL,status,history};
static uint64_t milliseconds(void) {uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
static void probe_thread(void *arg,wait_result_t result) {
    (void)arg;(void)result;uint64_t deadline=milliseconds()+120000;
    /* Stage7 follows the existing RSC/GCC/SMMU initialization in pt_thread.
     * Later touch recovery resets its endpoint only. UFS remains active. */
    while (__atomic_load_n(&nezha_touch_stage,__ATOMIC_ACQUIRE)!=7) {
        if (milliseconds()>=deadline) {native_error=NA_TIMEOUT;goto finished;}
        assert_wait_timeout((event_t)&snapshot,THREAD_UNINT,100,NSEC_PER_MSEC);
        thread_block(THREAD_CONTINUE_NULL);
    }
    __atomic_store_n(&state,2,__ATOMIC_RELEASE);
    /* These are observation mappings, not NaNativeWindow ownership leases.
     * They remain mapped until reboot and never expose a write API. */
    maps[0]=ml_io_map(0x15000000,map_bytes[0]);
    maps[1]=ml_io_map(0x18920000,map_bytes[1]);
    maps[2]=ml_io_map(0x00152000,map_bytes[2]);
    if (!maps[0] || !maps[1] || !maps[2]) {native_error=NA_IO;goto finished;}
    printf("NEZHA AUDIO27: read-only apps SMMU/RSC/GCC census, no hardware lease or audio start\n");
    NaResourceReader reader={NULL,read32,read64};native_error=na_resource_capture(&snapshot,&reader);
finished:
    printf("NEZHA AUDIO27: census error=%d phase=%u reads=%u ids=%x/%x/%x streams=%u banks=%u\n",
        native_error,snapshot.phase,snapshot.reads,snapshot.id[0],snapshot.id[1],snapshot.id[2],snapshot.stream_count,snapshot.bank_count);
    __atomic_store_n(&state,native_error ? 4:3,__ATOMIC_RELEASE);
    (void)thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
void nezha_audio27_start(void) {
    unsigned option=0;
    if (!PE_parse_boot_argn("na",&option,sizeof option)) return;
    if (option==2) {nezha_audio27_dsp_probe_start();return;}
    if (option!=1) return;
    if (__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL)) return;
    __atomic_store_n(&state,1,__ATOMIC_RELEASE);
    if (!qcom_usb_diag_register(&provider)) {native_error=NA_BUSY;__atomic_store_n(&state,4,__ATOMIC_RELEASE);return;}
    thread_t thread=THREAD_NULL;
    if (kernel_thread_start(probe_thread,NULL,&thread)!=KERN_SUCCESS) {
        native_error=NA_IO;__atomic_store_n(&state,4,__ATOMIC_RELEASE);return;
    }
    thread_deallocate(thread);
}
