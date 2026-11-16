/* SPDX-License-Identifier: MIT */
#include "../package/camera/userland/NezhaCameraLSRegistration.m"
#include <assert.h>

static BOOL fixture_server, fixture_accept, fixture_visible, fixture_info;
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
- (BOOL)hasServer {return YES;}
- (BOOL)isInSyncBubble {return NO;}
- (BOOL)isLightweightSystemServer {return NO;}
- (BOOL)registerApplication:(NSURL *)url
{(void)url;assert(!"The legacy wrapper loses installation info and NSError");return NO;}
- (id)initWithLegacyDictionaries:(NSArray *)dictionaries personasWithAttributes:(id)personas
{
    self=[super init];assert(dictionaries.count==1 && !personas);
    NSDictionary *record=dictionaries[0];
    assert([record[@"ApplicationType"] isEqualToString:@"System"] &&
        [record[@"CFBundleIdentifier"] isEqualToString:@"com.apple.camera"] &&
        [record[@"CompatibilityState"] isEqual:@0] && [record[@"IsDeletable"] isEqual:@0]);
    assert([record[@"Path"] hasSuffix:@"/Camera.app"]);
    return self;
}
- (BOOL)hasInfoForBundleIdentifier:(NSString *)identifier
{assert([identifier isEqualToString:@"com.apple.camera"]);return fixture_info;}
@end

static bool register_url_fixture(void *context,NSURL *url,uint32_t flags,id installation,NSError **error)
{
    assert(!context && flags==0x02000001u && [installation isKindOfClass:[RegistrationFixture class]] && error);
    assert([url.lastPathComponent isEqualToString:@"Camera.app"]);++calls;
    if (fixture_accept && fixture_visible) {
        RegistrationFixtureProxy *entry=[RegistrationFixtureProxy new];
        entry.identifier=@"com.apple.camera";entry.path=url;entry.installed=YES;proxy=entry;
    }
    if (!fixture_accept) *error=[NSError errorWithDomain:@"FixtureRegistration" code:-10811
        userInfo:@{NSLocalizedDescriptionKey:@"native registration error retained"}];
    return (bool)fixture_accept;
}

static void reset_fixture(void)
{ fixture_server=fixture_accept=fixture_visible=fixture_info=YES;calls=registration_attempted=completion_registration_attempted=0;proxy=nil;
  original_register_url=register_url_fixture; }
static void invoke(NSURL *url)
{ register_camera([RegistrationFixture new],[RegistrationFixture class],[RegistrationFixtureProxy class],url,&registration_attempted); }

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
        reset_fixture();BOOL changed=NO;
        BOOL registered=register_camera([RegistrationFixture new],[RegistrationFixture class],
            [RegistrationFixtureProxy class],url,&registration_attempted);
        publish_registration_change(&changed,registered);
        assert(registered && changed && calls==1);
        changed=YES;publish_registration_change(&changed,NO);assert(changed);
        changed=NO;publish_registration_change(&changed,NO);assert(!changed);
        publish_registration_change(NULL,YES);
        reset_fixture();fixture_accept=NO;changed=NO;
        registered=register_camera([RegistrationFixture new],[RegistrationFixture class],
            [RegistrationFixtureProxy class],url,&registration_attempted);
        publish_registration_change(&changed,registered);assert(!registered && !changed && calls==1);
        reset_fixture();RegistrationFixtureProxy *entry=[RegistrationFixtureProxy new];
        entry.identifier=@"com.apple.camera";entry.installed=YES;proxy=entry;
        invoke(url);assert(!calls); /* An installed entry requires no Info.plist read. */
        reset_fixture();fixture_server=NO;invoke(url);assert(!calls && !proxy);
        reset_fixture();fixture_accept=NO;invoke(url);assert(calls==1 && !camera_installed(proxy));
        reset_fixture();fixture_visible=NO;invoke(url);assert(calls==1 && !camera_installed(proxy));
        reset_fixture();fixture_info=NO;invoke(url);assert(!calls);
        reset_fixture();original_register_url=NULL;invoke(url);assert(!calls);
        reset_fixture();invoke(url);assert(calls==1 && camera_installed(proxy));
        /* Original MI seeding can replace the initial DB entry. At its
         * native completion only, recheck and restore this same Camera. */
        proxy=nil;
        register_camera([RegistrationFixture new],[RegistrationFixture class],[RegistrationFixtureProxy class],url,&completion_registration_attempted);
        assert(calls==2 && camera_installed(proxy));
        register_camera([RegistrationFixture new],[RegistrationFixture class],[RegistrationFixtureProxy class],url,&completion_registration_attempted);
        assert(calls==2);
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
        puts("Camera LS registration: 18 native consumer cases passed, accepted registration reports database change, native true survives, failed registration stays unchanged, original install info/NSError ABI preserved");
    }
    return 0;
}
