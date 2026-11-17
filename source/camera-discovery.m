/* SPDX-License-Identifier: MIT */
#include "NezhaCameraDiscovery.m"
#include <assert.h>

static unsigned source_calls,init_calls,device_calls,filter_calls,access_calls,property_calls,connection_calls;
static CameraSource expected_source;
static id expected_self,returned_device,expected_prior;
static CFArrayRef returned_sources;
static BOOL access_result;
static unsigned input_calls,session_start_calls,session_commit_calls,session_add_calls,session_can_add_calls;
static unsigned capabilities_calls,preferred_calls,engine_init_calls,engine_device_calls,engine_input_calls,engine_start_calls,available_calls;
static NSError *native_input_error;
static BOOL input_failed,session_allowed,available_value;
static id native_completion;

@interface NSObject (DiscoveryTestSelectors)
+ (CFArrayRef)_newFigCaptureSources;
- (id)_initWithFigCaptureSource:(CameraSource)source;
+ (id)_devicesWithPriorRegisteredDevices:(id)prior;
+ (void)_filterConnectedDevices:(id)devices withDeviceTypes:(id)types mediaType:(id)media position:(long)position;
- (BOOL)_cameraAccessIsEnabled;
- (id)_copyFigCaptureSourceProperty:(CFStringRef)key;
@end

@interface DiscoveryFixture : NSObject
@end
@implementation DiscoveryFixture
- (NSString *)uniqueID {return @"com.apple.avfoundation.avcapturedevice.built-in_video:0";}
- (NSString *)deviceType {return @"AVCaptureDeviceTypeBuiltInWideAngleCamera";}
- (BOOL)isConnected {return NO;}
@end

@interface SessionFixture : DiscoveryFixture
@end
@implementation SessionFixture
- (BOOL)isRunning {return NO;}
- (BOOL)isInterrupted {return YES;}
- (BOOL)isBackCameraSupported {return NO;}
- (BOOL)isFrontCameraSupported {return YES;}
- (BOOL)useMultiCamSession {return YES;}
@end

