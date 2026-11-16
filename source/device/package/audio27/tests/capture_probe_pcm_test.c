/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Executes the production capture/probe functions with private CPU buffers.
 * Shared transport, clocks and hardware are not exercised in this fixture. */
#include "../core/AudioRuntime.c"
#include <assert.h>
#include <stdio.h>
static NaRuntime runtime;
static NaPrm prm;
static NaGraph output,input;
static NaStream streams[2];
static uint64_t input_ring[NA_AUDIO_RING_FRAMES];
static int16_t samples[NA_AUDIO_PERIOD_FRAMES];
static void initialize(void) {
    memset(&runtime,0,sizeof runtime);memset(&prm,0,sizeof prm);
    memset(&output,0,sizeof output);memset(&input,0,sizeof input);
    memset(streams,0,sizeof streams);output.stream=&streams[0];input.stream=&streams[1];
    runtime.prm=&prm;runtime.engine.graph[0]=&output;runtime.engine.graph[1]=&input;
    runtime.input=input_ring;runtime.votes[1]=2;runtime.capture_probe_allowed=1;runtime.service_ready=1;
    for (unsigned i=0;i<NA_AUDIO_PERIOD_FRAMES;++i) samples[i]=(int16_t)((int)i*113-27000);
    samples[0]=-32768;
}
int main(void) {
    initialize();assert(na_runtime_probe_capture(&runtime,0,100)==NA_ARGUMENT);
    assert(na_runtime_probe_capture(&runtime,2*NA_AUDIO_RATE+1,100)==NA_ARGUMENT);
    runtime.capture_probe_allowed=0;assert(na_runtime_probe_capture(&runtime,480,100)==NA_MISSING);
    runtime.capture_probe_allowed=1;input.stage=NaGraphMap;
    assert(na_runtime_probe_capture(&runtime,480,100)==NA_BUSY);input.stage=NaGraphOff;
    assert(!na_runtime_probe_capture(&runtime,480,100));
    assert(na_runtime_request(&runtime,NaAudioStartInput)==NA_BUSY);
    assert(!capture(&runtime,0,samples,2*239));
    assert(runtime.probe.state==NaProbeCollecting && runtime.probe.frames==239 && runtime.probe.pcm_frames==239);
    assert(!capture(&runtime,239,samples+239,2*241));
    assert(runtime.probe.state==NaProbeDraining && runtime.probe.frames==480 && runtime.probe.pcm_frames==480);
    assert(!memcmp(runtime.probe.pcm,samples,sizeof samples) && runtime.probe.peak==32768 && runtime.probe.periods==2);
    uint64_t squares=0;for (unsigned i=0;i<480;++i) squares+=(uint64_t)((int32_t)samples[i]*samples[i]);
    assert(runtime.probe.squares==squares && !runtime.capture_proven && !runtime.status.in_channels);
    memset(samples,0,sizeof samples);assert(runtime.probe.pcm[0]==-32768);
    assert(!capture(&runtime,480,samples,sizeof samples) && runtime.probe.frames==480 && runtime.probe.pcm[0]==-32768);
    initialize();assert(!na_runtime_probe_capture(&runtime,13,100));
    assert(!capture(&runtime,0,samples,sizeof samples) && runtime.probe.frames==13 && runtime.probe.pcm_frames==13);
    assert(!memcmp(runtime.probe.pcm,samples,26));
    initialize();assert(!na_runtime_probe_capture(&runtime,960,100));
    assert(!capture(&runtime,0,samples,sizeof samples));
    int16_t first=runtime.probe.pcm[0];samples[0]=123;
    assert(!capture(&runtime,480,samples,sizeof samples) && runtime.probe.frames==960 && runtime.probe.pcm_frames==960);
    assert(runtime.probe.pcm[0]==first && runtime.probe.pcm[480]==123);
    initialize();memset(samples,0,sizeof samples);
    assert(!na_runtime_probe_capture(&runtime,2*NA_AUDIO_RATE,100));
    assert(!capture(&runtime,0,samples,sizeof samples)); /* real084 first period was silent */
    samples[0]=32767;samples[479]=-32768;
    for (unsigned i=1;i<200;++i) assert(!capture(&runtime,(uint64_t)i*480,samples,sizeof samples));
    assert(runtime.probe.frames==96000 && runtime.probe.pcm_frames==96000 && runtime.probe.periods==200);
    assert(!runtime.probe.pcm[0] && runtime.probe.pcm[480]==32767 && runtime.probe.pcm[95999]==-32768);
    assert(runtime.probe.state==NaProbeDraining && !runtime.capture_proven);
    memset(samples,0,sizeof samples);
    assert(!capture(&runtime,96000,samples,sizeof samples));
    assert(runtime.probe.pcm[95999]==-32768 && runtime.probe.frames==96000);
    initialize();assert(capture(&runtime,0,samples,sizeof samples)==NA_OWNERSHIP);
    /* System input has its own gain; the raw diagnostic probe above remains
     * exact. Signed extremes saturate without overflow, silence stays zero,
     * and frame tags keep the same recorded time. */
    initialize();runtime.capture_proven=1;runtime.origin_set[1]=1;runtime.origin[1]=1000;
    const int16_t speech[]={0,1,-1,1000,-1000,8191,-8192,8192,-8193,32767,-32768};
    const int16_t expected[]={0,4,-4,4000,-4000,32764,-32768,32767,-32768,32767,-32768};
    assert(!capture(&runtime,0,speech,sizeof speech));
    for (unsigned i=0;i<sizeof speech/sizeof speech[0];++i) {
        int16_t value=0;assert(!na_ring_read(input_ring,1000+i,&value) && value==expected[i]);
    }
    assert(runtime.status.in_produced==1011);
    assert(!runtime.probe.frames && !runtime.probe.nonzero && !runtime.probe.peak);
    int16_t saved=0;assert(!na_ring_read(input_ring,1003,&saved) && saved==4000);
    memset(samples,0,sizeof samples);assert(!capture(&runtime,11,samples,sizeof samples));
    assert(!na_ring_read(input_ring,1011,&saved) && saved==0);
    puts("capture PCM production fixture passed: exact raw probe, system speech gain, signed saturation, silence and frame continuity");
}
