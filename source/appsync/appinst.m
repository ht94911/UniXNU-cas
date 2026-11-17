// SPDX-License-Identifier: GPL-3.0-or-later
// Installs through the native coordinator on iOS 27.
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import <objc/runtime.h>
#import <dlfcn.h>

@interface NSObject (UniXNUInstallOptions)
- (void)setInstallTargetType:(NSUInteger)type;
- (void)setBundleIdentifier:(NSString *)identifier;
@end

@interface NSObject (UniXNUInstallCoordinator)
+ (void)installApplication:(NSURL *)url
    forPersonaUniqueString:(NSString *)persona
             consumeSource:(BOOL)consume
                   options:(id)options
                completion:(void (^)(id identity, NSError *error))completion;
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

static int installFile(NSString *path, NSString *identifier, BOOL consume)
{
    const char *framework = "/System/Library/PrivateFrameworks/InstallCoordination.framework/InstallCoordination";
    if (!dlopen(framework, RTLD_NOW)) {
        unixnu_appsync_probe_error("load-install-coordinator", nil);
        return 3;
    }
    Class coordinator = objc_getClass("IXAppInstallCoordinator");
    Class optionsClass = objc_getClass("MIInstallOptions");
    if (![coordinator respondsToSelector:@selector(installApplication:forPersonaUniqueString:consumeSource:options:completion:)]
        || ![optionsClass instancesRespondToSelector:@selector(setInstallTargetType:)]
        || (identifier && ![optionsClass instancesRespondToSelector:@selector(setBundleIdentifier:)])) {
        unixnu_appsync_probe_error("resolve-install-interface", nil);
        return 3;
    }
    BOOL installed = NO;
    @try {
        id options = [optionsClass new];
        [options setInstallTargetType:1];
        if (identifier) [options setBundleIdentifier:identifier];

        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        __block BOOL accepted = NO;
        __block NSError *installError = nil;
        [coordinator installApplication:[NSURL fileURLWithPath:path]
                 forPersonaUniqueString:nil consumeSource:consume options:options
                              completion:^(id identity, NSError *failure) {
            installError = failure;
            accepted = identity != nil && failure == nil;
            dispatch_semaphore_signal(finished);
        }];
        dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
        installed = accepted;
        if (!installed) unixnu_appsync_probe_error("install-coordination", installError);
    } @catch (NSException *exception) {
#ifdef UNIXNU_APPSYNC_PROBE
        unixnu_appsync_probe_exception(exception);
#else
        NSLog(@"Install exception: %@", exception);
#endif
    }
    return installed ? 0 : 1;
}

#ifdef UNIXNU_APPSYNC_PROBE
int unixnu_appinst_install_received(NSString *path, NSString *identifier)
{
    // The transfer task owns this temporary file and can let iOS consume it.
    return installFile(path, identifier, YES);
}
#endif

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 && argc != 3) {
            fprintf(stderr, "usage: unixnu-appinst [BUNDLE_ID] FILE.ipa\n");
            return 2;
        }
        NSFileManager *files = NSFileManager.defaultManager;
        NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSError *error = nil;
        if (![files createDirectoryAtPath:directory withIntermediateDirectories:NO
            attributes:@{NSFilePosixPermissions: @0755} error:&error]) {
            unixnu_appsync_probe_error("create-staging-directory", error);
            return 4;
        }
        NSString *copy = [directory stringByAppendingPathComponent:@"install.ipa"];
        @try {
            if (![files copyItemAtPath:@(argv[argc - 1]) toPath:copy error:&error]) {
                unixnu_appsync_probe_error("copy-ipa", error);
                return 1;
            }
            return installFile(copy, argc == 3 ? @(argv[1]) : nil, NO);
        } @finally {
            [files removeItemAtPath:directory error:NULL];
        }
    }
}
