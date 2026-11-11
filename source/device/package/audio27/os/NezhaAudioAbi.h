/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_ABI_H
#define NEZHA_AUDIO_ABI_H
#include <stdint.h>
#define NA_AUDIO_CLASS "NezhaAudio27"
#define NA_AUDIO_MAGIC 0x4e415532u
#define NA_AUDIO_ABI_VERSION 2u
#define NA_AUDIO_RATE 48000u
#define NA_AUDIO_RING_FRAMES 65536u
#define NA_AUDIO_PERIOD_FRAMES 480u
#define NA_AUDIO_LEAD_FRAMES 14400u
#define NA_AUDIO_RING_BYTES (NA_AUDIO_RING_FRAMES*8u)
#define NA_AUDIO_MAX_FRAME 0x7ffffffeULL
enum {NaAudioControlMemory=0,NaAudioOutputMemory=1,NaAudioInputMemory=2};
enum {NaAudioStartOutput=0,NaAudioStopOutput=1,NaAudioStartInput=2,NaAudioStopInput=3,
      NaAudioFixHostClock=4,NaAudioObserveHAL=5};
/* Bounded HAL observations use the existing owned user client. They carry
 * numeric callback results only and cannot request resources or memory writes. */
#define NA_HAL_OBSERVATION_VERSION 1u
#define NA_HAL_HISTORY_CAPACITY 64u
enum {NaHalConnected=1,NaHalDeviceList=2,NaHalPropertyError=3,
      NaHalStartIO=4,NaHalClockResult=5,NaHalPropertyRead=6,
      NaHalPropertyWrite=7,NaHalDeviceClient=8,NaHalPublication=9,NaHalSystemRoute=10};
typedef struct {
    uint32_t version,kind,object,selector,scope,element;
    int32_t result;
    uint32_t value;
} NaHalObservation;
/* StartInput is a500ms capture lease. HAL renews every100ms only while the
 * audio server actually requests ReadInput. Playing output alone never turns
 * the microphone on; a disappearing input client stops without a stuck ADC. */
enum {NaAudioOutputReady=1u,NaAudioInputReady=2u,NaAudioOutputRunning=4u,
      NaAudioInputRunning=8u,NaAudioFault=16u};
/* Derived from sheng's mapped rings, 48kHz timeline and independent direction
 * controls. ABI2 makes status/capture read-only to the client. Every PCM frame
 * is one atomic64 {consumed:1, frame+1:31, stereoS16:32}; capture uses the low S16.
 * Tags prevent stale audio when a ring wraps. Tag exhaustion stops a stream;
 * it must restart with a new epoch rather than alias 12.4h-old sample times. */
typedef struct {
    uint32_t magic,version,bytes,rate,ring_frames,period_frames,lead_frames;
    uint32_t sequence,state,epoch,out_channels,in_channels,host_hz,reserved;
    uint64_t ticks_per_frame_q32,anchor_host,out_consumed,in_produced;
    uint64_t output_periods,input_periods;
    uint32_t underruns,overruns,last_error,clock_result;
} NaAudioStatus;
/* The kernel never trusts mapped status fields for DMA, resource decisions or
 * format sizes. A private worker snapshot owns all such state. */
#endif
