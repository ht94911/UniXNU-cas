// Host-only checks execute the production helper with a private temporary
// directory and simulated SecItem results. They do not validate device K1.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <LocalAuthentication/LocalAuthentication.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <assert.h>

static NSString *testCache;
static NSString *testBoot;
static NSMutableDictionary *items;
static NSMutableArray *observations;
static BOOL rejectAdd;
static BOOL rejectCopy;
static unsigned calls;
static uid_t testGetuid(void) { return 501; }
static int testSysctl(const char *name, void *old, size_t *size, void *newValue, size_t newSize)
{
    assert(!strcmp(name, "kern.bootsessionuuid") && !newValue && !newSize);
    const char *value = testBoot.UTF8String;
    size_t length = strlen(value) + 1;
    assert(*size >= length);
    memcpy(old, value, length); *size = length;
    return 0;
}
static NSArray<NSString *> *testDirectories(NSSearchPathDirectory d, NSSearchPathDomainMask m, BOOL expand)
{
    assert(d == NSCachesDirectory && m == NSUserDomainMask && expand);
    return @[testCache];
}
static OSStatus testAdd(CFDictionaryRef, CFTypeRef *);
static OSStatus testCopy(CFDictionaryRef, CFTypeRef *);
static OSStatus testUpdate(CFDictionaryRef, CFDictionaryRef);
static OSStatus testDelete(CFDictionaryRef);
CFErrorRef testLastError(OSStatus) CF_RETURNS_RETAINED;
#define getuid testGetuid
#define sysctlbyname testSysctl
#define NSSearchPathForDirectoriesInDomains testDirectories
#define SecItemAdd testAdd
#define SecItemCopyMatching testCopy
#define SecItemUpdate testUpdate
#define SecItemDelete testDelete
#define SecCopyLastError testLastError
#include "../source/AccountKeyItemProbe.m"
#undef getuid
#undef sysctlbyname
#undef NSSearchPathForDirectoriesInDomains
#undef SecItemAdd
#undef SecItemCopyMatching
#undef SecItemUpdate
#undef SecItemDelete
#undef SecCopyLastError

