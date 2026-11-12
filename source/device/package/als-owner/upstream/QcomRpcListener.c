/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Qualcomm listener1 generated methods5/6 and listener_buf.h packing. */
#include "QcomRpcListener.h"
#include <string.h>
static const char default_listener_uri[]="file:///libadsp_default_listener1_skel.so?adsp_default_listener1_skel_handle_invoke&_modver=1.0&_dom=adsp";
static uint32_t get32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t x) {for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(x>>(8*i));}
static int fail(QcomRpcListener *l,int r) {l->error=r ? r:QcomRpcWire;l->phase=QcomListenerFailed;return l->error;}
int qcom_listener_prepare(QcomRpcListener *l,QcomFastRpc *r,const QcomRpcProvider *provider) {
    if (!l || l->rpc || !r || !r->glink || !provider || !provider->invoke) return QcomRpcArgument;
    memset(l,0,sizeof *l);l->rpc=r;l->provider=*provider;l->phase=QcomListenerAttach;
    l->previous_result=UINT32_MAX;return QcomRpcOkay;
}
static int control(QcomRpcListener *l,uint64_t now) {
    QcomRpcArg args[4];uint32_t count=0,handle=0,scalar=0;int zero=0;
    switch (l->phase) {
        case QcomListenerAttach:
            l->primitive_in[0]=l->rpc->pid;args[0]=(QcomRpcArg){l->primitive_in,4};
            count=1;handle=1;scalar=0x00010000;zero=1;break;
        case QcomListenerInit:handle=7;scalar=0x05000000;break;
        case QcomListenerOpen:
            l->open_in[0]=sizeof default_listener_uri;l->open_in[1]=sizeof l->open_error;
            l->open_out[0]=UINT32_MAX;l->open_out[1]=UINT32_MAX;
            args[0]=(QcomRpcArg){l->open_in,8};args[1]=(QcomRpcArg){(void *)default_listener_uri,sizeof default_listener_uri};
            args[2]=(QcomRpcArg){l->open_out,8};args[3]=(QcomRpcArg){l->open_error,sizeof l->open_error};
            count=4;handle=8;scalar=0x02020200;break; /* remotectl1_open1 */
        case QcomListenerRegister:handle=l->remote_handle;scalar=0x02000000;break;
        default:return QcomRpcArgument;
    }
    return qcom_rpc_begin(l->rpc,handle,scalar,args,count,zero,now,UINT64_C(5000000),&l->control_token);
}
static int next(QcomRpcListener *l,uint64_t now) {
    l->primitive_in[0]=l->previous_context;l->primitive_in[1]=l->previous_result;
    l->primitive_in[2]=l->output_bytes;l->primitive_in[3]=sizeof l->input;
    memset(l->primitive_out,0,sizeof l->primitive_out);l->primitive_out[2]=UINT32_MAX;
    QcomRpcArg args[]={{l->primitive_in,16},{l->output,l->output_bytes},
                      {l->primitive_out,16},{l->input,sizeof l->input}};
    return qcom_rpc_begin(l->rpc,7,0x06020200,args,4,0,now,0,&l->listener_token);
}
/* The wire contains length, 8-aligned data for every input, then only output
 * lengths. Output reply uses the same length + aligned data framing. */
static int dispatch(QcomRpcListener *l) {
    uint32_t sc=l->primitive_out[2],n=(sc>>16)&255,m=(sc>>8)&255,used=l->primitive_out[3];
    if ((sc&255) || n+m>QCOM_RPC_ARGS || used>sizeof l->input || !l->primitive_out[0]) return QcomRpcWire;
    QcomRpcArg args[QCOM_RPC_ARGS]={0};uint32_t at=0,out=0;
    for (unsigned i=0;i<n;++i) {
        if (at>used || used-at<4) return QcomRpcWire;
        uint32_t bytes=get32(l->input+at);at+=4;
        if (bytes) {at=(at+7)&~7u;if (at>used || bytes>used-at) return QcomRpcWire;}
        args[i]=(QcomRpcArg){bytes ? l->input+at:0,bytes};at+=bytes;
    }
    for (unsigned i=0;i<m;++i) {
        if (at>used || used-at<4 || out>sizeof l->output-4) return QcomRpcWire;
        uint32_t bytes=get32(l->input+at);at+=4;put32(l->output+out,bytes);out+=4;
        if (bytes) {out=(out+7)&~7u;if (out>sizeof l->output || bytes>sizeof l->output-out) return QcomRpcNoSpace;}
        args[n+i]=(QcomRpcArg){bytes ? l->output+out:0,bytes};
        if (bytes) memset(l->output+out,0,bytes);
        out+=bytes;
    }
    l->previous_context=l->primitive_out[0];
    l->previous_result=(uint32_t)l->provider.invoke(l->provider.context,l->primitive_out[1],sc,args,n+m);
    l->output_bytes=l->previous_result ? 0:out;++l->reverse_calls;
    return QcomRpcOkay;
}
int qcom_listener_poll(QcomRpcListener *l,uint64_t now,int up) {
    if (!l || !l->rpc) return QcomRpcArgument;
    if (l->error) return l->error;
    if (l->rpc->error) return fail(l,l->rpc->error);
    if (!up) {
        if (l->phase!=QcomListenerAttach || l->control_token) {qcom_rpc_lost(l->rpc,QcomRpcLost);return fail(l,QcomRpcLost);}
        return QcomRpcAgain;
    }
    if (l->control_token) {
        int32_t result=0;int r=qcom_rpc_take(l->rpc,l->control_token,&result);
        if (r<0) return fail(l,r);
        if (!r) {
            l->control_token=0;if (result) return fail(l,result);
            if (l->phase==QcomListenerOpen) {
                if (l->open_out[1] || !l->open_out[0] || l->open_out[0]==UINT32_MAX) return fail(l,QcomRpcWire);
                l->remote_handle=l->open_out[0];
            }
            ++l->phase;
        }
    }
    /* init2 completes before the first next2. That pending reverse wait must
     * coexist with module-open/register, which can itself need reverse RPC. */
    if (l->phase>=QcomListenerOpen) {
        if (l->listener_token) {
            int32_t result=0;int r=qcom_rpc_take(l->rpc,l->listener_token,&result);
            if (r<0) return fail(l,r);
            if (!r) {
                l->listener_token=0;if (result) return fail(l,result);
                r=dispatch(l);if (r) return fail(l,r);
            }
        }
        if (!l->listener_token) {int r=next(l,now);if (r<0) return fail(l,r);}
    }
    if (l->phase<QcomListenerRunning && !l->control_token) {
        int r=control(l,now);if (r<0) return fail(l,r);
    }
    return qcom_rpc_poll(l->rpc,now);
}
