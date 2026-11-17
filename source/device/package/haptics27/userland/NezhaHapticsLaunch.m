/* SPDX-License-Identifier: MIT */
/* Registration consumer reused from Camera071 current original LaunchServices API. */
/* Use the original LaunchServices request for NezhaHaptics.app. Original24A446
 * registerApplicationForRebuildWithInstallInfo: is the current entry for a
 * system bundle missing from the current Data's application registration.
 * The app arrives through this candidate's readonly addon view and owns its
 * normal app container. The original LaunchServices request registers it. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>

@interface NSObject (NezhaHapticLaunchServices)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (NSURL *)bundleURL;
- (NSString *)applicationIdentifier;
- (BOOL)isInstalled;
- (BOOL)isPlaceholder;
- (BOOL)registerApplicationForRebuildWithInfoDictionaries:(NSArray *)dictionaries
    requestContext:(id)context registrationError:(NSError **)error;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
@end

static BOOL registered_haptic(id application)
{
    return [application respondsToSelector:@selector(applicationIdentifier)] &&
        [[application applicationIdentifier] isEqualToString:@"org.unixnu.nezha.haptics27.test"] &&
        [application respondsToSelector:@selector(isInstalled)] && [application isInstalled] &&
        [application respondsToSelector:@selector(isPlaceholder)] && ![application isPlaceholder];
}

static void report_proxy(const char *step,id application)
{
    NSString *identifier=[application respondsToSelector:@selector(applicationIdentifier)] ?
        [application applicationIdentifier]:nil;
    NSURL *url=[application respondsToSelector:@selector(bundleURL)] ? [application bundleURL]:nil;
    fprintf(stderr,"nezha-haptics-launch: %s proxy=%u identifier=%s installed=%u placeholder=%u path=%s\n",
        step,(unsigned)(application!=nil),identifier.UTF8String ?: "<absent>",
        (unsigned)([application respondsToSelector:@selector(isInstalled)] && [application isInstalled]),
        (unsigned)([application respondsToSelector:@selector(isPlaceholder)] && [application isPlaceholder]),
        url.path.UTF8String ?: "<absent>");
}

/* Tests exercise this exact consumer with Foundation and fixture LS classes.
 * Runtime supplies only the measured original Camera path below. */