static NSString *resultPath(void)
{
    return [testCache stringByAppendingPathComponent:@"org.unixnu.nezha.account27.test/result.json"];
}
static NSDictionary *saved(void)
{
    NSData *data = [NSData dataWithContentsOfFile:resultPath()];
    assert(data);
    NSDictionary *value = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    assert(value);
    return value;
}
static NSString *beforeCall(CFDictionaryRef raw, NSString *api)
{
    ++calls;
    NSDictionary *q = (__bridge NSDictionary *)raw;
    assert([q[(__bridge id)kSecAttrAccessGroup] isEqual:@"org.unixnu.nezha.account27.test"]);
    assert([q[(__bridge id)kSecAttrService] isEqual:@"org.unixnu.nezha.account27.SecItem.v1"]);
    assert([q[(__bridge id)kSecAttrSynchronizable] isEqual:@NO]);
    // At the point that a real API could block, the checkpoint already exists.
    NSDictionary *disk = saved(), *view = observations.lastObject;
    assert([disk[@"run_state"] isEqual:@"in_progress"] && !disk[@"exit_code"]);
    assert([disk[@"activity"] isEqual:api] && [disk[@"call_state"] isEqual:@"before"]);
    assert([view[@"activity"] isEqual:api] && !view[@"exit_code"]);
    NSString *name = q[(__bridge id)kSecAttrAccount];
    assert([name isEqual:@"canary"] || [name isEqual:@"scratch"]);
    return name;
}
static OSStatus testAdd(CFDictionaryRef q, CFTypeRef *out)
{
    NSString *name = beforeCall(q, @"SecItemAdd"); assert(!out);
    if (rejectAdd) return errSecInteractionNotAllowed;
    if (items[name]) return errSecDuplicateItem;
    items[name] = ((__bridge NSDictionary *)q)[(__bridge id)kSecValueData];
    return errSecSuccess;
}
static OSStatus testCopy(CFDictionaryRef q, CFTypeRef *out)
{
    NSString *name = beforeCall(q, @"SecItemCopyMatching");
    NSDictionary *query = (__bridge NSDictionary *)q;
    assert(!query[(__bridge id)kSecUseAuthenticationContext]);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    assert([query[(__bridge id)kSecUseAuthenticationUI] isEqual:(__bridge id)kSecUseAuthenticationUIFail]);
#pragma clang diagnostic pop
    if (rejectCopy) return errSecAuthFailed;
    if (!items[name]) return errSecItemNotFound;
    *out = CFBridgingRetain(items[name]); return errSecSuccess;
}
CFErrorRef testLastError(OSStatus status)
{
    if (!rejectCopy || status != errSecAuthFailed) return NULL;
    const void *keys[] = { kCFErrorDescriptionKey };
    const void *values[] = { CFSTR("Unable to get externalizedContext from LAContext") };
    CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFErrorRef error = CFErrorCreate(NULL, kCFErrorDomainOSStatus, status, info);
    CFRelease(info);
    return error;
}
static OSStatus testUpdate(CFDictionaryRef q, CFDictionaryRef a)
{
    NSString *name = beforeCall(q, @"SecItemUpdate");
    if (!items[name]) return errSecItemNotFound;
    items[name] = ((__bridge NSDictionary *)a)[(__bridge id)kSecValueData]; return errSecSuccess;
}
static OSStatus testDelete(CFDictionaryRef q)
{
    NSString *name = beforeCall(q, @"SecItemDelete");
    if (!items[name]) return errSecItemNotFound;
    [items removeObjectForKey:name]; return errSecSuccess;
}
static NSDictionary *perform(NSString *op)
{
    calls = 0; observations = [NSMutableArray array];
    NSDictionary *value = AA27PerformProbe(op, ^(NSDictionary *v) { [observations addObject:v]; });
    assert([value[@"run_state"] isEqual:@"completed"]);
    assert([saved()[@"exit_code"] isEqual:value[@"exit_code"]]);
    assert([observations.firstObject[@"steps"] count] == 0); // retained snapshots cannot change
    return value;
}
int main(void)
{
    @autoreleasepool {
        testCache = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        testBoot = @"00000000-0000-0000-0000-000000000001";
        items = [NSMutableDictionary dictionary];
        NSDictionary *v = perform(@"auto");
        assert([v[@"exit_code"] isEqual:@0] && [v[@"phase"] isEqual:@"prepared"] && calls == 7);
        v = perform(@"auto");
        assert([v[@"phase"] isEqual:@"same_boot_only"] && calls == 1);
        testBoot = @"00000000-0000-0000-0000-000000000002";
        v = perform(@"auto");
        assert([v[@"phase"] isEqual:@"cross_boot_verified"] && calls == 1);
        v = perform(@"cleanup");
        assert([v[@"exit_code"] isEqual:@0] && calls == 2 && items.count == 0);
        rejectAdd = YES;
        v = perform(@"auto");
        assert([v[@"exit_code"] isEqual:@1] && calls == 1 && [v[@"steps"] count] == 1);
        assert([v[@"steps"][0][@"status"] intValue] == errSecInteractionNotAllowed);
        assert(![v[@"ordinary_operations"] boolValue]);
        rejectAdd = NO;
        rejectCopy = YES;
        v = perform(@"auto");
        assert([v[@"exit_code"] isEqual:@1] && calls == 2 && [v[@"steps"] count] == 2);
        NSDictionary *failed = v[@"steps"][1];
        assert([failed[@"step"] isEqual:@"copy_initial"] && [failed[@"status"] intValue] == errSecAuthFailed);
        assert([failed[@"error_domain"] isEqual:(__bridge NSString *)kCFErrorDomainOSStatus]);
        assert([failed[@"error_code"] intValue] == errSecAuthFailed);
        assert([failed[@"error_phase"] isEqual:@"client_context_export"]);
        assert(!failed[@"description"] && !failed[@"user_info"]);
        assert(![v[@"ordinary_operations"] boolValue] && items.count == 1);
        assert([NSFileManager.defaultManager removeItemAtPath:testCache error:NULL]);
        puts("PASS: production checkpoints, immutable UI snapshots, prepare/same/cross-boot, cleanup, noninteractive query and original error metadata, failure preserves partial item; simulated K1 only");
    }
    return 0;
}
