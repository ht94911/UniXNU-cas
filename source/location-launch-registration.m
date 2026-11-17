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
@interface LocationFixtureContext : NSObject
@property(nonatomic,strong) NSNumber *targetUserID;
@end
@implementation LocationFixtureContext
@end
Class fixture_context_class(const char *name) { assert(!strcmp(name,"LSOperationRequestContext"));return LocationFixtureContext.class; }
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
- (BOOL)openApplicationWithBundleID:(NSString *)identifier
{ assert([identifier isEqualToString:@"org.unixnu.nezha.location"]);++launches;return launch_result; }
@end
static void reset(void)
{ current_proxy=nil;registrations=launches=0;registration_result=registration_visible=launch_result=YES; }
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
        puts("Location launcher: 9 registration cases and mobile-owned output beside retained read-only file passed");
    }
    return 0;
}
