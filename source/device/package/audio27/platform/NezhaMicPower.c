/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaMicPower.h"
#include <string.h>
#include <limits.h>
enum {MicTcs=2};
/* Stock nezha CMDDB: L15B_E0, B1B_E0, L1G_E0. Both PMIC7 LDOs
 * support AUTO=5 as their highest DT-supported mode. BOB supports HPM=7.
 * Keep that shared BOB floor rather than lowering an inherited camera vote
 * to the microphone's standalone PASS=2. All three voltages are fixed by DT.
 * No release writes disable, a lower mode, voltage, or a sleep/wake request. */
static const uint32_t expected[3]={0x40b00,0x40000,0x42300};
static const uint32_t millivolts[3]={1800,3400,1200},mode[3]={5,7,5};
static int fail(NaMicPower *p,int r) {p->error=r<0 ? r:NA_IO;return p->error;}
int na_mic_power_init(NaMicPower *p,const uint8_t *db,uint32_t bytes,const NaMicPowerOps *ops) {
    if (!p || p->initialized || !db || bytes!=0x20000 || !ops || !ops->acquire || !ops->release || !ops->read || !ops->write) return NA_ARGUMENT;
    NaMicPower fresh={0};fresh.ops=*ops;
    const char *names[3]={"L15B_E0","B1B_E0","L1G_E0"};
    for (unsigned i=0;i<3;++i) {
        QcomRpmhResource r={0};
        if (qcom_cmd_db_find(db,bytes,names[i],&r) || r.address!=expected[i]) return NA_MISSING;
        fresh.address[i]=r.address; /* VRM aux is deliberately not an ARC table. */
    }
    uint32_t version,config;
    int r=ops->read(ops->context,0,&version);if (r) return r;
    r=ops->read(ops->context,0x0c,&config);if (r) return r;
    if (version!=0x40300 || config!=0x800c8104) return NA_MISSING;
    fresh.layout=qcom_rsc_layout(version);fresh.initialized=1;*p=fresh;return NA_OK;
}
int na_mic_power_request(NaMicPower *p,int on) {
    if (!p || !p->initialized || (on!=0 && on!=1)) return NA_ARGUMENT;
    if (p->error) return p->error;
    p->want=(uint32_t)on;return NA_OK;
}
static int read_tcs(NaMicPower *p,uint32_t offset,uint32_t *value) {
    int r=p->ops.read(p->ops.context,0xd00+offset,value);return r ? fail(p,r):NA_OK;
}
static int writes(NaMicPower *p,const QcomRpmhWrite *w,unsigned n) {
    for (unsigned i=0;i<n;++i) {
        int r=p->ops.write(p->ops.context,0xd00+w[i].offset,w[i].value);
        if (r) return fail(p,r);
    }
    return NA_OK;
}
int na_mic_power_poll(NaMicPower *p,uint64_t now) {
    if (!p || !p->initialized || now>UINT64_MAX-100) return NA_ARGUMENT;
    if (p->error) return p->error;
    const uint32_t b=MicTcs*p->layout.tcs_stride;
    if (p->busy) {
        if (read_tcs(p,p->layout.irq_status,&p->last_irq) || read_tcs(p,b+p->layout.status,&p->last_status)) return p->error;
        for (unsigned i=0;i<3;++i)
            if (read_tcs(p,b+p->layout.cmd_data+4+i*p->layout.cmd_stride,&p->last_ack[i])) return p->error;
        int complete=(p->last_irq&(1u<<MicTcs)) && p->last_status==1;
        for (unsigned i=0;i<3;++i) complete&=(p->last_ack[i]&0x10100u)==0x10100u;
        if (!complete) return now>=p->deadline ? fail(p,NA_TIMEOUT):NA_PENDING;
        QcomRpmhWrite finish[4];unsigned n=qcom_rpmh_finish(&p->layout,MicTcs,finish,4);
        if (!n) return fail(p,NA_WIRE);
        if (writes(p,finish,n)) return p->error;
        QcomRpmhWrite wait={b+p->layout.wait_for_cmpl,0};
        if (writes(p,&wait,1)) return p->error;
        int r=p->ops.release(p->ops.context);if (r) return fail(p,r);
        p->held|=1u<<p->rail;p->busy=0;p->deadline=0;
    }
    if (!p->want || p->held==7) return NA_OK;
    int r=p->ops.acquire(p->ops.context);
    if (r==NA_BUSY) return NA_PENDING;
    if (r) return fail(p,r);
    unsigned rail=0;while (p->held&(1u<<rail)) ++rail;
    uint32_t addr[3]={p->address[rail],p->address[rail]+8,p->address[rail]+4};
    uint32_t value[3]={millivolts[rail],mode[rail],1};
    QcomRpmhWrite w[15];unsigned n=qcom_rpmh_active_request(&p->layout,MicTcs,addr,value,3,w,15);
    if (!n) return fail(p,NA_WIRE);
    /* A failed write can already have reached the controller. Preserve its
     * potential floor and the owner's lease; never replay that transaction. */
    p->possible|=1u<<rail;p->rail=rail;p->busy=1;p->deadline=now+100;
    if (writes(p,w,n)) return p->error;
    return NA_PENDING;
}
