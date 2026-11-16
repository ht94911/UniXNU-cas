/* SPDX-License-Identifier: MIT */
/* Use the original LaunchServices request for NezhaLocation.app. Original24A446
 * registerApplicationForRebuildWithInstallInfo: is the current entry for a
 * system bundle missing from the current Data's application registration.
 * Registration is restricted to the existing /Applications/NezhaLocation.app;
 * no bundle, executable, signature, user app or root volume is rewritten. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

@interface NSObject (NezhaCameraLaunchServices)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (NSURL *)bundleURL;
- (NSString *)applicationIdentifier;
- (BOOL)isInstalled;
- (BOOL)isPlaceholder;
- (BOOL)registerApplicationForRebuildWithInfoDictionaries:(NSArray *)dictionaries
    requestContext:(id)context registrationError:(NSError **)error;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
- (void)setTargetUserID:(NSNumber *)uid;
@end

static BOOL registered_camera(id application)
{
    return [application respondsToSelector:@selector(applicationIdentifier)] &&
        [[application applicationIdentifier] isEqualToString:@"org.unixnu.nezha.location"] &&
        [application respondsToSelector:@selector(isInstalled)] && [application isInstalled] &&
        [application respondsToSelector:@selector(isPlaceholder)] && ![application isPlaceholder];
}

static void report_proxy(const char *step,id application)
{
    NSString *identifier=[application respondsToSelector:@selector(applicationIdentifier)] ?
        [application applicationIdentifier]:nil;
    NSURL *url=[application respondsToSelector:@selector(bundleURL)] ? [application bundleURL]:nil;
    fprintf(stderr,"nezha-location-launch: %s proxy=%u identifier=%s installed=%u placeholder=%u path=%s\n",
        step,(unsigned)(application!=nil),identifier.UTF8String ?: "<absent>",
        (unsigned)([application respondsToSelector:@selector(isInstalled)] && [application isInstalled]),
        (unsigned)([application respondsToSelector:@selector(isPlaceholder)] && [application isPlaceholder]),
        url.path.UTF8String ?: "<absent>");
}

/* Tests exercise this exact consumer with Foundation and fixture LS classes.
 * Runtime supplies only the measured original Camera path below. */
