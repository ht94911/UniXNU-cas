/* SPDX-License-Identifier: MIT */
#define NEZHA_LOCATION_LAUNCH_TEST 1
#define objc_getClass fixture_context_class
#include "NezhaLocationLaunch.m"
#undef objc_getClass
#include <assert.h>
#include <sys/stat.h>

static id current_proxy;
static unsigned registrations,launches;
static BOOL registration_result,launch_result,registration_visible;
static BOOL launch_callback;
static NSError *launch_error;
static id launch_process;
static id launch_service;
static BOOL launch_options_available;
@interface LocationFixtureContext : NSObject
@property(nonatomic,strong) NSNumber *targetUserID;
@end
@implementation LocationFixtureContext
@end
@interface LocationFixtureLaunchService : NSObject
@end
@interface LocationFixtureLaunchOptions : NSObject
@end
@interface LocationFixtureProcess : NSObject
@property(nonatomic) int pid;
@property(nonatomic) BOOL valid;
@end
@implementation LocationFixtureProcess
- (BOOL)isValid {return self.valid;}
@end
Class fixture_context_class(const char *name)
{
    if(!strcmp(name,"LSOperationRequestContext"))return LocationFixtureContext.class;
    if(!strcmp(name,"FBSOpenApplicationService"))return LocationFixtureLaunchService.class;
    assert(!strcmp(name,"FBSOpenApplicationOptions"));return LocationFixtureLaunchOptions.class;
}
@interface LocationFixtureProxy : NSObject
@property(nonatomic,copy) NSString *identifier;
@property(nonatomic,strong) NSURL *path;
@property(nonatomic) BOOL installed;
@property(nonatomic) BOOL placeholder;
@end
@implementation LocationFixtureProxy
+ (id)applicationProxyForIdentifier:(NSString *)identifier
{ assert([identifier isEqualToString:@"org.unixnu.nezha.location"]);return current_proxy; }
- (NSURL *)bundleURL {return self.path;}
- (NSString *)applicationIdentifier {return self.identifier;}
- (BOOL)isInstalled {return self.installed;}
- (BOOL)isPlaceholder {return self.placeholder;}
@end
@interface LocationFixtureWorkspace : NSObject
@end
@implementation LocationFixtureWorkspace
- (id)registerContainerizedApplicationWithInfoDictionaries:(NSArray *)dictionaries
    operationUUID:(NSUUID *)operation requestContext:(id)context saveObserver:(id)observer
    registrationError:(NSError **)error
{
    assert(dictionaries.count==1 && [operation isKindOfClass:[NSUUID class]] && [context isKindOfClass:LocationFixtureContext.class] && [[context targetUserID] isEqual:@501] && observer==nil);
    NSDictionary *registration=dictionaries[0];
    if (!registration_result && error) {
        NSError *underlying=[NSError errorWithDomain:@"FixtureXPC" code:4099
            userInfo:@{NSLocalizedDescriptionKey:@"com.apple.lsd.modify connection interrupted"}];
        *error=[NSError errorWithDomain:@"FixtureRegistration" code:13
            userInfo:@{NSUnderlyingErrorKey:underlying}];
    }
    assert([registration[@"ApplicationType"] isEqualToString:@"System"]);
    assert([registration[@"CFBundleIdentifier"] isEqualToString:@"org.unixnu.nezha.location"]);
    assert([registration[@"CompatibilityState"] isEqual:@0] && [registration[@"IsDeletable"] isEqual:@0]);
    NSURL *url=[NSURL fileURLWithPath:registration[@"Path"] isDirectory:YES];
    assert([url.lastPathComponent isEqualToString:@"NezhaLocation.app"]);++registrations;
    if (registration_result && registration_visible) {
        LocationFixtureProxy *proxy=[LocationFixtureProxy new];proxy.identifier=@"org.unixnu.nezha.location";
        proxy.path=url;proxy.installed=YES;current_proxy=proxy;
    }
    return registration_result ? @{} : nil;
}
@end
@implementation LocationFixtureLaunchOptions
+ (id)optionsWithDictionary:(NSDictionary *)dictionary
{ assert(dictionary.count==0);return launch_options_available ? [self new]:nil; }
@end
@implementation LocationFixtureLaunchService
+ (id)serviceWithDefaultShellEndpoint {return launch_service;}
- (void)openApplication:(NSString *)identifier withOptions:(id)options
    completion:(void (^)(id process,NSError *error))completion
{
    assert([identifier isEqualToString:@"org.unixnu.nezha.location"] && [options isKindOfClass:LocationFixtureLaunchOptions.class]);
    ++launches;
    if (launch_callback) {
        id process=launch_result ? launch_process:nil;
        NSError *error=launch_error;
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0),^{
            assert(!NSThread.isMainThread);
            completion(process,error);
        });
    }
}
@end
static void reset(void)
{
    current_proxy=nil;registrations=launches=0;
    registration_result=registration_visible=launch_result=launch_callback=launch_options_available=YES;
    launch_error=nil;launch_service=[LocationFixtureLaunchService new];
    LocationFixtureProcess *process=[LocationFixtureProcess new];process.pid=321;process.valid=YES;launch_process=process;
}
int main(void)
{
    @autoreleasepool {
        char folder[]="/tmp/nezha-location-launch-XXXXXX";assert(mkdtemp(folder));
        NSURL *root=[NSURL fileURLWithPath:@(folder) isDirectory:YES];
        NSURL *url=[root URLByAppendingPathComponent:@"NezhaLocation.app" isDirectory:YES];
        NSFileManager *files=[NSFileManager defaultManager];NSError *error=nil;
        assert([files createDirectoryAtURL:url withIntermediateDirectories:NO attributes:nil error:&error]);
        NSURL *info=[url URLByAppendingPathComponent:@"Info.plist"];
        assert(([@{@"CFBundleIdentifier":@"org.unixnu.nezha.location",@"CFBundleExecutable":@"NezhaLocation"} writeToURL:info atomically:YES]));
        assert([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:[url URLByAppendingPathComponent:@"NezhaLocation"] atomically:YES]);
        LocationFixtureWorkspace *workspace=[LocationFixtureWorkspace new];
        reset();LocationFixtureProxy *proxy=[LocationFixtureProxy new];proxy.identifier=@"org.unixnu.nezha.location";
        proxy.installed=YES;proxy.path=[NSURL fileURLWithPath:@"/unreadable/NezhaLocation.app"];current_proxy=proxy;
        assert(open_location([LocationFixtureProxy class],workspace,proxy.path)==0);
        assert(registrations==0 && launches==1); /* Old Info.plist gate rejected this real installed proxy. */
        reset();assert(open_location([LocationFixtureProxy class],nil,url)==5);
        assert(registrations==0 && launches==0);
        reset();assert(open_location([LocationFixtureProxy class],workspace,url)==0);
        assert(registrations==1 && launches==1); /* Actual current Data missing the application entry. */
        reset();registration_result=NO;assert(open_location([LocationFixtureProxy class],workspace,url)==6);
        assert(registrations==1 && launches==0);
        reset();registration_visible=NO;assert(open_location([LocationFixtureProxy class],workspace,url)==7);
        assert(registrations==1 && launches==0); /* A native acceptance alone is not a registered app. */
        reset();proxy=[LocationFixtureProxy new];proxy.identifier=@"org.unixnu.nezha.location";proxy.installed=YES;proxy.placeholder=YES;current_proxy=proxy;
        assert(open_location([LocationFixtureProxy class],workspace,url)==0);assert(registrations==1 && launches==1);
        reset();launch_result=NO;assert(open_location([LocationFixtureProxy class],workspace,url)==9);assert(launches==1);
        reset();launch_result=NO;
        NSError *underlying=[NSError errorWithDomain:@"FixtureOpenService" code:4099
            userInfo:@{NSLocalizedDescriptionKey:@"original application service interrupted"}];
        launch_error=[NSError errorWithDomain:@"FixtureLaunch" code:31
            userInfo:@{NSUnderlyingErrorKey:underlying,NSLocalizedDescriptionKey:@"application request declined"}];
        NSURL *launch_log=[root URLByAppendingPathComponent:@"launch-error.stderr"];
        int saved_stderr=dup(STDERR_FILENO);assert(saved_stderr>=0);
        assert(location_output(launch_log.fileSystemRepresentation)==0);
        assert(open_location([LocationFixtureProxy class],workspace,url)==9);
        fflush(stderr);assert(dup2(saved_stderr,STDERR_FILENO)>=0);close(saved_stderr);
        NSString *diagnostic=[NSString stringWithContentsOfURL:launch_log encoding:NSUTF8StringEncoding error:&error];
        assert([diagnostic containsString:@"launch_completed=1 process_handle=0 process_pid=0 process_valid=0 error=31 domain=FixtureLaunch"]);
        assert([diagnostic containsString:@"application request declined"]);
        assert([diagnostic containsString:@"launch-underlying="] && [diagnostic containsString:@"FixtureOpenService"]);
        assert(registrations==1 && launches==1);
        reset();launch_error=underlying;
        assert(open_location([LocationFixtureProxy class],workspace,url)==9);
        assert(launches==1); /* A true native result with an error is not success. */
        reset();assert(request_location(NSObject.class,LocationFixtureLaunchOptions.class,@"org.unixnu.nezha.location",DISPATCH_TIME_NOW)==8);
        assert(launches==0);
        reset();launch_callback=NO;
        assert(request_location(LocationFixtureLaunchService.class,LocationFixtureLaunchOptions.class,@"org.unixnu.nezha.location",
            dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_MSEC))==11);
        assert(launches==1); /* One request, no completion and no retry. */
        reset();launch_process=nil;
        assert(open_location(LocationFixtureProxy.class,workspace,url)==9);assert(launches==1);
        reset();[(LocationFixtureProcess *)launch_process setPid:0];
        assert(open_location(LocationFixtureProxy.class,workspace,url)==9);assert(launches==1);
        reset();[(LocationFixtureProcess *)launch_process setValid:NO];
        assert(open_location(LocationFixtureProxy.class,workspace,url)==9);assert(launches==1);
        reset();launch_service=nil;
        assert(open_location(LocationFixtureProxy.class,workspace,url)==8);assert(launches==0);
        reset();launch_options_available=NO;
        assert(open_location(LocationFixtureProxy.class,workspace,url)==8);assert(launches==0);
        reset();assert(([@{@"CFBundleIdentifier":@"com.apple.other",@"CFBundleExecutable":@"NezhaLocation"} writeToURL:info atomically:YES]));
        assert(open_location([LocationFixtureProxy class],workspace,url)==4);assert(registrations==0 && launches==0);
        reset();assert([files removeItemAtURL:info error:&error]);
        assert(open_location([LocationFixtureProxy class],workspace,url)==4);assert(registrations==0 && launches==0);
        NSURL *old_output=[root URLByAppendingPathComponent:@"old-root.stderr"];
        NSURL *new_output=[root URLByAppendingPathComponent:@"mobile.stderr"];
        NSData *retained=[@"retained prior result" dataUsingEncoding:NSUTF8StringEncoding];
        assert([retained writeToURL:old_output options:0 error:&error]);
        assert(chmod(old_output.fileSystemRepresentation,0444)==0);
        int old_stderr=dup(STDERR_FILENO);assert(old_stderr>=0);
        assert(location_output(new_output.fileSystemRepresentation)==0);
        static const char marker[]="new mobile process marker\n";
        assert(write(STDERR_FILENO,marker,sizeof(marker)-1)==sizeof(marker)-1);
        assert(dup2(old_stderr,STDERR_FILENO)>=0);close(old_stderr);
        struct stat output_stat={0};assert(stat(new_output.fileSystemRepresentation,&output_stat)==0);
        assert(output_stat.st_uid==geteuid()&&(output_stat.st_mode&0777)==0600);
        assert([[NSData dataWithContentsOfURL:old_output]isEqual:retained]);
        assert([[NSData dataWithContentsOfURL:new_output]isEqual:
            [NSData dataWithBytes:marker length:sizeof(marker)-1]]);
        assert([files removeItemAtURL:root error:&error]);
        puts("Location launcher: 18 registration/ordinary shell completion cases and mobile-owned output beside retained read-only file passed");
    }
    return 0;
}
