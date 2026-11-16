/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tas25xxEndpoint.h"
#include <string.h>
#include <limits.h>
int na_tas_identity_init(NaTasIdentity *d,const NaTasPowerOps *ops) {
    if (!d || !ops || d->initialized || !ops->claim || !ops->power ||
        !ops->reset || !ops->release || !ops->io.read) return NA_ARGUMENT;
    memset(d,0,sizeof *d);d->ops=*ops;d->initialized=1;return NA_OK;
}
int na_tas_identity_stop(NaTasIdentity *d) {
    if (!d) return NA_ARGUMENT;
    if (!d->initialized || d->cleanup_attempted) return d->cleanup_error;
    d->cleanup_attempted=1;
    if (d->phase!=3) d->phase=4;
    if (d->claim_failed) {
        /* A partial claim has no complete GPIO/clock snapshot to restore. */
        if (d->lease) d->cleanup_error=NA_OWNERSHIP;
        return d->cleanup_error;
    }
    if (!d->claimed) return NA_OK;
    int r=d->ops.reset(d->ops.context,d->lease,1);
    if (r && !d->cleanup_error) d->cleanup_error=r;
    if (d->power_attempted) {
        r=d->ops.power(d->ops.context,d->lease,0);
        if (r && !d->cleanup_error) d->cleanup_error=r;
    }
    if (!d->cleanup_error) {
        r=d->ops.release(d->ops.context,d->lease);
        if (r) d->cleanup_error=r;
        else {d->lease=0;d->claimed=0;}
    }
    return d->cleanup_error;
}
static int identity_failed(NaTasIdentity *d,int error) {
    d->error=error<0 ? error:NA_IO;d->phase=4;
    (void)na_tas_identity_stop(d);return d->error;
}
int na_tas_identity_poll(NaTasIdentity *d,uint64_t now) {
    if (!d || !d->initialized || now>UINT64_MAX-5000) return NA_ARGUMENT;
    if (d->error) return d->error;
    if (d->phase==3) return NA_OK;
    if (d->phase==4 || d->cleanup_attempted) return NA_OWNERSHIP;
    if (!d->deadline) d->deadline=now+5000;
    if (now>=d->deadline) return identity_failed(d,NA_TIMEOUT);
    int r;
    if (!d->phase) {
        r=d->ops.claim(d->ops.context,&d->lease);
        if (r==NA_BUSY && !d->lease) return NA_PENDING;
        if (r || !d->lease) {d->claim_failed=1;return identity_failed(d,r ? r:NA_OWNERSHIP);}
        d->claimed=1;
        r=d->ops.reset(d->ops.context,d->lease,1);if (r) return identity_failed(d,r);
        d->power_attempted=1;
        r=d->ops.power(d->ops.context,d->lease,1);if (r) return identity_failed(d,r);
        d->wait_until=now+20;d->phase=1;return NA_PENDING;
    }
    if (now<d->wait_until) return NA_PENDING;
    if (d->phase==1) {
        r=d->ops.reset(d->ops.context,d->lease,0);if (r) return identity_failed(d,r);
        d->wait_until=now+20;d->phase=2;return NA_PENDING;
    }
    for (unsigned i=0;i<2;++i) {
        r=d->ops.io.read(d->ops.io.context,d->lease,
            (uint8_t)na_nezha_facts.amp_address[i],0x78,&d->revision[i]);
        if (r) return identity_failed(d,r);
    }
    r=na_tas_identity_stop(d);if (r) return identity_failed(d,r);
    d->phase=3;return NA_OK;
}
static int append(NaTasEndpoint *e,unsigned phase,unsigned ch,NaTasBytes b,int check) {
    NaTasProgramInfo info;
    if (phase>2 || ch>1 || e->count[phase]>=NA_TAS_STEPS || na_tas_validate(b,&info)) return NA_WIRE;
    e->steps[phase][e->count[phase]++]=(NaTasStep){b,(uint8_t)ch,(uint8_t)check};return NA_OK;
}
static int control(NaTasEndpoint *e,const NaTasImage *image,unsigned phase,unsigned ch,const char *name,const char *value) {
    NaTasControl c;NaTasBytes b;int r=na_tas_control(image,name,&c);
    if (r) return r;if (c.channel!=ch) return NA_WIRE;
    r=value ? na_tas_enum(&c,value,&b):na_tas_enum_index(&c,c.default_index,&b);
    return r ? r:append(e,phase,ch,b,0);
}
int na_tas_endpoint_init(NaTasEndpoint *e,const NaTasImage *image,const NaTasSelection *s,const NaTasPowerOps *ops) {
    if (!e || !image || !s || !ops || !ops->claim || !ops->power || !ops->reset || !ops->release || !ops->io.read || !ops->io.write ||
        !s->calibrated || !s->profile || !s->rx_format || !s->tx_format || !s->revision_mask[0] || !s->revision_mask[1]) return NA_MISSING;
    memset(e,0,sizeof *e);e->ops=*ops;e->selected=*s;
    const uint32_t starts[8]={0,3,7,11,14,17,20,23},sizes[8]={3,4,4,3,3,3,3,3};
    const char *names[8][2]={{"NOISE_GATE_CTRL_TOP","NOISE_GATE_CTRL_BOTTOM"},{"BOOST_MODE_CTRL_TOP","BOOST_MODE_CTRL_BOTTOM"},
        {"ASI_SWITCH_TOP","ASI_SWITCH_BOTTOM"},{"POWER_ACTIVE_TOP","POWER_ACTIVE_BOTTOM"},{"CALIB_INIT_TOP","CALIB_INIT_BOTTOM"},
        {"DVC_GAIN_TOP","DVC_GAIN_BOTTOM"},{"SDA_DRV_STR_TOP","SDA_DRV_STR_BOTTOM"},
        {"SDO_DRV_STR_TOP","SDO_DRV_STR_BOTTOM"}};
    for (unsigned ch=0;ch<2;++ch) {
        NaTasProfile p;NaTasBytes b;NaTasControl gain;
        int r=na_tas_profile(image,ch,s->profile,&p);if (r) return r;
        if ((r=na_tas_fault_table(image,ch,&e->faults[ch]))) return r;
        /* Original default hardware layout, then device initialization. */
        for (unsigned i=0;i<8;++i) {
            if (image->default_hw[i]>=sizes[i]) return NA_WIRE;
            if ((r=na_tas_hardware(image,ch,starts[i]+image->default_hw[i],&b)) || (r=append(e,0,ch,b,0))) return r;
        }
        if ((r=append(e,0,ch,image->section[ch][NaTasInit],0))) return r;
        /* Override only the interface fields tied to the selected topology:
         * 48kHz, I2S/NB_NF, exact RX/TX labels. No near-match fallback. */
        const unsigned hw[3]={0,3,7};
        for (unsigned i=0;i<3;++i) if ((r=na_tas_hardware(image,ch,hw[i],&b)) || (r=append(e,0,ch,b,0))) return r;
        if ((r=na_tas_format(image,ch,0,s->rx_format,&b)) || (r=append(e,0,ch,b,0)) ||
            (r=na_tas_format(image,ch,1,s->tx_format,&b)) || (r=append(e,0,ch,b,0))) return r;
        for (unsigned i=0;i<8;++i) if (i!=5) {
            const char *value=i==0 ? "ENABLE":i==2 || i==3 ? "OFF":i==4 ? "DISABLE":0;
            if ((r=control(e,image,0,ch,names[i][ch],value))) return r;
        }
        if ((r=na_tas_control(image,names[5][ch],&gain))) return r;
        if (gain.channel!=ch || gain.reg!=0x10c || gain.reg_type!=1 || s->dvc_index[ch]>gain.default_index) return NA_MISSING;
        if ((r=na_tas_integer(&gain,s->dvc_index[ch],e->integers[ch],&b)) || (r=append(e,0,ch,b,0))) return r;
        if ((r=na_tas_block(image,ch,"MUTE0",&b)) || (r=append(e,0,ch,b,0))) return r;
        if ((r=append(e,1,ch,e->faults[ch].clear,0)) || (r=append(e,1,ch,p.phase[0],0)) ||
            (r=control(e,image,1,ch,names[2][ch],"ON")) || (r=control(e,image,1,ch,names[3][ch],"ON")) ||
            (r=append(e,1,ch,p.phase[1],0))) return r;
        if ((r=na_tas_integer(&gain,s->dvc_index[ch],e->integers[ch],&b)) || (r=append(e,1,ch,b,0)) ||
            (r=na_tas_block(image,ch,"PWRCK",&b)) || (r=append(e,1,ch,b,1)) ||
            (r=append(e,1,ch,e->faults[ch].enable,0))) return r;
        if ((r=append(e,2,ch,e->faults[ch].disable,0)) || (r=append(e,2,ch,p.phase[2],0)) ||
            (r=append(e,2,ch,p.phase[3],0)) || (r=control(e,image,2,ch,names[2][ch],"OFF")) ||
            (r=control(e,image,2,ch,names[3][ch],"OFF"))) return r;
    }
    return NA_OK;
}
static int emergency(NaTasEndpoint *e,int error) {
    if (!e->error) e->error=error;
    na_tas_abort(&e->run);e->changing=0;e->target=NaTasOff;
    if (e->lease && !e->claim_failed && !e->asserted) {
        int r=e->ops.reset(e->ops.context,e->lease,1);
        if (r) e->cleanup_error=r;else e->asserted=1;
    }
    /* Reset cuts speaker drive even if I2C timed out. Do not release clocks or
     * pins while the graph might still own a running data interface. */
    return error;
}
int na_tas_endpoint_request(NaTasEndpoint *e,uint32_t target) {
    if (!e || target>NaTasActive) return NA_ARGUMENT;
    if (e->error && target!=NaTasOff) return e->error;
    if (target==NaTasOff && e->changing && e->target!=NaTasOff) {na_tas_abort(&e->run);e->changing=0;}
    else if (e->changing && target!=e->target) return NA_BUSY;
    e->target=target;return NA_OK;
}
int na_tas_endpoint_poll(NaTasEndpoint *e,uint64_t now) {
    if (!e || now>UINT64_MAX-5000) return NA_ARGUMENT;
    if (e->error) return emergency(e,e->error);
    uint64_t trace_wait=e->ops.trace_wait_us ? *e->ops.trace_wait_us:0;
    if (!e->changing && e->state==e->target) {
        if (e->state==NaTasActive && now>=e->next_health) {
            for (unsigned ch=0;ch<2;++ch) {
                uint32_t fatal=0;int r=na_tas_faults(&e->faults[ch],&e->ops.io,e->lease,ch,1,&e->reported_faults[ch]);
                if (!r) r=na_tas_fault_severity(&e->faults[ch],ch,e->reported_faults[ch],&fatal);
                if (r || fatal) return emergency(e,r ? r:NA_IO);
            }
            e->next_health=now+100;
        }
        return NA_OK;
    }
    if (!e->changing) {
        if (e->state==NaTasOff && e->target==NaTasActive) return NA_OWNERSHIP;
        e->phase=e->target==NaTasOff || (e->state==NaTasActive && e->target==NaTasPrepared) ? 2:e->target==NaTasActive ? 1:0;
        e->changing=1;e->index=0;e->deadline=now+5000;e->trace_base_us=trace_wait;memset(&e->run,0,sizeof e->run);
        if (!e->lease) {
            int r=e->ops.claim(e->ops.context,&e->lease);
            if (r || !e->lease) {e->claim_failed=1;return emergency(e,r ? r:NA_OWNERSHIP);}
        }
        if (!e->powered && e->target!=NaTasOff) {
            int r=e->ops.reset(e->ops.context,e->lease,1);if (r) return emergency(e,r);e->asserted=1;
            r=e->ops.power(e->ops.context,e->lease,1);if (r) return emergency(e,r);e->powered=1;
            /* Match the real driver's 20ms reset-low / reset-high settles. */
            e->wait_until=now+20;return NA_PENDING;
        }
    }
    if (trace_wait<e->trace_base_us) return emergency(e,NA_IO);
    /* Keep the original five-second hardware/scheduler budget. Each fixed
     * BEFORE record already has its own bounded confirmation wait. Counting
     * that CPU-only wait again prematurely aborts a progressing TAS program.
     * Reset settles and GENI transaction timeouts continue to use real time. */
    uint64_t paused_ms=(trace_wait-e->trace_base_us)/1000;
    if (now>=e->deadline && now-e->deadline>=paused_ms) return emergency(e,NA_TIMEOUT);
    if (now<e->wait_until) return NA_PENDING;
    if (e->asserted && e->target!=NaTasOff) {
        int r=e->ops.reset(e->ops.context,e->lease,0);if (r) return emergency(e,r);
        e->asserted=0;e->wait_until=now+20;return NA_PENDING;
    }
    if (!e->identified && e->target!=NaTasOff) {
        for (unsigned ch=0;ch<2;++ch) {
            int r=e->ops.io.read(e->ops.io.context,e->lease,(uint8_t)na_nezha_facts.amp_address[ch],0x78,&e->observed_revision[ch]);
            if (r) return emergency(e,r);
            if ((e->observed_revision[ch]&e->selected.revision_mask[ch])!=(e->selected.revision[ch]&e->selected.revision_mask[ch])) return emergency(e,NA_MISSING);
        }
        e->identified=1;
    }
    if (e->index==e->count[e->phase]) {e->state=e->target;e->changing=0;return NA_OK;}
    NaTasStep *step=&e->steps[e->phase][e->index];int r;
    if (step->check) r=na_tas_power_check(step->bytes,&e->ops.io,e->lease,step->channel);
    else {
        if (!e->run.program.data) {
            r=na_tas_begin(&e->run,step->bytes,&e->ops.io,e->lease,step->channel);
            if (r) return emergency(e,r);
        }
        r=na_tas_poll(&e->run,now);
    }
    if (r==NA_PENDING) return r;
    if (r) return emergency(e,r);
    memset(&e->run,0,sizeof e->run);++e->index;return NA_PENDING;
}
int na_tas_endpoint_release(NaTasEndpoint *e,int quiescent) {
    if (!e) return NA_ARGUMENT;
    if (!quiescent) return NA_OWNERSHIP;
    if (e->claim_failed) return NA_OWNERSHIP;
    if (!e->lease) return NA_OK;
    if (!e->error && (e->state!=NaTasOff || e->changing)) return NA_PENDING;
    int r=e->ops.reset(e->ops.context,e->lease,1);
    if (r) {e->cleanup_error=r;return r;}e->asserted=1;
    r=e->ops.power(e->ops.context,e->lease,0);
    if (r) {e->cleanup_error=r;return r;}e->powered=0;
    r=e->ops.release(e->ops.context,e->lease);
    if (r) {e->cleanup_error=r;return r;}
    e->lease=0;e->identified=0;e->state=NaTasOff;return NA_OK;
}
