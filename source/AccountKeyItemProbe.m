// AA27 ordinary SecItem qualification. Values are public test canaries.
// Only this executable's explicit test access group and two item names are used.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <LocalAuthentication/LocalAuthentication.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static NSString *const group = @"org.unixnu.nezha.account27.test";
static NSString *const service = @"org.unixnu.nezha.account27.SecItem.v1";
static NSString *directory;
static NSMutableDictionary *result;
static NSMutableArray *steps;

static BOOL status(NSString *name, OSStatus code)
{
    [steps addObject:@{@"step": name, @"status": @(code)}];
    return code == errSecSuccess;
}

static NSMutableDictionary *query(NSString *account)
{
    return [@{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
              (__bridge id)kSecAttrService: service,
              (__bridge id)kSecAttrAccount: account,
              (__bridge id)kSecAttrAccessGroup: group,
              (__bridge id)kSecAttrSynchronizable: @NO} mutableCopy];
}

static NSData *canary(unsigned revision)
{
    uint8_t value[32];
    for (unsigned i = 0; i < sizeof(value); ++i) value[i] = (uint8_t)(revision * 31 + i);
    return [NSData dataWithBytes:value length:sizeof(value)];
}

static OSStatus add(NSString *account, unsigned revision)
{
    NSMutableDictionary *q = query(account);
    q[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    q[(__bridge id)kSecValueData] = canary(revision);
    return SecItemAdd((__bridge CFDictionaryRef)q, NULL);
}

static OSStatus copy(NSString *account, unsigned revision)
{
    NSMutableDictionary *q = query(account);
    q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    q[(__bridge id)kSecReturnData] = @YES;
    LAContext *context = [LAContext new];
    context.interactionNotAllowed = YES;
    q[(__bridge id)kSecUseAuthenticationContext] = context;
    CFTypeRef found = NULL;
    OSStatus rc = SecItemCopyMatching((__bridge CFDictionaryRef)q, &found);
    if (rc == errSecSuccess && (!found || CFGetTypeID(found) != CFDataGetTypeID() ||
                               ![(__bridge NSData *)found isEqualToData:canary(revision)])) rc = errSecDecode;
    if (found) CFRelease(found);
    return rc;
}

static OSStatus update(unsigned revision)
{
    NSDictionary *attributes = @{(__bridge id)kSecValueData: canary(revision)};
    return SecItemUpdate((__bridge CFDictionaryRef)query(@"canary"), (__bridge CFDictionaryRef)attributes);
}

static OSStatus removeItem(NSString *account)
{
    return SecItemDelete((__bridge CFDictionaryRef)query(account));
}

static NSString *bootSession(void)
{
    char value[64] = {0};
    size_t bytes = sizeof(value);
    if (sysctlbyname("kern.bootsessionuuid", value, &bytes, NULL, 0) != 0 ||
        bytes == 0 || bytes > sizeof(value) || !memchr(value, 0, bytes)) return nil;
    NSString *text = [NSString stringWithUTF8String:value];
    NSUUID *uuid = text ? [[NSUUID alloc] initWithUUIDString:text] : nil;
    return uuid.UUIDString;
}

static BOOL saveJSON(NSDictionary *value, NSString *path)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingPrettyPrinted error:NULL];
    return data && [data writeToFile:path options:NSDataWritingAtomic error:NULL];
}

