/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual native inline helper; secure monitor is an explicit fake provider. */
#include "../os/NezhaAudioNative.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t calls,command[4],pas[4];static int query_error;
static unsigned proxy_calls,stop_calls,dma_calls;static int stop_error;
int na_native_pas_query(uint32_t c,uint32_t p,uint64_t result[4]) {
    assert(calls<4);command[calls]=c;pas[calls]=p;++calls;
    result[0]=query_error ? 0xffcfffb8:0;
    result[1]=c==7 ? 1:0x8aa00000;result[2]=c==9 ? 0xa8c80000:0;
    return query_error;
}
static int proxy(void *v,int on) {
    (void)v;assert(on && !calls && !stop_calls);++proxy_calls;return NA_OK;
}
static int stop(void *v,uint32_t c,uint32_t p,uint64_t x,uint64_t y) {
    (void)v;(void)x;(void)y;assert(c==6 && p==(stop_calls ? 36u:1u));++stop_calls;
    /* This synthetic monitor models the038 observed rejection. Its query
     * prerequisite is a test assumption, not a proven hardware requirement. */
    return calls==4 ? stop_error:NA_IO;
}
static int dma(void *v) {
    (void)v;assert(calls==4 && stop_calls==2 && !stop_error);++dma_calls;return NA_OK;
}
static void lifecycle(int queries_fail,int stops_fail,int omit_queries) {
    NaAdsp a={0};NaPasLoad main_image={0},dtb_image={0};uint32_t completed=0;
    calls=proxy_calls=stop_calls=dma_calls=0;
    query_error=queries_fail ? NA_IO:NA_OK;stop_error=stops_fail ? NA_IO:NA_OK;
    a.stage=NaAdspPower;a.main_image=&main_image;a.dtb_image=&dtb_image;
    main_image.ops.pas=dtb_image.ops.pas=stop;a.ops.proxy=proxy;a.ops.dma_setup=dma;
    assert(na_adsp_poll(&a,1)==NA_PENDING && a.stage==NaAdspStopMain);
    assert(proxy_calls==1 && !calls && !stop_calls && !dma_calls);
    if (!omit_queries) na_native_pil_preflight(&a,&completed);
    assert(!a.masters_stopped && !a.dma_ready);
    assert(na_adsp_poll(&a,2)==NA_PENDING && a.stage==NaAdspStopDtb && stop_calls==1);
    na_native_pil_preflight(&a,&completed);
    int r=na_adsp_poll(&a,3);assert(stop_calls==2 && !dma_calls);
    if (stops_fail || omit_queries) {
        assert(r==NA_IO && a.stage==NaAdspFailed && !a.masters_stopped && !a.dma_ready);
        assert(na_adsp_poll(&a,4)==NA_IO && stop_calls==2 && !dma_calls);
    } else {
        assert(r==NA_PENDING && a.stage==NaAdspDma && a.masters_stopped && !a.dma_ready);
        assert(na_adsp_poll(&a,4)==NA_PENDING && a.stage==NaAdspSetup && a.dma_ready && dma_calls==1);
    }
    assert(calls==(omit_queries ? 0u:4u));
}
int main(void) {
    const uint32_t want_command[4]={7,9,7,9},want_pas[4]={1,1,36,36};
    for (unsigned bad=0;bad<2;++bad) {
        NaAdsp a={0};uint32_t completed=0;calls=0;query_error=bad ? NA_IO:0;
        a.stage=NaAdspPower;na_native_pil_preflight(&a,&completed);
        assert(!calls && !completed); /* No query before proxy completion. */
        a.stage=NaAdspStopMain;na_native_pil_preflight(&a,&completed);
        assert(calls==4 && completed==15);
        for (unsigned i=0;i<4;++i) assert(command[i]==want_command[i] && pas[i]==want_pas[i]);
        assert(a.stage==NaAdspStopMain && !a.masters_stopped && !a.dma_ready && !a.initial_stop_mask);
        na_native_pil_preflight(&a,&completed);assert(calls==4);
        a.stage=NaAdspStopDtb;na_native_pil_preflight(&a,&completed);assert(calls==4);
        a.stage=NaAdspFailed;completed=0;na_native_pil_preflight(&a,&completed);assert(calls==4);
        a.stage=NaAdspStopMain;completed=1;na_native_pil_preflight(&a,&completed);assert(calls==4);
    }
    for (unsigned q=0;q<2;++q) for (unsigned s=0;s<2;++s) lifecycle(q,s,0);
    lifecycle(0,0,1);
    puts("PASS: production PIL helper and ADSP lifecycle, exact037 query order after proxy and before PAS stops, once per boot, failed queries never manufacture stopped/DMA state, failed stops retain DMA gate, omitted-query negative control; SCM replies are synthetic");
}
