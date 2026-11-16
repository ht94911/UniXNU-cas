/* SPDX-License-Identifier: MIT */
#include "NezhaCameraLSRegistration.m"
#include <assert.h>

static BOOL fixture_server, fixture_accept, fixture_visible;
static unsigned calls;
static id proxy;

@interface RegistrationFixtureProxy : NSObject
@property(nonatomic,copy) NSString *identifier;
@property(nonatomic,strong) NSURL *path;
@property(nonatomic) BOOL installed;
@property(nonatomic) BOOL placeholder;
@end
@implementation RegistrationFixtureProxy
+ (id)applicationProxyForIdentifier:(NSString *)identifier
{ assert([identifier isEqualToString:@"com.apple.camera"]);return proxy; }
- (NSString *)applicationIdentifier {return self.identifier;}
- (NSURL *)bundleURL {return self.path;}
- (BOOL)isInstalled {return self.installed;}
- (BOOL)isPlaceholder {return self.placeholder;}
@end

@interface RegistrationFixture : NSObject
@end
@implementation RegistrationFixture
+ (id)sharedInstance {assert(!"Actual075 classaccessor is forbidden;do not use it");return nil;}
+ (id)defaultWorkspace {return [self new];}
- (BOOL)isServer {return fixture_server;}
- (BOOL)registerApplication:(NSURL *)url
{
    assert([url.lastPathComponent isEqualToString:@"Camera.app"]);++calls;
    if (fixture_accept && fixture_visible) {
        RegistrationFixtureProxy *entry=[RegistrationFixtureProxy new];
        entry.identifier=@"com.apple.camera";entry.path=url;entry.installed=YES;proxy=entry;
    }
    return fixture_accept;
}
@end

static void reset_fixture(void)
{ fixture_server=fixture_accept=fixture_visible=YES;calls=registration_attempted=0;proxy=nil; }
static void invoke(NSURL *url)
{ register_camera([RegistrationFixture new],[RegistrationFixture class],[RegistrationFixtureProxy class],url); }

int main(void)
{
    @autoreleasepool {
        char directory[]="/tmp/nezha-camera-ls-XXXXXX";assert(mkdtemp(directory));
        NSURL *root=[NSURL fileURLWithPath:@(directory) isDirectory:YES];
        NSURL *url=[root URLByAppendingPathComponent:@"Camera.app" isDirectory:YES];
        NSFileManager *files=[NSFileManager defaultManager];
        assert([files createDirectoryAtURL:url withIntermediateDirectories:NO attributes:nil error:NULL]);
        NSURL *info=[url URLByAppendingPathComponent:@"Info.plist"];
        NSDictionary *identity=@{@"CFBundleIdentifier":@"com.apple.camera",@"CFBundleExecutable":@"Camera"};
        assert([identity writeToURL:info atomically:YES]);
        NSURL *executable=[url URLByAppendingPathComponent:@"Camera"];
        assert([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:executable atomically:YES]);
        reset_fixture();invoke(url);assert(calls==1 && camera_installed(proxy));
        invoke(url);assert(calls==1); /* Original seed can be called twice; registration remains once. */
        reset_fixture();RegistrationFixtureProxy *entry=[RegistrationFixtureProxy new];
        entry.identifier=@"com.apple.camera";entry.installed=YES;proxy=entry;
        invoke(url);assert(!calls); /* An installed entry requires no Info.plist read. */
        reset_fixture();fixture_server=NO;invoke(url);assert(!calls && !proxy);
        reset_fixture();fixture_accept=NO;invoke(url);assert(calls==1 && !camera_installed(proxy));
        reset_fixture();fixture_visible=NO;invoke(url);assert(calls==1 && !camera_installed(proxy));
        reset_fixture();entry=[RegistrationFixtureProxy new];entry.identifier=@"com.apple.camera";
        entry.installed=YES;entry.placeholder=YES;proxy=entry;
        invoke(url);assert(calls==1 && camera_installed(proxy));
        reset_fixture();assert([files removeItemAtURL:executable error:NULL]);
        invoke(url);assert(!calls);
        reset_fixture();assert([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:executable atomically:YES]);
        assert(([@{@"CFBundleIdentifier":@"com.apple.other",@"CFBundleExecutable":@"Camera"} writeToURL:info atomically:YES]));
        invoke(url);assert(!calls);
        reset_fixture();assert([files removeItemAtURL:info error:NULL]);invoke(url);assert(!calls);
        assert([files removeItemAtURL:root error:NULL]);
        puts("Camera LS registration: 10 native consumer cases passed, one request only, existing identity and server context preserved");
    }
    return 0;
}
