/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Existing real runtime/driver/engine/graph fixture, with the acquired private
 * TAS tables and original DSP route parameter encoding. No phone access. */
#define main original_idle_main
#include "idle_restart_test.c"
#undef main
#include "../platform/AudioPlaybackInputs.h"

static uint8_t registers[2][0x40000];
static uint32_t route_changes,mode_requests,skip_mode_reply,mode_error;
static int route_step(void) {
    int result=step();
    if (result) fprintf(stderr,"poll=%llu result=%d TAS=%u:%u/%u error=%d faults=%x/%x graph=%u err=%d VI=%u err=%d route=%u:%u/%u\n",
        (unsigned long long)f.now,result,f.speaker.phase,f.speaker.index,f.speaker.count[f.speaker.phase],
        f.speaker.error,f.speaker.reported_faults[0],f.speaker.reported_faults[1],f.driver.graph[0].stage,
        f.driver.graph[0].error,f.driver.graph[2].stage,f.driver.graph[2].error,
        f.driver.runtime.status.route_pending,f.driver.route_stage,f.driver.runtime.status.route);
    return result;
}
static void route_steps(unsigned n) {
    for (unsigned i=0;i<n;++i) {
        if (f.driver.runtime.request&2) assert(!na_runtime_request(&f.driver.runtime,NaAudioStartInput));
        assert(!route_step());
    }
}
static int route_pin(void *v,uint64_t lease,int speaker) {
    (void)v;assert(lease==42);
    assert(f.speaker.state==NaTasOff && !f.speaker.changing);
    ++route_changes;assert(speaker==0 || speaker==1);return NA_OK;
}
static int actual_write(void *v,uint64_t lease,uint8_t address,uint32_t reg,const uint8_t *data,uint32_t bytes) {
    assert(reg<sizeof registers[0] && bytes<=sizeof registers[0]-reg);
    memcpy(registers[address==0x48]+reg,data,bytes);
    return tas_write(v,lease,address,reg,data,bytes);
}
static int actual_read(void *v,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *out) {
    assert(reg<sizeof registers[0]);
    if (reg==0x78) return tas_read(v,lease,address,reg,out);
    const NaTasStep *steps=f.speaker.route==NaTasReceiver ? f.speaker.receiver_steps[1]:f.speaker.steps[1];
    if (f.speaker.phase==1 && steps[f.speaker.index].check) {
        NaTasBytes check=steps[f.speaker.index].bytes;
        for (uint32_t at=4;at<check.bytes;) {
            uint8_t op=check.data[at++];uint32_t expected_reg=0;memcpy(&expected_reg,check.data+at,4);at+=4;
            if (op==2) ++at;
            uint8_t expected=check.data[at++];
            if (expected_reg==reg) {*out=expected;return NA_OK;}
        }
    }
    *out=registers[address==0x48][reg];return NA_OK;
}
static int route_send(void *v,const uint8_t *packet,uint32_t bytes) {
    GprPacket p;assert(!gpr_decode(packet,bytes,&p));
    if (p.opcode==AR_OP_SET_CFG) {
        uint32_t offset=0;
        memcpy(&offset,p.payload,4);offset-=(uint32_t)f.driver.stream[0].dsp_base;
        if (offset==40000 || offset==40024) {
            ++mode_requests;
            assert(!na_spf_validate(dma[0]+offset,24));
            uint32_t param=0,value=0;
            memcpy(&param,dma[0]+offset+4,4);memcpy(&value,dma[0]+offset+16,4);
            assert(param==0x10010f0e && value==(offset==40000 ? 3u:0x13u));
            assert(f.driver.runtime.engine.output_idle && f.speaker.state==NaTasOff);
            if (skip_mode_reply) return NA_OK;
            if (mode_error) {
                uint8_t reply[GPR_MAX_PACKET],body[8];put(body,p.opcode);put(body+4,mode_error);
                GprPacket q={.src_domain=GPR_DOMAIN_ADSP,.dst_domain=GPR_DOMAIN_APPS,
                    .src_port=p.dst_port,.dst_port=p.src_port,.token=p.token,.opcode=GPR_OP_BASIC_RESULT};
                uint32_t n=gpr_encode(reply,sizeof reply,&q,body,sizeof body);assert(n);
                assert(!na_rx_push(&f.rx,f.driver.session.generation,reply,n));return NA_OK;
            }
        }
    }
    return peer_send(v,packet,bytes);
}
static int runtime_route(void *v,uint32_t target) {return route(&((Fixture *)v)->driver,target);}
static void routes_init(const NaPlaybackInputs *inputs,int input) {
    initialize_capture(input);memset(registers,0,sizeof registers);
    route_changes=mode_requests=skip_mode_reply=mode_error=0;
    NaTasPowerOps ops={.context=&f,.reset=tas_reset,.io={&f,actual_read,actual_write},.route=route_pin};
    /* The graph fixture already owns a prepared power lease. Its earlier
     * claim/reset/power behavior is covered by the real GENI board test. */
    /* Init validates presence but these callbacks must never be called. */
    extern int receiver_test_claim(void *,uint64_t *);
    extern int receiver_test_power(void *,uint64_t,int);
    extern int receiver_test_release(void *,uint64_t);
    ops.claim=receiver_test_claim;ops.power=receiver_test_power;ops.release=receiver_test_release;
    assert(!na_tas_endpoint_init(&f.speaker,&inputs->tuning,&inputs->selection,&ops));
    assert(!na_tas_endpoint_receiver(&f.speaker,&inputs->tuning));
    f.speaker.lease=42;f.speaker.powered=f.speaker.identified=1;
    f.driver.runtime.ops.route=runtime_route;f.driver.runtime.status.route_ready=1;
    if (input) {f.driver.runtime.capture_proven=1;f.driver.runtime.status.in_channels=1;}
    const char *profile[2]={"music_18dB_stereo","voice"};
    for (unsigned i=0;i<2;++i) {
        assert(!na_ti_route_payload(&inputs->calibration,&inputs->tuning,42,profile[i],dma[0]+40000+24*i));
        f.driver.route_config[i]=(NaBlob){40000+24*i,24};
    }
    for (unsigned i=0;i<3;++i) if (f.driver.runtime.engine.graph[i]) f.driver.graph[i].ops.send=route_send;
}
int receiver_test_claim(void *v,uint64_t *lease) {(void)v;(void)lease;abort();}
int receiver_test_power(void *v,uint64_t lease,int on) {(void)v;(void)lease;(void)on;abort();}
int receiver_test_release(void *v,uint64_t lease) {(void)v;(void)lease;abort();}
static void select_and_wait(unsigned target) {
    NaRuntime *r=&f.driver.runtime;
    assert(!na_runtime_request(r,target==NaAudioReceiver ? NaAudioRouteReceiver:NaAudioRouteSpeaker));
    if (r->request&2) assert(!na_runtime_request(r,NaAudioStartInput));
    assert(!route_step());
    for (unsigned i=0;i<1200 && r->status.route_pending;++i) {
        if (r->request&2) assert(!na_runtime_request(r,NaAudioStartInput));
        assert(!route_step());
    }
    assert(!r->status.route_pending && r->status.route==target && !r->fault);
    assert(f.speaker.route==(target==NaAudioReceiver ? NaTasReceiver:NaTasSpeaker));
    assert(mode_requests==route_changes);
    NaAudioStatus mapped;assert(!na_audio_status_snapshot(&f.status,&mapped));
    assert(mapped.route==r->status.route && !mapped.route_pending);
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);
    fseek(file,0,SEEK_END);long bytes=ftell(file);rewind(file);
    assert(bytes>0 && bytes<=0x8000000);void *blob=malloc((size_t)bytes);assert(blob);
    assert(fread(blob,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);
    NaInputPack pack;assert(!na_input_pack(&pack,blob,(uint64_t)bytes));
    NaTasSelection choice={.revision={0x51,0x52},.revision_mask={0xff,0xff},.dvc_index={55,55},
        .profile="music_18dB_stereo",.rx_format="32BIT",.tx_format="24_I2S_02_12_1"};
    NaPlaybackInputs inputs={0};assert(!na_playback_inputs(&inputs,&pack,&choice));
    routes_init(&inputs,1);assert(!na_runtime_request(&f.driver.runtime,NaAudioStartOutput));route_steps(300);
    assert(f.driver.runtime.engine.active[0]);
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStartInput));
    for (unsigned i=0;i<600 && !f.driver.runtime.status.in_produced;++i) route_steps(1);
    assert(f.driver.runtime.status.in_produced);
    uint32_t input_opens=f.input_opens,input_maps=f.input_maps;uint64_t produced=f.driver.runtime.status.in_produced;
    select_and_wait(NaAudioReceiver);route_steps(10);
    assert((registers[1][2]&3)==2 && (registers[0][2]&3)==0);
    assert(f.driver.runtime.status.in_produced>produced && f.input_opens==input_opens && f.input_maps==input_maps);
    /* Mono downmix is actually submitted in the retained stereo TDM slot. */
    NaRuntime *runtime=&f.driver.runtime;
    uint64_t frame=runtime->origin[0]+f.driver.stream[0].next*NA_AUDIO_PERIOD_FRAMES-runtime->relative_origin[0];
    for (unsigned i=0;i<NA_DMA_FLIGHT*NA_AUDIO_PERIOD_FRAMES;++i)
        assert(!na_ring_mix(runtime->output,frame+i,12000,8000));
    route_steps(1);
    uint32_t slot=(uint32_t)(f.driver.stream[0].next-1)%NA_DMA_SLOTS;
    const int16_t *pcm=(const int16_t *)(dma[0]+slot*4096);
    for (unsigned i=0;i<NA_AUDIO_PERIOD_FRAMES;++i) assert(pcm[2*i]==10000 && !pcm[2*i+1]);
    select_and_wait(NaAudioSpeaker);route_steps(10);
    assert((registers[1][2]&3)==0 && (registers[0][2]&3)==0);
    select_and_wait(NaAudioReceiver);select_and_wait(NaAudioSpeaker);
    assert(mode_requests==4 && route_changes==4 && f.input_opens==input_opens && f.input_maps==input_maps);
    /* First receiver+duplex uses061's muted output for the first READ, then
     * the actual capture completion unlocks the receiver amplifier. */
    routes_init(&inputs,1);
    assert(!na_runtime_request(&f.driver.runtime,NaAudioRouteReceiver));
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStartOutput));
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStartInput));
    for (unsigned i=0;i<1200 && (!f.driver.runtime.status.in_produced || f.driver.runtime.status.route_pending);++i) {
        if (!f.driver.runtime.status.in_produced) assert(f.speaker.state!=NaTasActive);
        route_steps(1);
    }
    assert(f.driver.runtime.status.in_produced && f.driver.runtime.status.route==NaAudioReceiver &&
        !f.driver.runtime.status.route_pending && !f.driver.runtime.fault);
    /* The066 first no-PCM close/unmap/reopen also runs under a pending
     * receiver change. The physical output stays muted through recovery. */
    routes_init(&inputs,1);f.suppress_reads=1;
    assert(!na_runtime_request(&f.driver.runtime,NaAudioRouteReceiver));
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStartOutput));
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStartInput));
    for (unsigned i=0;i<4200 && !f.driver.runtime.input_recovery_attempted;++i) {
        assert(!f.driver.runtime.status.in_produced && f.speaker.state!=NaTasActive);
        route_steps(1);
    }
    assert(f.driver.runtime.input_recovery_attempted && f.input_closes==1 && f.input_unmaps==1);
    assert(f.driver.runtime.input_first_error==NA_TIMEOUT);
    f.suppress_reads=0;
    for (unsigned i=0;i<1600 && (!f.driver.runtime.status.in_produced || f.driver.runtime.status.route_pending);++i) {
        if (!f.driver.runtime.status.in_produced) assert(f.speaker.state!=NaTasActive);
        route_steps(1);
    }
    assert(f.driver.runtime.status.in_produced && f.driver.runtime.status.route==NaAudioReceiver &&
        !f.driver.runtime.status.route_pending && !f.driver.runtime.fault);
    assert(f.input_opens==2 && f.input_maps==2 && f.driver.runtime.input_first_error==NA_TIMEOUT);
    /* No output request still permits a cold route setup without opening mic. */
    routes_init(&inputs,0);select_and_wait(NaAudioReceiver);route_steps(30);
    assert(!f.driver.runtime.request && !f.driver.runtime.engine.active[0] && !f.reads);
    /* Missing/failed DSP ACK never publishes the selected route or GPIO. */
    routes_init(&inputs,0);skip_mode_reply=1;
    assert(!na_runtime_request(&f.driver.runtime,NaAudioRouteReceiver));
    int result=0;
    for (unsigned i=0;i<2600 && !result;++i) result=route_step();
    assert(result==NA_TIMEOUT && !route_changes && f.driver.runtime.status.route==NaAudioSpeaker);
    routes_init(&inputs,0);mode_error=4;
    assert(!na_runtime_request(&f.driver.runtime,NaAudioRouteReceiver));result=0;
    for (unsigned i=0;i<600 && !result;++i) result=route_step();
    assert(result==NA_REMOTE && !route_changes && f.driver.runtime.status.route==NaAudioSpeaker);
    free(blob);puts("PASS: real OEM voice/stereo programs, DSP mode ACK, top-only output, continuous mic, cold selection and timeout/error without false route");
}
