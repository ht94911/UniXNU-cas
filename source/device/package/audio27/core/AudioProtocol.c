/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioProtocol.h"
#include <string.h>
#include <limits.h>
static uint32_t rd(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void wr(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
void na_session_init(NaSession *s) { memset(s, 0, sizeof *s); s->generation=1; }
int na_session_link(NaSession *s, int online) {
    if (!s) return NA_ARGUMENT;
    if (s->exhausted && online) return NA_OVERFLOW;
    if (s->online == (uint32_t)!!online) return NA_OK;
    if (s->generation == UINT32_MAX) { s->exhausted=1; online=0; }
    else ++s->generation;
    s->online=(uint32_t)!!online;
    for (unsigned i=0;i<NA_TRANSACTIONS;++i)
        if (s->transaction[i].token && s->transaction[i].state==NA_PENDING)
            s->transaction[i].state=NA_OFFLINE;
    return s->exhausted ? NA_OVERFLOW : NA_OK;
}
uint32_t na_token(NaSession *s) {
    if (!s || !s->online || s->exhausted) return 0;
    if (s->next_token==UINT32_MAX) { s->exhausted=1; return 0; }
    return ++s->next_token;
}
int na_expect(NaSession *s, uint32_t token, uint32_t port, uint32_t peer,
              uint32_t opcode, uint32_t response, uint64_t now, uint32_t timeout) {
    if (!s || !token || token!=s->next_token || token<=s->last_expected_token || !port || !peer || !opcode ||
        !response || !timeout || timeout>10000 || now>UINT64_MAX-timeout) return NA_ARGUMENT;
    if (!s->online) return NA_OFFLINE;
    NaTransaction *free_slot=0;
    for (unsigned i=0;i<NA_TRANSACTIONS;++i) {
        NaTransaction *t=&s->transaction[i];
        if (t->token==token) return NA_BUSY;
        if (!t->token && !free_slot) free_slot=t;
    }
    if (!free_slot) return NA_BUSY;
    *free_slot=(NaTransaction){token,s->generation,port,peer,opcode,response,
                               now+timeout,NA_PENDING,0};
    s->last_expected_token=token;
    return NA_OK;
}
int na_result(NaSession *s,uint32_t token,uint32_t *value) {
    if (!s || !token || !value) return NA_ARGUMENT;
    for (unsigned i=0;i<NA_TRANSACTIONS;++i) {
        NaTransaction *t=&s->transaction[i];
        if (t->token!=token) continue;
        if (t->state==NA_PENDING) return NA_PENDING;
        int result=t->state; *value=t->value; memset(t,0,sizeof *t); return result;
    }
    return NA_MISSING;
}
int na_cancel_unsent(NaSession *s,uint32_t token) {
    if (!s || !token) return NA_ARGUMENT;
    for (unsigned i=0;i<NA_TRANSACTIONS;++i) {
        NaTransaction *t=&s->transaction[i];
        if (t->token==token && t->state==NA_PENDING) { memset(t,0,sizeof *t); return NA_OK; }
    }
    return NA_OWNERSHIP;
}
int na_packet(const uint8_t *data,uint32_t length,GprPacket *p) {
    if (!data || !p || length>GPR_MAX_PACKET || gpr_decode(data,length,p)!=GprOk ||
        p->header_bytes!=GPR_HEADER_BYTES || p->size!=length || (rd(data+4)&0xffff0000u) ||
        p->dst_domain!=GPR_DOMAIN_APPS || p->src_domain!=GPR_DOMAIN_ADSP) return NA_WIRE;
    return NA_OK;
}
static int malformed_reply(NaSession *s,NaTransaction *t) {
    ++s->malformed;t->state=NA_WIRE;t->value=0;return NA_WIRE;
}
int na_receive(NaSession *s,const uint8_t *data,uint32_t length) {
    GprPacket p;
    if (!s) return NA_ARGUMENT;
    if (na_packet(data,length,&p)) { ++s->malformed; return NA_WIRE; }
    if (!s->online) return NA_OFFLINE;
    for (unsigned i=0;i<NA_TRANSACTIONS;++i) {
        NaTransaction *t=&s->transaction[i];
        if (!t->token || t->state!=NA_PENDING || t->generation!=s->generation ||
            t->token!=p.token || t->port!=p.dst_port || t->peer!=p.src_port) continue;
        uint32_t value=0;
        if (p.opcode==GPR_OP_BASIC_RESULT) {
            if (p.payload_bytes!=8) return malformed_reply(s,t);
            if (rd(p.payload)!=t->opcode) continue;
            value=rd(p.payload+4);
            /* MAP/PRM/SPF success needs its dedicated response, not a basic ack. */
            if (!value && t->response!=GPR_OP_BASIC_RESULT) return NA_IGNORED;
        } else if (p.opcode==t->response) {
            if (p.opcode==AR_OP_PRM_REQUEST_DONE || p.opcode==AR_OP_PRM_RELEASE_DONE) {
                /* Current q6prm/sheng supports status or {resource,status}. */
                if (p.payload_bytes!=4 && p.payload_bytes!=8) return malformed_reply(s,t);
                value=rd(p.payload+p.payload_bytes-4);
            } else if (p.opcode==AR_OP_MEM_MAP_DONE || p.opcode==GPR_OP_APM_SPF_STATE) {
                if (p.payload_bytes!=4) return malformed_reply(s,t);
                t->value=rd(p.payload);
                t->state=(p.opcode==AR_OP_MEM_MAP_DONE ? t->value!=0 : t->value==1) ? NA_OK : NA_REMOTE;
                return t->state;
            } else return malformed_reply(s,t);
        } else continue;
        t->value=value; t->state=value ? NA_REMOTE : NA_OK;
        if (value) ++s->remote_errors;
        return t->state;
    }
    ++s->ignored; return NA_IGNORED;
}
void na_expire(NaSession *s,uint64_t now) {
    if (!s) return;
    for (unsigned i=0;i<NA_TRANSACTIONS;++i) {
        NaTransaction *t=&s->transaction[i];
        if (t->token && t->state==NA_PENDING && now>=t->deadline_ms) {
            t->state=NA_TIMEOUT; ++s->timeouts;
        }
    }
}
uint32_t na_buffer_packet(uint8_t out[GPR_MAX_PACKET],uint32_t port,uint32_t endpoint,
                         uint32_t token,int capture,uint64_t address,uint32_t handle,uint32_t bytes) {
    if (!out || !port || !endpoint || !token || !handle || !bytes || (address&7u)) return 0;
    uint32_t n=capture ? ar_read(out,GPR_MAX_PACKET,port,token,address,handle,bytes) :
                         ar_write(out,GPR_MAX_PACKET,port,token,address,handle,bytes);
    if (n) wr(out+12,endpoint);
    return n;
}
uint32_t na_oob_packet_to(uint8_t out[GPR_MAX_PACKET],uint32_t port,uint32_t peer,uint32_t token,uint32_t opcode,
                      uint64_t address,uint32_t handle,uint32_t bytes) {
    if (!out || !peer || !port || !token || !handle || !bytes || (address&7) ||
        (opcode!=AR_OP_GRAPH_OPEN && opcode!=AR_OP_SET_CFG)) return 0;
    uint8_t payload[16]; wr(payload,(uint32_t)address);wr(payload+4,(uint32_t)(address>>32));
    wr(payload+8,handle);wr(payload+12,bytes);
    GprPacket p={.dst_domain=GPR_DOMAIN_ADSP,.src_domain=GPR_DOMAIN_APPS,
        .src_port=port,.dst_port=peer,.token=token,.opcode=opcode};
    return gpr_encode(out,GPR_MAX_PACKET,&p,payload,sizeof payload);
}
uint32_t na_oob_packet(uint8_t out[GPR_MAX_PACKET],uint32_t port,uint32_t token,uint32_t opcode,
                      uint64_t address,uint32_t handle,uint32_t bytes) {
    return na_oob_packet_to(out,port,AR_APM_IID,token,opcode,address,handle,bytes);
}
uint32_t na_subgraphs_packet(uint8_t out[GPR_MAX_PACKET],uint32_t port,uint32_t token,uint32_t opcode,
                            const uint32_t *ids,uint32_t count) {
    if (!out || !ids || !count || count>16 || !port || !token ||
        (opcode!=AR_OP_GRAPH_PREPARE && opcode!=AR_OP_GRAPH_START &&
         opcode!=AR_OP_GRAPH_STOP && opcode!=AR_OP_GRAPH_CLOSE)) return 0;
    uint8_t payload[104]={0};
    uint32_t param_size=(4+count*4+7)&~7u;
    wr(payload+12,16+param_size); wr(payload+16,AR_APM_IID);
    wr(payload+20,0x08001005u); /* APM_PARAM_ID_SUB_GRAPH_LIST, sheng/Linux */
    wr(payload+24,param_size); wr(payload+32,count);
    for (uint32_t i=0;i<count;++i) {
        if (!ids[i]) return 0;
        for (uint32_t j=0;j<i;++j) if (ids[i]==ids[j]) return 0;
        wr(payload+36+4*i,ids[i]);
    }
    GprPacket p={.dst_domain=GPR_DOMAIN_ADSP,.src_domain=GPR_DOMAIN_APPS,
        .src_port=port,.dst_port=AR_APM_IID,.token=token,.opcode=opcode};
    return gpr_encode(out,GPR_MAX_PACKET,&p,payload,32+param_size);
}
int na_rx_push(NaRxQueue *q,uint32_t generation,const uint8_t *data,uint32_t length) {
    if (!q || !data || !generation) return NA_ARGUMENT;
    if (length<GPR_HEADER_BYTES || length>GPR_MAX_PACKET) {
        __atomic_store_n(&q->fault,1,__ATOMIC_RELEASE);return NA_WIRE;
    }
    uint32_t h=__atomic_load_n(&q->head,__ATOMIC_RELAXED);
    uint32_t t=__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE);
    if (h-t>=NA_RX_SLOTS) { __atomic_store_n(&q->fault,1,__ATOMIC_RELEASE);return NA_OVERFLOW; }
    NaRxPacket *p=&q->slots[h%NA_RX_SLOTS];p->generation=generation;p->length=length;
    memcpy(p->data,data,length);__atomic_store_n(&q->head,h+1,__ATOMIC_RELEASE);return NA_OK;
}
int na_rx_pop(NaRxQueue *q,NaRxPacket *p) {
    if (!q || !p) return NA_ARGUMENT;
    if (__atomic_load_n(&q->fault,__ATOMIC_ACQUIRE)) return NA_OVERFLOW;
    uint32_t t=__atomic_load_n(&q->tail,__ATOMIC_RELAXED);
    if (t==__atomic_load_n(&q->head,__ATOMIC_ACQUIRE)) return NA_PENDING;
    *p=q->slots[t%NA_RX_SLOTS];__atomic_store_n(&q->tail,t+1,__ATOMIC_RELEASE);return NA_OK;
}
int na_rx_link(NaRxQueue *q,uint32_t generation,int online) {
    if (!q || !generation) return NA_ARGUMENT;
    uint32_t h=__atomic_load_n(&q->head,__ATOMIC_RELAXED);
    uint32_t t=__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE);
    if (h-t>=NA_RX_SLOTS) {__atomic_store_n(&q->fault,1,__ATOMIC_RELEASE);return NA_OVERFLOW;}
    NaRxPacket *p=&q->slots[h%NA_RX_SLOTS];
    p->generation=generation;p->length=0;p->data[0]=(uint8_t)!!online;
    __atomic_store_n(&q->head,h+1,__ATOMIC_RELEASE);return NA_OK;
}