static int operate(NSString *operation)
{
    if (getuid() != 501) {
        result[@"reason"] = @"requires_mobile_user_context";
        return 1;
    }
    NSString *boot = bootSession();
    if (!boot) {
        result[@"reason"] = @"native_boot_session_not_available";
        return 1;
    }
    result[@"boot_session"] = boot;
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm createDirectoryAtPath:directory withIntermediateDirectories:YES
                       attributes:@{NSFilePosixPermissions: @0700} error:NULL]) {
        result[@"reason"] = @"test_directory_unavailable";
        return 1;
    }
    NSString *statePath = [directory stringByAppendingPathComponent:@"state.json"];
    if ([operation isEqualToString:@"cleanup"]) {
        for (NSString *name in @[@"canary", @"scratch"]) {
            OSStatus rc = removeItem(name);
            [steps addObject:@{@"step": [@"cleanup_" stringByAppendingString:name], @"status": @(rc)}];
            if (rc != errSecSuccess && rc != errSecItemNotFound) return 1;
        }
        if ([fm fileExistsAtPath:statePath] && ![fm removeItemAtPath:statePath error:NULL]) return 1;
        result[@"cleanup_complete"] = @YES;
        return 0;
    }
    if (![operation isEqualToString:@"auto"]) {
        result[@"reason"] = @"unknown_operation";
        return 2;
    }
    if ([fm fileExistsAtPath:statePath]) {
        NSData *encoded = [NSData dataWithContentsOfFile:statePath];
        id state = encoded ? [NSJSONSerialization JSONObjectWithData:encoded options:0 error:NULL] : nil;
        if (![state isKindOfClass:NSDictionary.class] || ![state[@"schema"] isEqual:@1] ||
            ![state[@"phase"] isEqual:@"prepared"] ||
            ![state[@"boot_session"] isKindOfClass:NSString.class] ||
            ![[NSUUID alloc] initWithUUIDString:state[@"boot_session"]]) {
            result[@"reason"] = @"test_state_invalid_preserved";
            return 1;
        }
        if (!status(@"copy_persisted", copy(@"canary", 2))) return 1;
        result[@"read_matches_public_canary"] = @YES;
        result[@"cross_boot_read"] = @(![boot isEqualToString:state[@"boot_session"]]);
        result[@"phase"] = [result[@"cross_boot_read"] boolValue] ? @"cross_boot_verified" : @"same_boot_only";
        return 0;
    }
    // No broad query or cleanup is performed before creation. A duplicate or
    // partial earlier attempt remains visible as an actual SecItem result.
    if (!status(@"add_canary", add(@"canary", 1)) ||
        !status(@"copy_initial", copy(@"canary", 1)) ||
        !status(@"update_canary", update(2)) ||
        !status(@"copy_updated", copy(@"canary", 2)) ||
        !status(@"add_scratch", add(@"scratch", 1)) ||
        !status(@"delete_scratch", removeItem(@"scratch"))) return 1;
    OSStatus absent = copy(@"scratch", 1);
    [steps addObject:@{@"step": @"copy_deleted", @"status": @(absent)}];
    if (absent != errSecItemNotFound) return 1;
    if (!saveJSON(@{@"schema": @1, @"phase": @"prepared", @"boot_session": boot}, statePath)) {
        result[@"reason"] = @"test_state_write_failed";
        return 1;
    }
    result[@"phase"] = @"prepared";
    result[@"ordinary_operations"] = @YES;
    return 0;
}

// Called only on the app's serial worker. All Security queries and public
// canary values above are the previously built probe002 implementation.
NSDictionary *AA27PerformProbe(NSString *operation)
{
    @autoreleasepool {
        NSString *cache = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
        if (!cache) return @{ @"exit_code": @1, @"reason": @"application_cache_unavailable" };
        directory = [cache stringByAppendingPathComponent:@"org.unixnu.nezha.account27.test"];
        steps = [NSMutableArray array];
        result = [@{@"schema": @1, @"operation": operation, @"uid": @(getuid()), @"pid": @(getpid()),
                    @"steps": steps, @"ordinary_operations": @NO, @"cross_boot_read": @NO,
                    @"remote_account_tested": @NO, @"private_values_logged": @NO} mutableCopy];
        int rc = operate(operation);
        result[@"exit_code"] = @(rc);
        if (getuid() == 501) {
            BOOL written = saveJSON(result, [directory stringByAppendingPathComponent:@"result.json"]);
            result[@"result_file_written"] = @(written);
            if (!written) result[@"exit_code"] = @(rc ? rc : 1);
        }
        return [result copy];
    }
}
