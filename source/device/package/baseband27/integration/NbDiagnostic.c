/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdint.h>
#ifdef NB_HOST_TEST
#include <stdio.h>
#else
#include <sys/systm.h>
#endif
#include <QcomUsbDiagnostic.h>
#include "../platform/NbCommon.h"
extern int nezha_bb_snapshot(uint64_t[16]);
static int snapshot(void *cookie,char *out,size_t capacity,size_t *used,unsigned *truncated)
{
    (void)cookie;uint64_t words[16];*used=0;*truncated=0;
    if(nezha_bb_snapshot(words))return QUD_BUSY;
    static const char *keys[]={"stage","nezhaBasebandStatus","mss_stage","state","rx","tx","services","dropped",
        "watchdog","events","ram_reads","ram_writes","file_reads","file_writes","remotes","rejected"};
    size_t at=0;
    for(unsigned i=0;i<16;i++){
        int n=i==1?snprintf(out+at,capacity-at,"%s\"%s\":%lld",i?",":"{",keys[i],(long long)(int64_t)words[i]):
            snprintf(out+at,capacity-at,"%s\"%s\":%llu",i?",":"{",keys[i],(unsigned long long)words[i]);
        if(n<0||(size_t)n>=capacity-at){*truncated=1;return QUD_FAILED;}at+=(size_t)n;
    }
    if(capacity-at<2){*truncated=1;return QUD_FAILED;}out[at++]='}';out[at]=0;*used=at;return QUD_OK;
}
/* GPU024 already uses provider 7. ID 8 is this package's read-only provider. */
static const struct qud_diag_provider provider={8,1,(1u<<QUD_SNAPSHOT)|(1u<<QUD_HISTORY),0,0,"nezha-mss",0,snapshot,0};
int nezha_bb_diag_register(void){return qcom_usb_diag_register(&provider)?NB_OK:NB_IO;}
