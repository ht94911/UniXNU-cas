// SPDX-License-Identifier: BSD-2-Clause-Patent
// CoreHaptics API path adapted from MarbleHapticsProbe.m at7a0d68fff255.
// Finite diagnostic process, run only in the qualified system candidate.
#import <Foundation/Foundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>

static NSMutableArray *records;
static void note(NSString *phase, BOOL success, NSError *error) {
    NSDictionary *row=@{@"phase":phase,@"success":@(success),@"error":@(error.code),
        @"description":error.localizedDescription ?: @"",@"uptime":@(NSProcessInfo.processInfo.systemUptime)};
    @synchronized(records) { [records addObject:row]; }
    fprintf(stdout,"NEZHA HAPTICS SYSTEM: %s success=%d error=%ld\n",phase.UTF8String,success,(long)error.code);
    fflush(stdout);
}
static void pump(double seconds) {
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(end.timeIntervalSinceNow>0) {
        @autoreleasepool {
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:MIN(0.02,end.timeIntervalSinceNow)]];
        }
    }
}
static BOOL play(CHHapticEngine *engine, BOOL continuous, BOOL cancel) {
    NSError *error=nil;
    NSArray *parameters=@[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:0.25f],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f]];
    CHHapticEvent *event=continuous?
        [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:parameters relativeTime:0 duration:0.15]:
        [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient parameters:parameters relativeTime:0];
    CHHapticPattern *pattern=[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
    note(continuous?@"continuous-pattern":@"transient-pattern",pattern!=nil,error);
    if(!pattern)return NO;
    id<CHHapticPatternPlayer> player=[engine createPlayerWithPattern:pattern error:&error];
    note(@"player",player!=nil,error);if(!player)return NO;
    BOOL started=[player startAtTime:CHHapticTimeImmediate error:&error];
    note(cancel?@"cancel-start":continuous?@"continuous-start":@"transient-start",started,error);
    if(!started)return NO;
    pump(cancel?0.05:0.3);
    if(cancel) {
        BOOL stopped=[player stopAtTime:CHHapticTimeImmediate error:&error];
        note(@"player-cancel",stopped,error);if(!stopped)return NO;
    }
    pump(1.0);return YES;
}
int main(void) {
    alarm(180);
    @autoreleasepool {
        records=[NSMutableArray array];
        // Allow normal launchd/audiomxd/HAL startup before the finite test.
        pump(90);
        id<CHHapticDeviceCapability> capability=[CHHapticEngine capabilitiesForHardware];
        note(@"supports-haptics",capability.supportsHaptics,nil);
        NSError *error=nil;
        CHHapticEngine *engine=[[CHHapticEngine alloc] initAndReturnError:&error];
        note(@"engine-create",engine!=nil,error);
        BOOL success=NO;
        if(engine) {
            engine.playsHapticsOnly=YES;
            BOOL started=[engine startAndReturnError:&error];
            note(@"engine-start",started,error);
            if(started) {
                success=play(engine,NO,NO);
                if(success)success=play(engine,YES,NO);
                if(success)success=play(engine,YES,YES);
                __block atomic_int done;
                atomic_init(&done,0);
                [engine stopWithCompletionHandler:^(NSError *stopError){note(@"engine-stop",stopError==nil,stopError);atomic_store(&done,1);}];
                for(unsigned i=0;i<250 && !atomic_load(&done);++i)pump(0.02);
                note(@"engine-stop-callback",atomic_load(&done)!=0,nil);success=success&&atomic_load(&done);
            }
        }
        NSData *data=[NSJSONSerialization dataWithJSONObject:@{@"records":records,@"api_success":@(success),
            @"hardware_sensation_verified":@NO,@"version":@1} options:NSJSONWritingPrettyPrinted error:&error];
        BOOL saved=data && [data writeToFile:@"/private/var/tmp/nezha-haptics-core001.json" options:0 error:&error];
        note(@"result-written",saved,error);
        return success&&saved?0:1;
    }
}
