/* SPDX-License-Identifier: MIT */
#include "../package/camera/userland/NezhaCameraLSRegistration.m"
#include <assert.h>

static BOOL fixture_server, fixture_accept, fixture_visible, fixture_info;
static unsigned calls,resume_calls,accept_calls;
static id proxy;
static unsigned query_calls,store_calls,rebuilt_calls;
static id pending_query,pending_store;

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

static void resume_fixture(id self,SEL selector)
{assert(self && selector==@selector(resume));++resume_calls;}
static BOOL accept_fixture(id self,SEL selector,id listener,id connection)
{
    assert(self && selector==@selector(listener:shouldAcceptNewConnection:) && listener && connection);
    ++accept_calls;return fixture_accept;
}

@interface ListenerFixture : NSObject
@property(nonatomic,copy) NSString *name;
@end
@implementation ListenerFixture
- (NSString *)serviceName {return self.name;}
- (int)processIdentifier {return 123;}
- (id)XPCConnection {return self;}
@end

static void query_fixture(id self,SEL selector,id queries,BOOL legacy,id completion)
{
    assert(self && selector==sel_registerName("resolveQueries:legacySPI:completionHandler:") && [queries isEqual:@[@"native-query"]] && legacy);
    ++query_calls;pending_query=[completion copy];
}
static void store_fixture(id self,SEL selector,id completion)
{assert(self && selector==sel_registerName("getServerStoreNonBlockingWithCompletionHandler:"));++store_calls;pending_store=[completion copy];}
static void rebuilt_fixture(id self,SEL selector)
{assert(self && selector==sel_registerName("sendDatabaseRebuiltNotification"));++rebuilt_calls;}

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
        assert(ls_service_name(@"com.apple.lsd.mapdb") && ls_service_name(@"com.apple.lsd.system.modifydb"));
        assert(!ls_service_name(@"com.apple.camera") && !ls_service_name(nil));
        ListenerFixture *listener=[ListenerFixture new];listener.name=@"com.apple.lsd.mapdb";
        original_listener_resume=(IMP)resume_fixture;
        listener_resume(listener,@selector(resume));assert(resume_calls==1);
        listener.name=@"unrelated.service";listener_resume(listener,@selector(resume));assert(resume_calls==2);
        original_listener_accept=(IMP)accept_fixture;fixture_accept=NO;
        assert(!listener_accept(listener,@selector(listener:shouldAcceptNewConnection:),listener,listener) && accept_calls==1);
        fixture_accept=YES;
        assert(listener_accept(listener,@selector(listener:shouldAcceptNewConnection:),listener,listener) && accept_calls==2);
        original_read_queries=(IMP)query_fixture;original_read_store=(IMP)store_fixture;original_rebuilt=(IMP)rebuilt_fixture;
        NSDictionary *results=@{@"native-query":@[@"existing-record"]};
        NSError *native_error=[NSError errorWithDomain:@"LSNativeFixture" code:-10814 userInfo:nil];
        __block unsigned query_replies=0,store_replies=0;
        @autoreleasepool {
            read_queries(listener,sel_registerName("resolveQueries:legacySPI:completionHandler:"),@[@"native-query"],YES,^(id values,NSError *error) {
                assert(values==results && error==native_error);++query_replies;
            });
        }
        assert(query_calls==1 && query_replies==0 && pending_query);
        ((void (^)(id,NSError *))pending_query)(results,native_error);
        assert(query_replies==1);pending_query=nil;
        read_queries(listener,sel_registerName("resolveQueries:legacySPI:completionHandler:"),@[@"native-query"],YES,nil);
        assert(query_calls==2 && !pending_query);
        id store=[NSObject new],catalogue=[NSObject new],endpoint=[NSObject new];
        @autoreleasepool {
            read_store(listener,sel_registerName("getServerStoreNonBlockingWithCompletionHandler:"),^(id s,id c,id e,BOOL manual,NSError *error) {
                assert(s==store && c==catalogue && e==endpoint && manual && error==native_error);++store_replies;
            });
        }
        assert(store_calls==1 && store_replies==0 && pending_store);
        ((void (^)(id,id,id,BOOL,NSError *))pending_store)(store,catalogue,endpoint,YES,native_error);
        assert(store_replies==1);pending_store=nil;
        read_store(listener,sel_registerName("getServerStoreNonBlockingWithCompletionHandler:"),nil);
        assert(store_calls==2 && !pending_store);
        database_rebuilt(listener,sel_registerName("sendDatabaseRebuiltNotification"));assert(rebuilt_calls==1);
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
        puts("Camera LS registration: 28 native consumer cases passed; async original replies retain all arguments and NSError, nil completion stays nil, original rebuilt request runs once");
    }
    return 0;
}
