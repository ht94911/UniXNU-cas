/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomFastRpc.h"
#include <string.h>
/* fastrpc_shared.h: nscalars * (remote_arg16 + invoke_buf8 + phy_page16),
 * fdlist16*u64 + crclist64*u32 + poll*u32 + dsp_perf12*u64. */
#define META_FIXED (16u*8u+64u*4u+4u+12u*8u)
static uint32_t get32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p) {return get32(p)|(uint64_t)get32(p+4)<<32;}
static void put32(uint8_t *p,uint32_t x) {for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(x>>(8*i));}
static void put64(uint8_t *p,uint64_t x) {put32(p,(uint32_t)x);put32(p+4,(uint32_t)(x>>32));}
void qcom_rpc_lost(QcomFastRpc *r,int error) {
    if (!r || !r->glink || r->error) return;
    r->error=error<0 ? error:QcomRpcLost;r->online=0;
    for (unsigned i=0;i<QCOM_RPC_SLOTS;++i) if (r->call[i].state!=QcomRpcFree) r->call[i].state=QcomRpcRetained;
}
static void state(void *v,QcomGlinkChannel *channel,int up) {
    QcomFastRpc *r=v;(void)channel;
    if (!up && r->selected) {qcom_rpc_lost(r,QcomRpcLost);return;}
    if (!r->error && up) {r->online=1;r->selected=1;}
}
static void receive(void *v,QcomGlinkChannel *channel,const uint8_t *data,uint32_t bytes) {
    QcomFastRpc *r=v;(void)channel;
    if (r->error) return;
    /* FASTRPC signal messages have no invoke result. No signal was requested
     * by this client, so retain the count without completing any call. */
    if (bytes==8) {++r->signals;return;}
    /* ARM64 sizeof basic response is16; v2 is24. Its padding is not data. */
    if (bytes!=16 && bytes!=24) {qcom_rpc_lost(r,QcomRpcWire);return;}
    uint64_t context=get64(data)&~UINT64_C(15);QcomRpcCall *c=0;unsigned index=0;
    for (unsigned i=0;i<QCOM_RPC_SLOTS;++i) if (r->call[i].state==QcomRpcAwaiting && (r->call[i].context&~UINT64_C(15))==context) {c=&r->call[i];index=i;break;}
    if (!c) {++r->stale;return;}
    if (bytes==24) {
        uint32_t flags=get32(data+12),version=get32(data+20);
        if (version!=2 || flags>5) {qcom_rpc_lost(r,QcomRpcWire);return;}
        if (flags==1 || flags==2 || flags==5) {
            if (flags==1) {c->early_response=1;c->result=(int32_t)get32(data+8);}
            ++r->early;return;
        }
        if (flags==4) {qcom_rpc_lost(r,QcomRpcLost);return;}
    }
    if (r->ops.sync(r->ops.context,index*QCOM_RPC_SLOT_BYTES,c->total_bytes,0)) {qcom_rpc_lost(r,QcomRpcSync);return;}
    c->result=(int32_t)get32(data+8);
    if (!c->result) for (unsigned i=c->inbufs;i<c->inbufs+c->outbufs;++i)
        if (c->args[i].bytes) memcpy(c->args[i].data,r->memory+index*QCOM_RPC_SLOT_BYTES+c->offset[i],c->args[i].bytes);
    c->state=QcomRpcComplete;++r->received;
}
int qcom_rpc_prepare(QcomFastRpc *r,QcomGlink *g,uint8_t *memory,uint32_t bytes,
                      uint64_t address,uint32_t pid,uint32_t tid,const QcomRpcOps *ops) {
    if (!r || r->glink || !g || !memory || ((uintptr_t)memory&4095) ||
        bytes!=QCOM_RPC_SLOTS*QCOM_RPC_SLOT_BYTES || !address || (address&4095) ||
        (address&UINT64_C(0xffffffff))+bytes>UINT64_C(0x100000000) ||
        !pid || pid>INT32_MAX || !tid || tid>=INT32_MAX || !ops || !ops->sync) return QcomRpcArgument;
    memset(r,0,sizeof *r);r->glink=g;r->ops=*ops;r->memory=memory;r->bytes=bytes;
    r->dsp_address=address;r->pid=pid;r->tid=tid;
    QcomGlinkClient client={state,receive};
    r->channel=qcom_glink_channel(g,"fastrpcglink-apps-dsp",QCOM_RPC_RX_BYTES,8,r->intents,&client,r,0);
    if (!r->channel) {qcom_rpc_lost(r,QcomRpcNoSpace);return r->error;}
    return QcomRpcOkay;
}
int qcom_rpc_begin(QcomFastRpc *r,uint32_t handle,uint32_t sc,const QcomRpcArg *args,
                   uint32_t count,int zero,uint64_t now,uint64_t timeout,uint32_t *token) {
    if (!r || !r->glink || !token || count>QCOM_RPC_ARGS || (count && !args) ||
        (sc&0xff) || count!=((sc>>16)&255)+((sc>>8)&255) ||
        (!timeout && (handle!=7 || sc!=0x06020200)) ||
        now>UINT64_MAX-timeout || zero<0 || zero>1) return QcomRpcArgument;
    if (r->error) return r->error;
    if (!r->online) return QcomRpcAgain;
    unsigned index=0;while (index<QCOM_RPC_SLOTS && r->call[index].state!=QcomRpcFree) ++index;
    if (index==QCOM_RPC_SLOTS) return QcomRpcNoSpace;
    if (r->job>=UINT64_MAX>>16) return QcomRpcArgument;
    uint32_t metadata=count*40+META_FIXED,at=(metadata+127)&~127u;
    QcomRpcCall c={0};c.handle=handle;c.scalar=sc;c.inbufs=(sc>>16)&255;c.outbufs=(sc>>8)&255;
    c.metadata_bytes=metadata;
    for (unsigned i=0;i<count;++i) {
        if ((args[i].bytes && !args[i].data) || args[i].bytes>QCOM_RPC_SLOT_BYTES) return QcomRpcArgument;
        at=(at+127)&~127u;
        if (args[i].bytes>QCOM_RPC_SLOT_BYTES-at) return QcomRpcNoSpace;
        c.offset[i]=at;c.args[i]=args[i];at+=args[i].bytes;
    }
    uint8_t *mem=r->memory+index*QCOM_RPC_SLOT_BYTES;
    memset(mem,0,at);uint64_t address=r->dsp_address+index*QCOM_RPC_SLOT_BYTES;
    for (unsigned i=0;i<count;++i) {
        uint32_t n=args[i].bytes,offset=c.offset[i];
        put64(mem+16*i,n ? (uint64_t)(uintptr_t)(mem+offset):0);put64(mem+16*i+8,n);
        uint8_t *list=mem+count*16+i*8,*page=mem+count*24+i*16;
        put32(list,n ? 1:0);put32(list+4,i);
        if (n) {
            put64(page,(address+offset)&~UINT64_C(4095));
            put64(page+8,((offset&4095)+n+4095)&~4095u);
            if (i<c.inbufs) memcpy(mem+offset,args[i].data,n);
        }
    }
    c.total_bytes=at;c.context=(++r->job<<16)|((uint64_t)(index+1)<<6)|2;
    c.deadline_us=timeout ? now+timeout:0;c.state=QcomRpcQueued;
    /* DSP serializes calls by the HLOS logical thread ID. The remote next2
     * waits for AP work; control open/register must use another thread ID to
     * run while it is pending. Both still execute on this sole AP worker. */
    uint32_t tid=handle==7 && (sc==0x06020200 || sc==0x07010200) ? r->tid+1:r->tid;
    put32(c.message,zero ? 0:r->pid);put32(c.message+4,tid);put64(c.message+8,c.context);
    put32(c.message+16,handle);put32(c.message+20,sc);put64(c.message+24,address);
    put64(c.message+32,(metadata+4095)&~4095u);
    if (r->ops.sync(r->ops.context,index*QCOM_RPC_SLOT_BYTES,at,1)) return QcomRpcSync;
    r->call[index]=c;*token=index+1;return QcomRpcOkay;
}
int qcom_rpc_poll(QcomFastRpc *r,uint64_t now) {
    if (!r || !r->glink) return QcomRpcArgument;
    if (r->error) return r->error;
    if (r->glink->broken) {qcom_rpc_lost(r,QcomRpcLost);return r->error;}
    for (unsigned i=0;i<QCOM_RPC_SLOTS;++i) {
        QcomRpcCall *c=&r->call[i];
        if (c->state==QcomRpcAwaiting && c->early_response) {
            uint32_t count=c->inbufs+c->outbufs,poll=count*40+16*8+64*4;
            if (r->ops.sync(r->ops.context,i*QCOM_RPC_SLOT_BYTES,c->total_bytes,0)) {qcom_rpc_lost(r,QcomRpcSync);return r->error;}
            uint32_t value=get32(r->memory+i*QCOM_RPC_SLOT_BYTES+poll);
            /* fastrpc_wait_for_completion accepts the two published words
             * after EARLY_RESPONSE; an early wire hint alone never does. */
            if (value==0xabbccdde || value==0xdecaf) {
                if (value==0xdecaf) c->result=0;
                if (!c->result) for (unsigned j=c->inbufs;j<count;++j) if (c->args[j].bytes)
                    memcpy(c->args[j].data,r->memory+i*QCOM_RPC_SLOT_BYTES+c->offset[j],c->args[j].bytes);
                c->state=QcomRpcComplete;++r->received;
            }
        }
        if ((c->state==QcomRpcQueued || c->state==QcomRpcAwaiting) && c->deadline_us && now>=c->deadline_us) {
            qcom_rpc_lost(r,QcomRpcTimeout);return r->error;
        }
        if (c->state!=QcomRpcQueued) continue;
        int result=qcom_glink_send(r->glink,r->channel,c->message,sizeof c->message);
        if (result==QcomGlinkAgain) continue;
        if (result) {qcom_rpc_lost(r,QcomRpcLost);return r->error;}
        c->state=QcomRpcAwaiting;++r->sent;
    }
    return QcomRpcOkay;
}
int qcom_rpc_take(QcomFastRpc *r,uint32_t token,int32_t *result) {
    if (!r || !result || !token || token>QCOM_RPC_SLOTS) return QcomRpcArgument;
    if (r->error) return r->error;
    QcomRpcCall *c=&r->call[token-1];
    if (c->state==QcomRpcFree) return QcomRpcArgument;
    if (c->state!=QcomRpcComplete) return QcomRpcAgain;
    *result=c->result;memset(c,0,sizeof *c);return QcomRpcOkay;
}