static int open_haptic(Class proxy,id service,NSURL *system_url)
{
    NSString *identifier=@"org.unixnu.nezha.haptics27.test";
    id application=[proxy applicationProxyForIdentifier:identifier];
    report_proxy("before",application);
    if (!registered_haptic(application)) {
        NSError *error=nil;
        NSURL *info_url=[system_url URLByAppendingPathComponent:@"Info.plist"];
        NSData *data=[NSData dataWithContentsOfURL:info_url options:0 error:&error];
        NSDictionary *info=data ? [NSPropertyListSerialization propertyListWithData:data
            options:NSPropertyListImmutable format:NULL error:&error]:nil;
        if (![info isKindOfClass:[NSDictionary class]] ||
            ![info[@"CFBundleIdentifier"] isEqualToString:identifier] ||
            ![info[@"CFBundleExecutable"] isEqualToString:@"NezhaHapticsUI"] ||
            access([system_url URLByAppendingPathComponent:@"NezhaHapticsUI"].fileSystemRepresentation,R_OK)) {
            fprintf(stderr,"nezha-haptics-launch: haptic app identity unavailable error=%ld path=%s\n",
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
        NSDictionary *registration=@{@"ApplicationType":@"System",
            @"BundleNameIsLocalized":@1,@"CFBundleIdentifier":identifier,
            @"CompatibilityState":@0,@"IsDeletable":@0,@"Path":system_url.path};
        error=nil;
        const BOOL registered=[service registerApplicationForRebuildWithInfoDictionaries:@[registration]
            requestContext:nil registrationError:&error];
        fprintf(stderr,"nezha-haptics-launch: original haptic app registration=%u error=%ld domain=%s path=%s\n",
            (unsigned)registered,(long)error.code,error.domain.UTF8String ?: "<none>",system_url.path.UTF8String);
        if (!registered) return 6;
        application=[proxy applicationProxyForIdentifier:identifier];
        report_proxy("after",application);
        if (!registered_haptic(application)) return 7;
    }
    if (![service respondsToSelector:@selector(openApplicationWithBundleID:)]) {
        fputs("nezha-haptics-launch: original launch method unavailable\n",stderr);
        return 8;
    }
    const BOOL requested=[service openApplicationWithBundleID:identifier];
    fprintf(stderr,"nezha-haptics-launch: haptic app registered=1 launch_request=%u UIKit_haptics_verified=0 CoreHaptics_verified=0\n",(unsigned)requested);
    return requested ? 0:9;
}

/* Inspect the exact directories the stock HAL loader scans, before opening
 * the normal application. Preflight validates the bundle but does not run its
 * factory or replace the audio server's discovery and initialization. */
static void inspect_hal_bundle(void)
{
    const char *directory="/System/Library/Audio/Plug-Ins/HAL";
    DIR *stream=opendir(directory);unsigned count=0,found=0;
    if(stream) {
        struct dirent *entry;
        while((entry=readdir(stream)) && count<128) {
            if(entry->d_name[0]=='.')continue;
            ++count;if(!strcmp(entry->d_name,"NezhaHaptics27.driver"))found=1;
        }
        closedir(stream);
    }
    fprintf(stderr,"nezha-haptics-launch: HAL directory=%s entries=%u expected=%u\n",directory,count,found);
    NSArray *paths=@[@"/System/Library/Audio/Plug-Ins/HAL/NezhaHaptics27.driver",
        @"/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin"];
    for(NSString *path in paths) {
        NSURL *url=[NSURL fileURLWithPath:path isDirectory:YES];
        CFBundleRef bundle=CFBundleCreate(NULL,(__bridge CFURLRef)url);
        CFErrorRef error=NULL;Boolean preflight=bundle?CFBundlePreflightExecutable(bundle,&error):false;
        NSString *message=error?[(__bridge NSError *)error localizedDescription]:@"";
        fprintf(stderr,"nezha-haptics-launch: HAL path=%s bundle=%u preflight=%u error=%ld description=%s\n",
            path.UTF8String,(unsigned)(bundle!=NULL),(unsigned)preflight,
            error?(long)CFErrorGetCode(error):0,message.UTF8String);
        if(error)CFRelease(error);if(bundle)CFRelease(bundle);
    }
}

#ifndef NEZHA_CAMERA_LAUNCH_TEST
static void deadline(int value)
{
    (void)value;
    static const char text[]="nezha-haptics-launch: bounded LaunchServices deadline\n";
    (void)write(STDERR_FILENO,text,sizeof(text)-1);
    _exit(124);
}

int main(void)
{
    @autoreleasepool {
        setvbuf(stderr,NULL,_IONBF,0);
        signal(SIGALRM,deadline);
        alarm(45);
        inspect_hal_bundle();
        const char *framework="/System/Library/Frameworks/CoreServices.framework/CoreServices";
        void *handle=dlopen(framework,RTLD_NOW|RTLD_LOCAL);
        Class proxy=objc_getClass("LSApplicationProxy");
        Class workspace=objc_getClass("LSApplicationWorkspace");
        if (!handle || !proxy || !workspace ||
            ![proxy respondsToSelector:@selector(applicationProxyForIdentifier:)] ||
            ![workspace respondsToSelector:@selector(defaultWorkspace)]) {
            fputs("nezha-haptics-launch: original LaunchServices interface unavailable\n",stderr);
            return 3;
        }
        return open_haptic(proxy,[workspace defaultWorkspace],
            [NSURL fileURLWithPath:@"/Applications/NezhaHapticsUI.app" isDirectory:YES]);
    }
}
#endif
