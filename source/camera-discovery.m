/* SPDX-License-Identifier: MIT */
#include "NezhaCameraDiscovery.m"
#include <assert.h>

static unsigned source_calls,init_calls,device_calls,filter_calls,access_calls;
static CameraSource expected_source;
static id expected_self,returned_device,expected_prior;
static CFArrayRef returned_sources;
static BOOL access_result;

@interface NSObject (DiscoveryTestSelectors)
+ (CFArrayRef)_newFigCaptureSources;
- (id)_initWithFigCaptureSource:(CameraSource)source;
+ (id)_devicesWithPriorRegisteredDevices:(id)prior;
+ (void)_filterConnectedDevices:(id)devices withDeviceTypes:(id)types mediaType:(id)media position:(long)position;
- (BOOL)_cameraAccessIsEnabled;
@end

@interface DiscoveryFixture : NSObject
@end
@implementation DiscoveryFixture
- (NSString *)uniqueID {return @"com.apple.avfoundation.avcapturedevice.built-in_video:0";}
- (NSString *)deviceType {return @"AVCaptureDeviceTypeBuiltInWideAngleCamera";}
- (BOOL)isConnected {return NO;}
@end

static CFArrayRef source_fixture(id self,SEL selector)
{
    assert(self==expected_self && selector==@selector(_newFigCaptureSources));++source_calls;
    return returned_sources;
}
static id init_fixture(id self,SEL selector,CameraSource source)
{
    assert(self==expected_self && selector==@selector(_initWithFigCaptureSource:) && source==expected_source);
    ++init_calls;
    [self release]; /* Native init can consume self and return a different object. */
    return [returned_device retain];
}
static id devices_fixture(id self,SEL selector,id prior)
{
    assert(self==expected_self && selector==@selector(_devicesWithPriorRegisteredDevices:) && prior==expected_prior);
    ++device_calls;return prior;
}
static void filter_fixture(id self,SEL selector,id devices,id types,id media,long position)
{
    assert(self==expected_self && selector==@selector(_filterConnectedDevices:withDeviceTypes:mediaType:position:));
    assert([types isEqual:@[@"AVCaptureDeviceTypeBuiltInWideAngleCamera"]] && [media isEqual:@"vide"] && position==2);
    ++filter_calls;[devices removeAllObjects];
}
static BOOL access_fixture(id self,SEL selector)
{
    assert(self==expected_self && selector==@selector(_cameraAccessIsEnabled));++access_calls;return access_result;
}
static int32_t copy_fixture(NezhaFigObject object,NezhaFigString key,NezhaFigAllocator allocator,void *out)
{
    assert(object==(NezhaFigObject)expected_source && allocator==NULL);
    if ([(id)key isEqual:@"Formats"]) {
        *(CFTypeRef *)out=CFRetain((CFTypeRef)@[@"real-format"]);return 0;
    }
    *(CFTypeRef *)out=NULL;return -12784;
}
static const struct NezhaFigBaseClass base={.version=1,.copy_property=copy_fixture};
static const struct NezhaFigDeviceVTable vtable={.base=&base};
static const struct NezhaFigDeviceVTable *vtable_fixture(NezhaFigObject object)
{assert(object==(NezhaFigObject)expected_source);return &vtable;}

int main(void)
{
    @autoreleasepool {
        observations=tmpfile();assert(observations);
        expected_self=(id)@"original-client";
        original_new_sources=(IMP)source_fixture;
        CFArrayRef empty=CFArrayCreate(NULL,NULL,0,&kCFTypeArrayCallBacks);
        returned_sources=empty;
        assert(new_sources(expected_self,@selector(_newFigCaptureSources))==empty && source_calls==1);
        CFRelease(empty);
        returned_sources=NULL;
        assert(!new_sources(expected_self,@selector(_newFigCaptureSources)) && source_calls==2);
        original_video_devices=(IMP)devices_fixture;
        expected_prior=@[@"existing-device"];
        assert(video_devices(expected_self,@selector(_devicesWithPriorRegisteredDevices:),expected_prior)==expected_prior && device_calls==1);
        original_access=(IMP)access_fixture;access_result=NO;
        assert(!camera_access(expected_self,@selector(_cameraAccessIsEnabled)) && access_calls==1);
        access_result=YES;
        assert(camera_access(expected_self,@selector(_cameraAccessIsEnabled)) && access_calls==2);
        original_filter=(IMP)filter_fixture;
        DiscoveryFixture *item=[DiscoveryFixture new];
        NSMutableArray *devices=[NSMutableArray arrayWithObject:item];
        filter_devices(expected_self,@selector(_filterConnectedDevices:withDeviceTypes:mediaType:position:),
            devices,@[@"AVCaptureDeviceTypeBuiltInWideAngleCamera"],@"vide",2);
        assert(!devices.count && filter_calls==1);[item release];
        original_video_init=(IMP)init_fixture;
        expected_self=[NSObject new];expected_source=(CameraSource)0x123400;
        returned_device=[DiscoveryFixture new];
        id made=video_init(expected_self,@selector(_initWithFigCaptureSource:),expected_source);
        assert(made==returned_device && init_calls==1);[made release];[returned_device release];
        expected_self=[NSObject new];returned_device=nil;expected_source=NULL;
        assert(!video_init(expected_self,@selector(_initWithFigCaptureSource:),NULL) && init_calls==2);
        expected_source=(CameraSource)0x123400;source_vtable=vtable_fixture;
        int32_t error=0;
        id value=source_property(expected_source,CFSTR("Formats"),&error);
        assert(error==0 && [value isEqual:@[@"real-format"]]);[value release];
        value=source_property(expected_source,CFSTR("Attributes"),&error);
        assert(error==-12784 && value==nil);
        value=source_property(NULL,CFSTR("Formats"),&error);
        assert(error==-12782 && value==nil);
        fflush(observations);fclose(observations);observations=NULL;
        puts("Camera discovery: 11 native consumer cases passed; original CF ownership, init consumes self, void filter and original access verdicts retained");
    }
    return 0;
}
