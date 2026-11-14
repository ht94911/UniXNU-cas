// SPDX-License-Identifier: BSD-2-Clause
// One explicitly selected two-second recording through the system audio API.
#import <AVFAudio/AVFAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "QcomUserLogTransport.h"

static constexpr uint32_t rate = 48000, limit = 2 * rate;
static int16_t pcm[limit];
static uint32_t frames, callbacks, peak, nonzero;
static uint64_t squares;
static uint64_t queue_start_ms, queue_stop_ms;
static OSStatus callback_error;
static AudioQueueRef queue;
static bool collecting;

static uint64_t uptime_ms(void)
{
    struct timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void report(const char *event, int value)
{
    char text[480];
    snprintf(text, sizeof text,
        "AU90 event=%s value=%d uptime_ms=%llu frames=%u callbacks=%u "
        "peak=%u nonzero=%u squares=%llu callback_error=%d start_ms=%llu stop_ms=%llu",
        event, value, (unsigned long long)uptime_ms(), frames, callbacks,
        peak, nonzero, (unsigned long long)squares, (int)callback_error,
        (unsigned long long)queue_start_ms, (unsigned long long)queue_stop_ms);
    os_log(OS_LOG_DEFAULT, "%{public}s", text);
    (void)QcomUserLog::status(text);
}

static void hard_timeout(int signal_number)
{
    (void)signal_number;
    static const char text[] = "AU90 helper deadline\n";
    (void)write(STDERR_FILENO, text, sizeof text - 1);
    _exit(124);
}

static bool selected_payload(void)
{
    io_registry_entry_t chosen = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/chosen");
    if (!chosen) return false;
    CFTypeRef value = IORegistryEntryCreateCFProperty(chosen, CFSTR("nezha-audio-record"),
                                                     kCFAllocatorDefault, 0);
    IOObjectRelease(chosen);
    bool selected = false;
    if (value && CFGetTypeID(value) == CFDataGetTypeID()
        && CFDataGetLength((CFDataRef)value) == 4) {
        const UInt8 *bytes = CFDataGetBytePtr((CFDataRef)value);
        selected = bytes[0] == 90 && !bytes[1] && !bytes[2] && !bytes[3];
    }
    if (value) CFRelease(value);
    return selected;
}

// The callback runs on the same CFRunLoop as main. It never sends logs or
// writes a file; stop/dispose complete before the retained PCM is exported.
static void received(void *, AudioQueueRef aq, AudioQueueBufferRef buffer,
                     const AudioTimeStamp *, UInt32, const AudioStreamPacketDescription *)
{
    if (!collecting) return;
    ++callbacks;
    if (buffer->mAudioDataByteSize & 1) { callback_error = kAudio_ParamError; return; }
    const int16_t *samples = (const int16_t *)buffer->mAudioData;
    uint32_t take = buffer->mAudioDataByteSize / 2;
    if (take > limit - frames) take = limit - frames;
    for (uint32_t i = 0; i < take; ++i) {
        int32_t value = samples[i];
        uint32_t absolute = value < 0 ? (uint32_t)-value : (uint32_t)value;
        pcm[frames++] = (int16_t)value;
        if (value) ++nonzero;
        if (absolute > peak) peak = absolute;
        squares += (uint64_t)(value * value);
    }
    if (frames < limit) callback_error = AudioQueueEnqueueBuffer(aq, buffer, 0, nullptr);
}

static bool wave_snapshot(void)
{
    static const char path[] = "/private/var/ngpu/audio-record090.wav.gz";
    // Standard mono S16LE RIFF. The helper runs only on the arm64e LE target.
    uint32_t header[11] = {
        0x46464952, 36 + frames * 2, 0x45564157, 0x20746d66, 16,
        0x00010001, rate, rate * 2, 0x00100002, 0x61746164, frames * 2
    };
    gzFile file = gzopen(path, "wb1");
    if (!file) return false;
    int h = gzwrite(file, header, sizeof header);
    int n = gzwrite(file, pcm, frames * 2);
    int closed = gzclose(file);
    if (h != (int)sizeof header || n != (int)(frames * 2) || closed != Z_OK) return false;
    bool sent = QcomUserLog::snapshot(path, 90);
    if (sent) unlink(path);
    return sent;
}

int main(void)
{
    if (geteuid() != 0 || !selected_payload()) return 0;
    signal(SIGALRM, hard_timeout);
    alarm(260);
    // Fixed uptime lets the USB observer schedule the human-confirmed local
    // source before this request; no Wi-Fi or app interaction is needed.
    report("selected_wait_until_150s", 0);
    while (uptime_ms() < 150000) usleep(50000);
    alarm(60);
    @autoreleasepool {
        AVAudioSession *session = [AVAudioSession sharedInstance];
        NSError *error = nil;
        if (![session setCategory:AVAudioSessionCategoryRecord mode:AVAudioSessionModeMeasurement
                         options:0 error:&error]) {
            report("session_category_failed", (int)error.code); return 2;
        }
        if (![session setPreferredSampleRate:rate error:&error]
            || ![session setPreferredIOBufferDuration:0.010 error:&error]) {
            report("session_preferences_failed", (int)error.code); return 3;
        }
        if (![session setActive:YES error:&error]) {
            report("session_activation_failed", (int)error.code); return 4;
        }
        report("session_input_available", session.inputAvailable ? 1 : 0);
        for (AVAudioSessionPortDescription *port in session.currentRoute.inputs) {
            char text[480];
            snprintf(text, sizeof text, "AU90 route type=%s uid=%s rate=%.0f",
                     port.portType.UTF8String, port.UID.UTF8String, session.sampleRate);
            os_log(OS_LOG_DEFAULT, "%{public}s", text);
            (void)QcomUserLog::status(text);
        }
        AudioStreamBasicDescription format = {};
        format.mSampleRate = rate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        format.mBytesPerPacket = format.mBytesPerFrame = 2;
        format.mFramesPerPacket = format.mChannelsPerFrame = 1;
        format.mBitsPerChannel = 16;
        OSStatus status = AudioQueueNewInput(&format, received, nullptr, CFRunLoopGetCurrent(),
                                             kCFRunLoopDefaultMode, 0, &queue);
        int result = status ? 5 : 0;
        if (status) report("queue_create_failed", (int)status);
        if (!result) for (unsigned i = 0; i < 4; ++i) {
            AudioQueueBufferRef buffer = nullptr;
            status = AudioQueueAllocateBuffer(queue, 480 * 2, &buffer);
            if (!status) status = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
            if (status) { report("queue_buffer_failed", (int)status); result = 6; break; }
        }
        if (!result) {
            report("queue_start_requested", 0);
            collecting = true;
            queue_start_ms = uptime_ms();
            status = AudioQueueStart(queue, nullptr);
            if (status) { report("queue_start_failed", (int)status); result = 7; }
            else {
                // USB log delivery can wait on its FIFO. Defer it until Stop
                // so it cannot extend the selected recording window.
                uint64_t deadline = queue_start_ms + 2000;
                while (frames < limit && !callback_error && uptime_ms() < deadline)
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, true);
                if (callback_error || frames != limit) result = 8;
            }
        }
        collecting = false;
        if (queue) {
            queue_stop_ms = uptime_ms();
            status = AudioQueueStop(queue, true);
            report("queue_stopped", (int)status);
            if (status) result = 9;
            status = AudioQueueDispose(queue, true);
            report("queue_disposed", (int)status);
            if (status) result = 10;
            queue = nullptr;
        }
        if (![session setActive:NO error:&error]) {
            report("session_deactivation_failed", (int)error.code); result = 11;
        }
        report("capture_finished", result);
        if (frames && !wave_snapshot()) { report("sample_export_failed", 1); result = 12; }
        report("helper_finished", result);
        alarm(0);
        return result;
    }
}
