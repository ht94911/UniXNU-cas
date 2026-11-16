/* SPDX-License-Identifier: MIT */
/* HakiOS/umi camera_avtest supplies the AVFoundation enumeration reference.
 * Discovery only: no camera authorization request or capture is started.
 * A finite deadline also bounds an unavailable system camera service. */
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <unistd.h>

static NSDictionary *describe_format(AVCaptureDeviceFormat *format)
{
    const CMVideoDimensions size=CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    const FourCharCode subtype=CMFormatDescriptionGetMediaSubType(format.formatDescription);
    NSMutableArray *rates=[NSMutableArray array];
    for (AVFrameRateRange *range in format.videoSupportedFrameRateRanges)
        [rates addObject:@{@"min":@(range.minFrameRate),@"max":@(range.maxFrameRate)}];
    return @{@"width":@(size.width),@"height":@(size.height),@"pixel_format":@(subtype),
             @"frame_rates":rates};
}

int main(void)
{
    @autoreleasepool {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,20*NSEC_PER_SEC),
            dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{
                fputs("nezha-camera-enumerate: system discovery deadline reached\n",stderr);
                _exit(124);
            });
        AVCaptureDeviceDiscoverySession *session=[AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
            mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
        NSMutableArray *devices=[NSMutableArray array];
        bool main_found=false;
        for (AVCaptureDevice *device in session.devices) {
            NSMutableArray *formats=[NSMutableArray array];
            for (AVCaptureDeviceFormat *format in device.formats)
                [formats addObject:describe_format(format)];
            [devices addObject:@{@"id":device.uniqueID,@"name":device.localizedName,
                @"model":device.modelID,@"position":@(device.position),@"formats":formats}];
            if (device.position==AVCaptureDevicePositionBack &&
                [device.uniqueID isEqualToString:@"com.apple.avfoundation.avcapturedevice.built-in_video:0"])
                main_found=true;
        }
        NSDictionary *report=@{@"devices":devices,@"expected_main_found":@(main_found),
            @"authorization":@([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]),
            @"capture_started":@NO,@"authorization_requested":@NO};
        NSError *error=nil;
        NSData *data=[NSJSONSerialization dataWithJSONObject:report
            options:NSJSONWritingPrettyPrinted error:&error];
        if (!data) {
            fprintf(stderr,"nezha-camera-enumerate: %s\n",error.description.UTF8String);
            return 2;
        }
        if (fwrite(data.bytes,1,data.length,stdout)!=data.length || fputc('\n',stdout)==EOF)
            return 3;
        return main_found?0:1;
    }
}