static int open_camera(Class proxy,id service,NSURL *system_url,Class request_context_class)
{
    NSString *identifier=@"org.unixnu.nezha.location";
    id application=[proxy applicationProxyForIdentifier:identifier];
    report_proxy("before",application);
    if (!registered_camera(application)) {
        NSError *error=nil;
        NSURL *info_url=[system_url URLByAppendingPathComponent:@"Info.plist"];
        NSData *data=[NSData dataWithContentsOfURL:info_url options:0 error:&error];
        NSDictionary *info=data ? [NSPropertyListSerialization propertyListWithData:data
            options:NSPropertyListImmutable format:NULL error:&error]:nil;
        if (![info isKindOfClass:[NSDictionary class]] ||
            ![info[@"CFBundleIdentifier"] isEqualToString:identifier] ||
            ![info[@"CFBundleExecutable"] isEqualToString:@"NezhaLocation"] ||
            access([system_url URLByAppendingPathComponent:@"NezhaLocation"].fileSystemRepresentation,R_OK)) {
            fprintf(stderr,"nezha-location-launch: system Camera identity unavailable error=%ld path=%s\n",
                (long)error.code,system_url.path.UTF8String);
            return 4;
        }
        /* The native synchronous return is followed by a fresh proxy query.
         * Do not equate queued registration or placeholder presence with an
         * installed app; no private replacement identity is synthesized. */
        /* Reuse UIKitTools' original System bundle description through
         * the current24A446 LSInstallInfo conversion. The old dictionary
         * endpoint returns false unconditionally in this actual OS. */
        if (![service respondsToSelector:
            @selector(registerApplicationForRebuildWithInfoDictionaries:requestContext:registrationError:)]) return 5;
        /* Actual24A446 _requestContextForUserDomain: selects its original
         * systemSessionDomain when targetUserID is @0. A nil context picks
         * the current mobile user domain, where072's com.apple.lsd.rebuild
         * lookup returned NoSuchProcess. The caller keeps its original uid
         * and rebuild entitlement; only the LS request domain changes. */
        id context=[request_context_class new];
        if (![context respondsToSelector:@selector(setTargetUserID:)]) {
            fputs("nezha-location-launch: original LS request context unavailable\n",stderr);
            return 5;
        }
        [context setTargetUserID:@0];
        NSDictionary *registration=@{@"ApplicationType":@"System",
            @"BundleNameIsLocalized":@1,@"CFBundleIdentifier":identifier,
            @"CompatibilityState":@0,@"IsDeletable":@0,@"Path":system_url.path};
        error=nil;
        const BOOL registered=[service registerApplicationForRebuildWithInfoDictionaries:@[registration]
            requestContext:context registrationError:&error];
        fprintf(stderr,"nezha-location-launch: original system Camera registration=%u error=%ld domain=%s path=%s\n",
            (unsigned)registered,(long)error.code,error.domain.UTF8String ?: "<none>",system_url.path.UTF8String);
        if (!registered && error) {
            /* NSCocoaError4099 alone loses the service name and the native
             * interruption reason. Preserve the actual XPC diagnostic for
             * this one registration; do not resubmit a failed request. */
            fprintf(stderr,"nezha-location-launch: registration-detail=%s\n",
                error.description.UTF8String ?: "<absent>");
            NSError *underlying=error.userInfo[NSUnderlyingErrorKey];
            if ([underlying isKindOfClass:[NSError class]])
                fprintf(stderr,"nezha-location-launch: registration-underlying=%s\n",
                    underlying.description.UTF8String ?: "<absent>");
        }
        if (!registered) return 6;
        application=[proxy applicationProxyForIdentifier:identifier];
        report_proxy("after",application);
        if (!registered_camera(application)) return 7;
    }
    if (![service respondsToSelector:@selector(openApplicationWithBundleID:)]) {
        fputs("nezha-location-launch: original launch method unavailable\n",stderr);
        return 8;
    }
    const BOOL requested=[service openApplicationWithBundleID:identifier];
    fprintf(stderr,"nezha-location-launch: originalCamera registered=1 launch_request=%u preview_verified=0 photo_verified=0\n",(unsigned)requested);
    return requested ? 0:9;
}

#ifndef NEZHA_CAMERA_LAUNCH_TEST
static void deadline(int value)
{
    (void)value;
    static const char text[]="nezha-location-launch: bounded LaunchServices deadline\n";
    (void)write(STDERR_FILENO,text,sizeof(text)-1);
    _exit(124);
}

int main(void)
{
    @autoreleasepool {
        setvbuf(stderr,NULL,_IONBF,0);
        signal(SIGALRM,deadline);
        alarm(45);
        const char *framework="/System/Library/Frameworks/CoreServices.framework/CoreServices";
        void *handle=dlopen(framework,RTLD_NOW|RTLD_LOCAL);
        Class proxy=objc_getClass("LSApplicationProxy");
        Class workspace=objc_getClass("LSApplicationWorkspace");
        Class request_context=objc_getClass("LSOperationRequestContext");
        if (!handle || !proxy || !workspace || !request_context ||
            ![proxy respondsToSelector:@selector(applicationProxyForIdentifier:)] ||
            ![workspace respondsToSelector:@selector(defaultWorkspace)]) {
            fputs("nezha-location-launch: original LaunchServices interface unavailable\n",stderr);
            return 3;
        }
        return open_camera(proxy,[workspace defaultWorkspace],
            [NSURL fileURLWithPath:@"/Applications/NezhaLocation.app" isDirectory:YES],request_context);
    }
}
#endif
