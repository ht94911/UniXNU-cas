/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaBluetoothDiagnostic.h"
#include <QcomUsbDiagnostic.h>
#ifdef KERNEL
#include <sys/systm.h>
#else
#include <string.h>
#endif
struct cached_state { uint64_t state[32], ns; struct qbt_stats stats; uint32_t pending, stopping, stopped; };
static struct cached_state latest;
static uint32_t busy, ready, registered, skipped;
static int take(void) { uint32_t zero=0;return __atomic_compare_exchange_n(&busy,&zero,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); }
static void release(void) { __atomic_store_n(&busy,0,__ATOMIC_RELEASE); }
void nezha_bt_diagnostic_update(const uint64_t state[32],const struct qbt_stats *stats,
        uint64_t ns,uint32_t pending,uint32_t stopping,uint32_t stopped) {
    if(!state || !stats)return;
    if(!take()){__atomic_fetch_add(&skipped,1,__ATOMIC_RELAXED);return;}
    memcpy(latest.state,state,sizeof(latest.state));latest.stats=*stats;latest.ns=ns;
    latest.pending=pending;latest.stopping=stopping;latest.stopped=stopped;
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);release();
}
struct json_output { char *data; size_t capacity, used; bool failed; };
static void append(struct json_output *j,const char *text,size_t length) {
    if(j->failed)return;
    if(length>=j->capacity-j->used){j->failed=true;return;}
    memcpy(j->data+j->used,text,length);j->used+=length;j->data[j->used]=0;
}
static void text(struct json_output *j,const char *value) { append(j,value,strlen(value)); }
static void number(struct json_output *j,uint64_t value) {
    char digits[20];size_t first=sizeof(digits);
    do {digits[--first]=(char)('0'+value%10);value/=10;}while(value);
    append(j,digits+first,sizeof(digits)-first);
}
static void field(struct json_output *j,const char *name,uint64_t value) {text(j,name);number(j,value);}
static int output_init(struct json_output *j,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    if(bytes)*bytes=0;if(truncated)*truncated=0;
    if(!out || !capacity || !bytes || !truncated)return QUD_INVALID;
    out[0]=0;*j=(struct json_output){out,capacity<QUD_DIAG_BYTES?capacity:QUD_DIAG_BYTES,0,false};
    return QUD_OK;
}
static int output_done(struct json_output *j,size_t *bytes,unsigned *truncated) {
    if(j->failed){j->data[0]=0;*truncated=1;return QUD_FAILED;}
    *bytes=j->used;return QUD_OK;
}
static int copy_cache(struct cached_state *s) {
    if(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE) || !take())return QUD_BUSY;
    *s=latest;release();
    if(s->stats.hci.recent_count>QBT_HCI_RECENT_CAPACITY ||
       s->stats.hci.recent_next>=QBT_HCI_RECENT_CAPACITY)return QUD_FAILED;
    return QUD_OK;
}
static const struct qbt_hci_recent *recent(const struct qbt_hci_stats *h,unsigned index) {
    unsigned first=(h->recent_next+QBT_HCI_RECENT_CAPACITY-h->recent_count)%QBT_HCI_RECENT_CAPACITY;
    return &h->recent[(first+index)%QBT_HCI_RECENT_CAPACITY];
}
static void records(struct json_output *j,const struct qbt_hci_stats *h,unsigned first,unsigned count) {
    text(j,"[");
    for(unsigned i=0;i<count;i++){
        const struct qbt_hci_recent *r=recent(h,first+i);
        if(i)text(j,",");text(j,"[");number(j,r->sequence);text(j,",");number(j,r->ns);
        text(j,",");number(j,r->kind);text(j,",");number(j,r->opcode);text(j,",");number(j,r->code);
        text(j,",");number(j,r->status);text(j,",");number(j,r->value);text(j,"]");
    }
    text(j,"]");
}
static const struct qbt_link_recent *link_recent(const struct qbt_link_stats *l,unsigned index) {
    unsigned first=(l->recent_next+QBT_LINK_RECENT_CAPACITY-l->recent_count)%QBT_LINK_RECENT_CAPACITY;
    return &l->recent[(first+index)%QBT_LINK_RECENT_CAPACITY];
}
static void link_records(struct json_output *j,const struct qbt_link_stats *l,unsigned first,unsigned count) {
    text(j,"[");
    for(unsigned i=0;i<count;i++){
        const struct qbt_link_recent *r=link_recent(l,first+i);
        if(i)text(j,",");text(j,"[");number(j,r->sequence);text(j,",");number(j,r->ns);
        text(j,",");number(j,r->kind);text(j,",");number(j,r->opcode);text(j,",");number(j,r->code);
        text(j,",");number(j,r->status);text(j,",");number(j,r->value);text(j,",");number(j,r->handle);text(j,"]");
    }
    text(j,"]");
}
static int link_history(struct json_output *j,const struct cached_state *s,
        const struct qud_diag_request *request,size_t *bytes,unsigned *truncated) {
    const struct qbt_link_stats *l=&s->stats.link;
    if(l->recent_count>QBT_LINK_RECENT_CAPACITY || l->recent_next>=QBT_LINK_RECENT_CAPACITY)return QUD_FAILED;
    uint64_t first=l->recent_count?link_recent(l,0)->sequence:0;
    uint64_t last=l->recent_count?link_recent(l,l->recent_count-1)->sequence:0;
    unsigned begin=0;
    while(begin<l->recent_count && request->cursor && link_recent(l,begin)->sequence<request->cursor)begin++;
    unsigned count=l->recent_count-begin;if(count>12)count=12;
    uint64_t next=begin+count<l->recent_count?link_recent(l,begin+count)->sequence:0;
    field(j,"{\"v\":2,\"stream\":\"link\",\"cached\":true,\"ns\":",s->ns);field(j,",\"total\":",l->recent_total);
    field(j,",\"overwritten\":",l->recent_total>l->recent_count?l->recent_total-l->recent_count:0);
    text(j,",\"cursor_overwritten\":");text(j,request->cursor && first && request->cursor<first?"true":"false");
    field(j,",\"first\":",first);field(j,",\"last\":",last);field(j,",\"next\":",next);text(j,",\"counts\":[");
    /* Fixed qbt_link_stats header order; no addresses or security payloads. */
    const uint64_t counts[]={l->command_errors,l->classic_connections,l->classic_connection_errors,
        l->le_connections,l->le_connection_errors,l->authentication_complete,l->authentication_errors,
        l->pairing_complete,l->pairing_errors,l->encryption_change,l->encryption_errors,l->disconnects,
        l->smp_packets,l->smp_failures,l->acl_observation_dropped,l->recent_total};
    for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);i++){if(i)text(j,",");number(j,counts[i]);}
    text(j,"],\"records\":");link_records(j,l,begin,count);text(j,"}");
    return output_done(j,bytes,truncated);
}
static int snapshot(void *cookie,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    (void)cookie;
    struct json_output j;int error=output_init(&j,out,capacity,bytes,truncated);if(error)return error;
    struct cached_state s;error=copy_cache(&s);if(error)return error;
    field(&j,"{\"version\":1,\"cached\":true,\"ns\":",s.ns);
    field(&j,",\"pending\":",s.pending);field(&j,",\"stopping\":",s.stopping);field(&j,",\"stopped\":",s.stopped);
    field(&j,",\"skipped\":",__atomic_load_n(&skipped,__ATOMIC_RELAXED));text(&j,",\"state\":[");
    for(unsigned i=0;i<32;i++){if(i)text(&j,",");number(&j,s.state[i]);}
    field(&j,"],\"credits\":",s.stats.command_credits);field(&j,",\"credit_events\":",s.stats.command_credit_events);
    field(&j,",\"completed_packets\":",s.stats.completed_packets);field(&j,",\"hardware_errors\":",s.stats.hardware_errors);
    field(&j,",\"backpressure\":",s.stats.backpressure);text(&j,",\"hci\":{\"v\":1,\"counts\":[");
    /* Fixed qbt_hci_stats header order; keep this wire schema independent of padding. */
    const struct qbt_hci_stats *h=&s.stats.hci;
    const uint64_t counts[]={h->commands_tx,h->command_complete,h->command_status,h->command_errors,
        h->inquiry_tx,h->inquiry_cancel_tx,h->inquiry_complete,h->inquiry_result_events,h->announced_devices,
        h->remote_name_completions,h->remote_name_errors,h->le_scan_parameters_tx,h->le_scan_enable_tx,
        h->le_scan_disable_tx,h->scan_responses,h->scan_errors,h->legacy_report_events,h->extended_report_events,
        h->announced_advertisements,h->recent_total};
    for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);i++){if(i)text(&j,",");number(&j,counts[i]);}
    text(&j,"],\"recent\":");unsigned count=h->recent_count<8?h->recent_count:8;
    records(&j,h,h->recent_count-count,count);text(&j,"}}");
    return output_done(&j,bytes,truncated);
}
/* QUD_HISTORY uses the existing submit callback slot, but only reads our cache. */
static int history(void *cookie,const struct qud_diag_request *request,char *out,size_t capacity,
                   size_t *bytes,unsigned *truncated) {
    (void)cookie;
    struct json_output j;int error=output_init(&j,out,capacity,bytes,truncated);if(error)return error;
    if(!request)return QUD_INVALID;
    if(request->op!=QUD_HISTORY)return QUD_UNSUPPORTED;
    if(request->provider!=10 || request->arg0>1 || request->arg1)return QUD_INVALID;
    struct cached_state s;error=copy_cache(&s);if(error)return error;
    if(request->arg0==1)return link_history(&j,&s,request,bytes,truncated);
    const struct qbt_hci_stats *h=&s.stats.hci;
    uint64_t first=h->recent_count?recent(h,0)->sequence:0;
    uint64_t last=h->recent_count?recent(h,h->recent_count-1)->sequence:0;
    unsigned begin=0;
    while(begin<h->recent_count && request->cursor && recent(h,begin)->sequence<request->cursor)begin++;
    unsigned count=h->recent_count-begin;if(count>16)count=16;
    uint64_t next=begin+count<h->recent_count?recent(h,begin+count)->sequence:0;
    field(&j,"{\"v\":1,\"cached\":true,\"ns\":",s.ns);field(&j,",\"total\":",h->recent_total);
    field(&j,",\"overwritten\":",h->recent_total>h->recent_count?h->recent_total-h->recent_count:0);
    text(&j,",\"cursor_overwritten\":");text(&j,request->cursor && first && request->cursor<first?"true":"false");
    field(&j,",\"first\":",first);field(&j,",\"last\":",last);field(&j,",\"next\":",next);text(&j,",\"records\":");
    records(&j,h,begin,count);text(&j,",\"announcements_are_unique_devices\":false}");
    return output_done(&j,bytes,truncated);
}
static const struct qud_diag_provider provider={.id=10,.version=3,.operations=(1u<<QUD_SNAPSHOT)|(1u<<QUD_HISTORY),
    .max_arg0=1,.max_arg1=0,.name="nezha-bluetooth",.cookie=NULL,.snapshot=snapshot,.submit=history};
int nezha_bt_diagnostic_init(void) {
    if(registered)return 1;
    registered=qcom_usb_diag_register(&provider);return registered;
}
