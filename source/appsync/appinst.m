// SPDX-License-Identifier: GPL-3.0-or-later
// Uses AppSync appinst's LSApplicationWorkspace install entry point.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>

@interface NSObject (UniXNUInstall)
+ (id)defaultWorkspace;
- (BOOL)installApplication:(NSURL *)url withOptions:(NSDictionary *)options error:(NSError **)error;
@end

#ifdef UNIXNU_APPSYNC_PROBE
extern void unixnu_appsync_probe_error(const char *stage, NSError *error);
extern void unixnu_appsync_probe_exception(NSException *exception);
#else
static void unixnu_appsync_probe_error(const char *stage, NSError *error)
{
    NSLog(@"%s failed: %@", stage, error);
}
#endif

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 && argc != 3) {
            fprintf(stderr, "usage: unixnu-appinst [BUNDLE_ID] FILE.ipa\n");
            return 2;
        }
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
        Class cls = objc_getClass("LSApplicationWorkspace");
        if (![cls respondsToSelector:@selector(defaultWorkspace)]) return 3;
        id workspace = [cls defaultWorkspace];
        if (![workspace respondsToSelector:@selector(installApplication:withOptions:error:)]) return 3;
        NSFileManager *files = NSFileManager.defaultManager;
        NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSError *error = nil;
        if (![files createDirectoryAtPath:directory withIntermediateDirectories:NO
            attributes:@{NSFilePosixPermissions: @0755} error:&error]) {
            unixnu_appsync_probe_error("create-staging-directory", error);
            return 4;
        }
        NSString *copy = [directory stringByAppendingPathComponent:@"install.ipa"];
        BOOL installed = NO;
        @try {
            // MobileInstallation may consume the input. Keep the caller's IPA.
            if ([files copyItemAtPath:@(argv[argc - 1]) toPath:copy error:&error]) {
                installed = [workspace installApplication:[NSURL fileURLWithPath:copy]
                    withOptions:argc == 3 ? @{@"CFBundleIdentifier": @(argv[1]), @"ApplicationType": @"User"} :
                        @{@"ApplicationType": @"User"} error:&error];
                if (!installed) unixnu_appsync_probe_error("install-application", error);
            } else {
                unixnu_appsync_probe_error("copy-ipa", error);
            }
        } @catch (NSException *exception) {
#ifdef UNIXNU_APPSYNC_PROBE
            unixnu_appsync_probe_exception(exception);
#else
            NSLog(@"Install exception: %@", exception);
#endif
        } @finally {
            [files removeItemAtPath:directory error:NULL];
        }
        return installed ? 0 : 1;
    }
}