@interface MissingConstantFixture : NSObject
@property(nonatomic,getter=isRequired) BOOL required;
@end
@implementation MissingConstantFixture
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
static id property_fixture(id self,SEL selector,CFStringRef key)
{
    assert(self==expected_self && selector==@selector(_copyFigCaptureSourceProperty:));
    assert(CFEqual(key,CFSTR("Formats")));++property_calls;
    return [returned_device retain];
}
static id connection_fixture(id self,SEL selector,id name,unsigned long options)
{
    assert(self==expected_self && selector==@selector(initWithMachServiceName:options:));
    assert([name isEqual:@"com.apple.lsd.mapdb"] && options==0);
    ++connection_calls;[self release];return [returned_device retain];
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

static id input_fixture(id self,SEL selector,id device,NSError **error)
{
    assert(self==expected_self && selector==sel_registerName("initWithDevice:error:") && device==returned_device);
    ++input_calls;[self release];
    if (error) *error=input_failed ? native_input_error:nil;
    return input_failed ? nil:[returned_device retain];
}
static void session_void_fixture(id self,SEL selector)
{
    assert(self==returned_device);
    if (selector==sel_registerName("startRunning")) ++session_start_calls;
    else {assert(selector==sel_registerName("commitConfiguration"));++session_commit_calls;}
}
static void session_add_fixture(id self,SEL selector,id input)
{assert(self==returned_device && input==returned_device && selector==sel_registerName("addInput:"));++session_add_calls;}
static BOOL session_can_add_fixture(id self,SEL selector,id input)
{assert(self==returned_device && input==returned_device && selector==sel_registerName("canAddInput:"));++session_can_add_calls;return session_allowed;}
static id capabilities_fixture(id self,SEL selector,long host)
{assert(self==expected_self && selector==sel_registerName("initWithHostProcess:") && host==3);++capabilities_calls;[self release];return [returned_device retain];}
static id capabilities_get_fixture(id self,SEL selector)
{assert(self==returned_device && selector==sel_registerName("capabilities"));return returned_device;}
static long preferred_fixture(id self,SEL selector,long position,long mode,long configuration,long strength,BOOL simultaneous)
{
    assert(self==returned_device && selector==sel_registerName("preferredDeviceForPosition:mode:videoConfiguration:videoStabilizationStrength:frontRearSimultaneousVideoEnabled:"));
    assert(position==1 && mode==7 && configuration==4 && strength==2 && simultaneous);++preferred_calls;return 12;
}
static id engine_init_fixture(id self,SEL selector,id power,id controller,long options)
{assert(self==expected_self && selector==sel_registerName("initWithPowerController:captureController:options:") && power==returned_device && controller==returned_device && options==9);++engine_init_calls;[self release];return [returned_device retain];}
static id engine_device_fixture(id self,SEL selector,id type,long position)
{assert(self==returned_device && selector==sel_registerName("_captureEngineDeviceForDeviceType:position:") && [type isEqual:@"wide"] && position==1);++engine_device_calls;return returned_device;}
static id engine_input_fixture(id self,SEL selector)
{assert(self==returned_device && selector==sel_registerName("captureDeviceInput"));++engine_input_calls;return returned_device;}
static void engine_start_fixture(id self,SEL selector,unsigned long retries,double interval,id reason,id completion)
{
    assert(self==returned_device && selector==sel_registerName("startWithRetryCount:retryInterval:logReason:completion:") && retries==7 && interval==0.25 && [reason isEqual:@"native"] && completion==native_completion);
    ++engine_start_calls; /* A queued request does not call completion here. */
}
static void available_fixture(id self,SEL selector,BOOL available)
{assert(self==returned_device && selector==sel_registerName("_setCaptureAvailable:"));++available_calls;available_value=available;}

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
        expected_self=(id)@"original-client";returned_device=[DiscoveryFixture new];
        original_copy_property=(IMP)property_fixture;
        value=copy_property(expected_self,@selector(_copyFigCaptureSourceProperty:),CFSTR("Formats"));
        assert(value==returned_device && property_calls==1);[value release];[returned_device release];
        returned_device=nil;
        assert(!copy_property(expected_self,@selector(_copyFigCaptureSourceProperty:),CFSTR("Formats")) && property_calls==2);
        original_connection_init=(IMP)connection_fixture;
        expected_self=[NSObject new];returned_device=[DiscoveryFixture new];
        value=connection_init(expected_self,@selector(initWithMachServiceName:options:),@"com.apple.lsd.mapdb",0);
        assert(value==returned_device && connection_calls==1);[value release];[returned_device release];
        returned_device=[SessionFixture new];original_input_init=(IMP)input_fixture;
        native_input_error=[NSError errorWithDomain:@"AVNativeFixture" code:-11800 userInfo:nil];
        NSError *input_error=native_input_error;expected_self=[NSObject new];
        value=input_init(expected_self,sel_registerName("initWithDevice:error:"),returned_device,&input_error);
        assert(value==returned_device && !input_error && input_calls==1);[value release];
        input_failed=YES;expected_self=[NSObject new];
        assert(!input_init(expected_self,sel_registerName("initWithDevice:error:"),returned_device,&input_error));
        assert(input_error==native_input_error && input_calls==2);
        expected_self=[NSObject new];
        assert(!input_init(expected_self,sel_registerName("initWithDevice:error:"),returned_device,NULL) && input_calls==3);
        original_session_start=original_session_commit=(IMP)session_void_fixture;
        session_start(returned_device,sel_registerName("startRunning"));assert(session_start_calls==1);
        session_commit(returned_device,sel_registerName("commitConfiguration"));assert(session_commit_calls==1);
        original_session_add=(IMP)session_add_fixture;
        session_add(returned_device,sel_registerName("addInput:"),returned_device);assert(session_add_calls==1);
        original_session_can_add=(IMP)session_can_add_fixture;session_allowed=NO;
        assert(!session_can_add(returned_device,sel_registerName("canAddInput:"),returned_device));session_allowed=YES;
        assert(session_can_add(returned_device,sel_registerName("canAddInput:"),returned_device) && session_can_add_calls==2);
        original_capabilities_init=(IMP)capabilities_fixture;expected_self=[NSObject new];
        value=capabilities_init(expected_self,sel_registerName("initWithHostProcess:"),3);
        assert(value==returned_device && capabilities_calls==1);[value release];
        original_capabilities_get=(IMP)capabilities_get_fixture;
        assert(capabilities_get(returned_device,sel_registerName("capabilities"))==returned_device);
        original_preferred_device=(IMP)preferred_fixture;
        assert(preferred_device(returned_device,sel_registerName("preferredDeviceForPosition:mode:videoConfiguration:videoStabilizationStrength:frontRearSimultaneousVideoEnabled:"),1,7,4,2,YES)==12 && preferred_calls==1);
        original_engine_init=(IMP)engine_init_fixture;expected_self=[NSObject new];
        value=engine_init(expected_self,sel_registerName("initWithPowerController:captureController:options:"),returned_device,returned_device,9);
        assert(value==returned_device && engine_init_calls==1);[value release];
        original_engine_device=(IMP)engine_device_fixture;
        assert(engine_device(returned_device,sel_registerName("_captureEngineDeviceForDeviceType:position:"),@"wide",1)==returned_device && engine_device_calls==1);
        original_engine_input=(IMP)engine_input_fixture;
        assert(engine_input(returned_device,sel_registerName("captureDeviceInput"))==returned_device && engine_input_calls==1);
        original_engine_start=(IMP)engine_start_fixture;native_completion=^{assert(!"Observer must not run a native completion itself");};
        engine_start(returned_device,sel_registerName("startWithRetryCount:retryInterval:logReason:completion:"),7,0.25,@"native",native_completion);
        assert(engine_start_calls==1);original_capture_available=(IMP)available_fixture;
        capture_available(returned_device,sel_registerName("_setCaptureAvailable:"),NO);assert(!available_value);
        capture_available(returned_device,sel_registerName("_setCaptureAvailable:"),YES);assert(available_value && available_calls==2);
        [returned_device release];returned_device=nil;
        Class constants=[MissingConstantFixture class];
        assert(!class_getInstanceMethod(constants,sel_registerName("required")));
        assert(nezha_camera_metal_alias(constants));
        MissingConstantFixture *constant=[MissingConstantFixture new];
        constant.required=YES;
        assert(((BOOL (*)(id,SEL))objc_msgSend)(constant,sel_registerName("required")));
        constant.required=NO;
        assert(!((BOOL (*)(id,SEL))objc_msgSend)(constant,sel_registerName("required")));
        assert(nezha_camera_metal_alias(constants));[constant release];
        assert(!nezha_camera_metal_alias([NSObject class]));
        fflush(observations);fclose(observations);observations=NULL;
        puts("Camera discovery: 35 native consumer cases passed; required selector aliases real native getter, original owned input/session and callback values retained");
    }
    return 0;
}
