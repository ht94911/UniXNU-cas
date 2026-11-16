/* SPDX-License-Identifier: MIT */
#define NEZHA_CAMERA_LAUNCH_TEST 1
#include "NezhaCameraLaunch.m"
#include <assert.h>
#include <sys/stat.h>

static id current_proxy;
static unsigned registrations,launches;
static BOOL registration_result,launch_result,registration_visible;
@interface CameraFixtureRequestContext : NSObject
@property(nonatomic,strong) NSNumber *targetUserID;
@end
@implementation CameraFixtureRequestContext
@end
@interface CameraFixtureProxy : NSObject
@property(nonatomic,copy) NSString *identifier;
@property(nonatomic,strong) NSURL *path;
@property(nonatomic) BOOL installed;
@property(nonatomic) BOOL placeholder;
@end
@implementation CameraFixtureProxy
+ (id)applicationProxyForIdentifier:(NSString *)identifier
{ assert([identifier isEqualToString:@"com.apple.camera"]);return current_proxy; }
- (NSURL *)bundleURL {return self.path;}
- (NSString *)applicationIdentifier {return self.identifier;}
- (BOOL)isInstalled {return self.installed;}
- (BOOL)isPlaceholder {return self.placeholder;}
@end
@interface CameraFixtureWorkspace : NSObject
@end
@implementation CameraFixtureWorkspace
- (BOOL)registerApplicationForRebuildWithInfoDictionaries:(NSArray *)dictionaries
    requestContext:(id)context registrationError:(NSError **)error
{
    assert(dictionaries.count==1 && [context isKindOfClass:[CameraFixtureRequestContext class]] &&
        [((CameraFixtureRequestContext *)context).targetUserID isEqual:@0]);
    NSDictionary *registration=dictionaries[0];
    if (!registration_result && error) {
        NSError *underlying=[NSError errorWithDomain:@"FixtureXPC" code:4099
            userInfo:@{NSLocalizedDescriptionKey:@"com.apple.lsd.modify connection interrupted"}];
        *error=[NSError errorWithDomain:@"FixtureRegistration" code:13
            userInfo:@{NSUnderlyingErrorKey:underlying}];
    }
    assert([registration[@"ApplicationType"] isEqualToString:@"System"]);
    assert([registration[@"CFBundleIdentifier"] isEqualToString:@"com.apple.camera"]);
    assert([registration[@"CompatibilityState"] isEqual:@0] && [registration[@"IsDeletable"] isEqual:@0]);
    NSURL *url=[NSURL fileURLWithPath:registration[@"Path"] isDirectory:YES];
    assert([url.lastPathComponent isEqualToString:@"Camera.app"]);++registrations;
    if (registration_result && registration_visible) {
        CameraFixtureProxy *proxy=[CameraFixtureProxy new];proxy.identifier=@"com.apple.camera";
        proxy.path=url;proxy.installed=YES;current_proxy=proxy;
    }
    return registration_result;
}
- (BOOL)openApplicationWithBundleID:(NSString *)identifier
{ assert([identifier isEqualToString:@"com.apple.camera"]);++launches;return launch_result; }
@end
static void reset(void)
{ current_proxy=nil;registrations=launches=0;registration_result=registration_visible=launch_result=YES; }
int main(void)
{
    @autoreleasepool {
        char folder[]="/tmp/nezha-camera-launch-XXXXXX";assert(mkdtemp(folder));
        NSURL *root=[NSURL fileURLWithPath:@(folder) isDirectory:YES];
        NSURL *url=[root URLByAppendingPathComponent:@"Camera.app" isDirectory:YES];
        NSFileManager *files=[NSFileManager defaultManager];NSError *error=nil;
        assert([files createDirectoryAtURL:url withIntermediateDirectories:NO attributes:nil error:&error]);
        NSURL *info=[url URLByAppendingPathComponent:@"Info.plist"];
        assert(([@{@"CFBundleIdentifier":@"com.apple.camera",@"CFBundleExecutable":@"Camera"} writeToURL:info atomically:YES]));
        assert([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:[url URLByAppendingPathComponent:@"Camera"] atomically:YES]);
        CameraFixtureWorkspace *workspace=[CameraFixtureWorkspace new];
        reset();CameraFixtureProxy *proxy=[CameraFixtureProxy new];proxy.identifier=@"com.apple.camera";
        proxy.installed=YES;proxy.path=[NSURL fileURLWithPath:@"/unreadable/Camera.app"];current_proxy=proxy;
        assert(open_camera([CameraFixtureProxy class],workspace,proxy.path,[CameraFixtureRequestContext class])==0);
        assert(registrations==0 && launches==1); /* Old Info.plist gate rejected this real installed proxy. */
        reset();assert(open_camera([CameraFixtureProxy class],workspace,url,Nil)==5);
        assert(registrations==0 && launches==0);
        reset();assert(open_camera([CameraFixtureProxy class],workspace,url,[CameraFixtureRequestContext class])==0);
        assert(registrations==1 && launches==1); /* Actual current Data missing the application entry. */
        reset();registration_result=NO;assert(open_camera([CameraFixtureProxy class],workspace,url,[CameraFixtureRequestContext class])==6);
        assert(registrations==1 && launches==0);
        reset();registration_visible=NO;assert(open_camera([CameraFixtureProxy class],workspace,url,[CameraFixtureRequestContext class])==7);
        assert(registrations==1 && launches==0); /* A native acceptance alone is not a registered app. */
        reset();proxy=[CameraFixtureProxy new];proxy.identifier=@"com.apple.camera";proxy.installed=YES;proxy.placeholder=YES;current_proxy=proxy;
        assert(open_camera([CameraFixtureProxy class],workspace,url,[CameraFixtureRequestContext class])==0);assert(registrations==1 && launches==1);
        reset();launch_result=NO;assert(open_camera([CameraFixtureProxy class],workspace,url,[CameraFixtureRequestContext class])==9);assert(launches==1);
        reset();assert(([@{@"CFBundleIdentifier":@"com.apple.other",@"CFBundleExecutable":@"Camera"} writeToURL:info atomically:YES]));
        assert(open_camera([CameraFixtureProxy class],workspace,url,[CameraFixtureRequestContext class])==4);assert(registrations==0 && launches==0);
        reset();assert([files removeItemAtURL:info error:&error]);
        assert(open_camera([CameraFixtureProxy class],workspace,url,[CameraFixtureRequestContext class])==4);assert(registrations==0 && launches==0);
        assert([files removeItemAtURL:root error:&error]);
        puts("Camera launcher: 9 original consumer registration, unreadable installed bundle, placeholder and native error cases passed");
    }
    return 0;
}
