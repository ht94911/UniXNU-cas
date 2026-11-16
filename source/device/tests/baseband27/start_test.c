/* SPDX-License-Identifier: BSD-2-Clause */
#define NB_HOST_TEST 1
#include "../../package/baseband27/integration/NbBasebandStart.c"
#include "../../package/baseband27/integration/NbDiagnostic.c"
#include <assert.h>
static unsigned switch_value,property_size=4,property_present=1,threads,bindings,registrations,diagnostic_error,alloc_count,sleeps,ended,events;
static int thread_error,bind_error=NB_IO,ready_waits;
static uint64_t clock_ms;static void *allocations[32];static void (*entry)(void *,wait_result_t);
static const struct qud_diag_provider *diagnostic;
int SecureDTLookupEntry(void *p,const char *name,DTEntry *e){(void)p;assert(!strcmp(name,"/chosen"));*e=(void *)1;return property_present?0:-1;}
int SecureDTGetProperty(DTEntry e,const char *name,const void **value,unsigned *size){assert(e&&!strcmp(name,"nezha-baseband"));*value=&switch_value;*size=property_size;return 0;}
void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n){*n=t;}uint64_t mach_absolute_time(void){return clock_ms*1000000;}
void *kalloc_data(size_t n,int flags){assert(flags==(Z_WAITOK|Z_ZERO)&&alloc_count<32);void *p=calloc(1,n);allocations[alloc_count++]=p;return p;}
void IOSleep(unsigned n){sleeps++;clock_ms+=n;}
int kernel_thread_start(void (*f)(void *,wait_result_t),void *p,thread_t *t){assert(!p);threads++;entry=f;*t=(void *)2;return thread_error;}
void thread_deallocate(thread_t t){assert(t==(void *)2);}void thread_terminate(thread_t t){assert(t==(void *)3);ended++;}thread_t current_thread(void){return (void *)3;}
int nezha_bb_owners_ready(void){if(ready_waits){ready_waits--;return NB_PENDING;}return NB_OK;}
int nezha_bb_bind(NbBasebandConfig *c){bindings++;if(bind_error)return bind_error;static NbPas loaders[3];c->ipa_fws=&loaders[0];c->modem=&loaders[1];c->modem_dtb=&loaders[2];return 0;}
int nezha_bb_events(void (*a)(void),void (*b)(void)){assert(a&&b);events++;return 0;}
int nezha_bb_dispatch(void){return NB_IO;}
NbBasebandBridge nb_bridge_hooks(NbBaseband *b){assert(b);return (NbBasebandBridge){0};}
void nb_bridge_poll(void){assert(0);}void nb_bridge_report(unsigned n){(void)n;assert(0);}
void nb_bridge_state(int r){assert(r==NB_IO);}void nb_bridge_invalidate(void){}
int nb_baseband_init(NbBaseband *b,const NbBasebandConfig *c){b->c=*c;return 0;}
int nb_baseband_poll(NbBaseband *b,uint64_t t){(void)b;(void)t;assert(0);return NB_IO;}
void nb_baseband_smp2p_event(NbBaseband *b){(void)b;}void nb_baseband_wdog_event(NbBaseband *b){(void)b;}
int nezha_gnss_diagnostic(uint64_t cursor,uint32_t page,char *out,size_t capacity,size_t *used,unsigned *truncated)
{
    (void)cursor;(void)page;(void)out;(void)capacity;*used=0;*truncated=0;
    return QUD_UNSUPPORTED; /* This startup fixture has no LOC endpoint. */
}
int qcom_usb_diag_register(const struct qud_diag_provider *p){assert(p&&p->id==9&&p->version==2&&p->submit);diagnostic=p;registrations++;return !diagnostic_error;}
int main(void){
    property_present=0;nezha_baseband_late_init();property_present=1;switch_value=2;nezha_baseband_late_init();switch_value=1;property_size=8;nezha_baseband_late_init();
    assert(!threads&&!bindings&&!registrations&&!alloc_count);
    property_size=4;ready_waits=2;assert(!nezha_baseband_start()&&threads==1&&registrations==1&&!bindings);
    assert(nezha_baseband_start()==NB_BUSY&&threads==1);entry(NULL,0);assert(bindings==1&&sleeps==2&&ended==1&&nezha_baseband_status==NB_IO);
    uint64_t words[16];assert(!nezha_bb_snapshot(words)&&(int64_t)words[1]==NB_IO);
    char out[512];size_t used;unsigned truncated;assert(diagnostic->snapshot(NULL,out,sizeof out,&used,&truncated)==QUD_OK&&!truncated&&used<sizeof out);
    assert(strstr(out,"\"nezhaBasebandStatus\":-2"));assert(diagnostic->snapshot(NULL,out,1,&used,&truncated)==QUD_FAILED&&truncated);
    nb_stats_sequence=1;assert(diagnostic->snapshot(NULL,out,sizeof out,&used,&truncated)==QUD_BUSY&&!used);nb_stats_sequence=0;
    /* A fresh boot fixture: successful setup, then a terminal owner failure. */
    nb_started=0;bind_error=0;assert(!nezha_baseband_start());entry(NULL,0);assert(bindings==2&&events==1&&alloc_count==6&&ended==2);
    /* A duplicate diagnostic ID must fail before another worker is created. */
    nb_started=0;diagnostic_error=1;unsigned previous_threads=threads;
    assert(nezha_baseband_start()==NB_IO&&threads==previous_threads);
    diagnostic_error=0;
    nb_started=0;thread_error=1;unsigned before=bindings;assert(nezha_baseband_start()==NB_IO&&bindings==before);assert(nezha_baseband_start()==NB_BUSY);
    for(unsigned i=0;i<alloc_count;i++)free(allocations[i]);
    puts("PASS late start: off has no side effects, once only, readiness, failure, bounded consistent provider");
}
