/* SPDX-License-Identifier: MIT */
/* Use the original LaunchServices request for NezhaLocation.app. Original24A446
 * registerContainerizedApplicationWithInfoDictionaries: registers a
 * system bundle missing from the current Data's application registration.
 * Registration is restricted to the existing /Applications/NezhaLocation.app;
 * no bundle, executable, signature, user app or root volume is rewritten. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <unistd.h>

@interface NSObject (NezhaLocationLaunchServices)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (NSURL *)bundleURL;
- (NSString *)applicationIdentifier;
- (BOOL)isInstalled;
- (BOOL)isPlaceholder;
- (id)registerContainerizedApplicationWithInfoDictionaries:(NSArray *)dictionaries
    operationUUID:(NSUUID *)operation requestContext:(id)context saveObserver:(id)observer
    registrationError:(NSError **)error;
- (void)setTargetUserID:(NSNumber *)uid;
+ (id)serviceWithDefaultShellEndpoint;
+ (id)optionsWithDictionary:(NSDictionary *)options;
- (void)openApplication:(NSString *)identifier withOptions:(id)options
    completion:(void (^)(id process,NSError *error))completion;
- (int)pid;
- (BOOL)isValid;
@end

static BOOL registered_location(id application)
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

static int location_output(const char *path)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0) return 10;
    if(dup2(fd,STDERR_FILENO)<0) {
        close(fd);
        return 10;
    }
    close(fd);
    return 0;
}

/* Original LaunchServices forwards to this same default-shell service.
 * GNSS008's error identified the intermediate lsd springBoardQueue timeout.
 * Submit one ordinary request and retain the actual process/error callback. */
static int request_location(Class serviceClass,Class optionsClass,NSString *identifier,dispatch_time_t end)
{
    if (![serviceClass respondsToSelector:@selector(serviceWithDefaultShellEndpoint)] ||
        ![optionsClass respondsToSelector:@selector(optionsWithDictionary:)]) {
        fputs("nezha-location-launch: original shell launch service unavailable\n",stderr);
        return 8;
    }
    id service=[serviceClass serviceWithDefaultShellEndpoint];
    id options=[optionsClass optionsWithDictionary:@{}];
    if (!options || ![service respondsToSelector:@selector(openApplication:withOptions:completion:)]) {
        fputs("nezha-location-launch: original shell launch endpoint/options unavailable\n",stderr);
        return 8;
    }
    dispatch_semaphore_t completion=dispatch_semaphore_create(0);
    __block id process=nil;
    __block NSError *launch_error=nil;
    fputs("nezha-location-launch: NezhaLocation registered=1 launch_submitted=1 method=FBSOpenApplicationService/openApplication:withOptions:completion:\n",stderr);
    [service openApplication:identifier withOptions:options
        completion:^(id handle,NSError *error) {
            process=handle;
            launch_error=error;
            dispatch_semaphore_signal(completion);
        }];
    if (dispatch_semaphore_wait(completion,end)) {
        fputs("nezha-location-launch: NezhaLocation registered=1 launch_request=0 launch_completed=0 completion_timeout=1 CoreLocation_callback_verified=0\n",stderr);
        return 11;
    }
    const int pid=[process respondsToSelector:@selector(pid)] ? [process pid]:0;
    const BOOL valid=[process respondsToSelector:@selector(isValid)] && [process isValid];
    const BOOL requested=pid>0 && valid && launch_error==nil;
    fprintf(stderr,"nezha-location-launch: NezhaLocation registered=1 launch_request=%u launch_completed=1 process_handle=%u process_pid=%d process_valid=%u error=%ld domain=%s CoreLocation_callback_verified=0\n",
        (unsigned)requested,(unsigned)(process!=nil),pid,(unsigned)valid,(long)launch_error.code,
        launch_error.domain.UTF8String ?: "<none>");
    if (launch_error) {
        fprintf(stderr,"nezha-location-launch: launch-detail=%s\n",
            launch_error.description.UTF8String ?: "<absent>");
        NSError *underlying=launch_error.userInfo[NSUnderlyingErrorKey];
        if ([underlying isKindOfClass:[NSError class]])
            fprintf(stderr,"nezha-location-launch: launch-underlying=%s\n",
                underlying.description.UTF8String ?: "<absent>");
    }
    return requested ? 0:9;
}

/* Tests exercise this exact consumer with Foundation and fixture LS classes.
 * Runtime supplies only the selected NezhaLocation bundle path below. */
static int open_location(Class proxy,id service,NSURL *system_url)
{
    NSString *identifier=@"org.unixnu.nezha.location";
    id application=[proxy applicationProxyForIdentifier:identifier];
    report_proxy("before",application);
    if (!registered_location(application)) {
        NSError *error=nil;
        NSURL *info_url=[system_url URLByAppendingPathComponent:@"Info.plist"];
        NSData *data=[NSData dataWithContentsOfURL:info_url options:0 error:&error];
        NSDictionary *info=data ? [NSPropertyListSerialization propertyListWithData:data
            options:NSPropertyListImmutable format:NULL error:&error]:nil;
        if (![info isKindOfClass:[NSDictionary class]] ||
            ![info[@"CFBundleIdentifier"] isEqualToString:identifier] ||
            ![info[@"CFBundleExecutable"] isEqualToString:@"NezhaLocation"] ||
            access([system_url URLByAppendingPathComponent:@"NezhaLocation"].fileSystemRepresentation,R_OK)) {
            fprintf(stderr,"nezha-location-launch: system NezhaLocation identity unavailable error=%ld path=%s\n",
                (long)error.code,system_url.path.UTF8String);
            return 4;
        }
        /* The native synchronous return is followed by a fresh proxy query.
         * Do not equate queued registration or placeholder presence with an
         * installed app; no private replacement identity is synthesized. */
        /* The old rebuild endpoints were absent from the actual072/073
         * bootstrap domains. Current24A446 converts this same System
         * dictionary to LSInstallInfo, then performPostInstallationRegistration
         * on the existing modifydb service. This only registers the existing
         * NezhaLocation bundle through its real Info.plist; it does not change location authorization. */
        if (![service respondsToSelector:
            @selector(registerContainerizedApplicationWithInfoDictionaries:operationUUID:requestContext:saveObserver:registrationError:)]) return 5;
        NSDictionary *registration=@{@"ApplicationType":@"System",
            @"BundleNameIsLocalized":@1,@"CFBundleIdentifier":identifier,
            @"CompatibilityState":@0,@"IsDeletable":@0,@"Path":system_url.path};
        /* Original24A446 nil context resolves the current root proxy UID.
         * This job registers an app for the existing mobile lsd service;
         * targetUserID selects the original user-domain connection instead
         * of looking for a root-domain modifydb endpoint that is absent. */
        Class contextClass=objc_getClass("LSOperationRequestContext");
        id context=contextClass ? [contextClass new] : nil;
        if(!context || ![context respondsToSelector:@selector(setTargetUserID:)]) {
            fputs("nezha-location-launch: original user request context unavailable\n",stderr);
            return 5;
        }
        [context setTargetUserID:@501];
        error=nil;
        id result=[service registerContainerizedApplicationWithInfoDictionaries:@[registration]
            operationUUID:[NSUUID UUID] requestContext:context saveObserver:nil registrationError:&error];
        const BOOL registered=result!=nil && error==nil;
        fprintf(stderr,"nezha-location-launch: original system NezhaLocation registration=%u error=%ld domain=%s path=%s\n",
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
        if (!registered_location(application)) return 7;
    }
    return request_location(objc_getClass("FBSOpenApplicationService"),
        objc_getClass("FBSOpenApplicationOptions"),identifier,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC));
}

#ifndef NEZHA_LOCATION_LAUNCH_TEST
static void deadline(int value)
{
    (void)value;
    static const char text[]="nezha-location-launch: bounded LaunchServices deadline\n";
    (void)write(STDERR_FILENO,text,sizeof(text)-1);
    _exit(124);
}

int main(void)
{
    /* The old root job created a0644 stderr file owned by uid0. Open the
     * mobile-owned output before Foundation startup, keeping that old file. */
    const char *output="/private/var/mobile/Library/Logs/nezha-location-launch-mobile.stderr";
    if(location_output(output)) return 10;
    setvbuf(stderr,NULL,_IONBF,0);
    fprintf(stderr,"nezha-location-launch: process pid=%u uid=%u euid=%u\n",
        (unsigned)getpid(),(unsigned)getuid(),(unsigned)geteuid());
    @autoreleasepool {
        fprintf(stderr,"nezha-location-launch: scheduled pid=%u uid=%u euid=%u uptime=%.3f delay_seconds=120\n",
            (unsigned)getpid(),(unsigned)getuid(),(unsigned)geteuid(),
            NSProcessInfo.processInfo.systemUptime);
        /* The original RunAtLoad/LaunchOnlyOnce cache condition launches
         * exactly once. Delay this ordinary registration until startup is
         * settled, then retain the existing45-second request deadline. */
        unsigned remaining=120;
        while(remaining) remaining=sleep(remaining);
        signal(SIGALRM,deadline);
        alarm(45);
        char boot[128]={0};size_t boot_bytes=sizeof boot;
        const BOOL have_boot=sysctlbyname("kern.bootsessionuuid",boot,&boot_bytes,NULL,0)==0;
        fprintf(stderr,"nezha-location-launch: startup pid=%u uid=%u euid=%u uptime=%.3f boot=%s\n",
            (unsigned)getpid(),(unsigned)getuid(),(unsigned)geteuid(),
            NSProcessInfo.processInfo.systemUptime,have_boot ? boot:"unavailable");
        if(geteuid()!=501) {
            fputs("nezha-location-launch: requires the existing mobile launch user\n",stderr);
            return 5;
        }
        const char *framework="/System/Library/Frameworks/CoreServices.framework/CoreServices";
        void *handle=dlopen(framework,RTLD_NOW|RTLD_LOCAL);
        void *frontboard=dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices",RTLD_NOW|RTLD_LOCAL);
        Class proxy=objc_getClass("LSApplicationProxy");
        Class workspace=objc_getClass("LSApplicationWorkspace");
        if (!handle || !frontboard || !proxy || !workspace ||
            ![proxy respondsToSelector:@selector(applicationProxyForIdentifier:)] ||
            ![workspace respondsToSelector:@selector(defaultWorkspace)]) {
            fputs("nezha-location-launch: original LaunchServices interface unavailable\n",stderr);
            return 3;
        }
        return open_location(proxy,[workspace defaultWorkspace],
            [NSURL fileURLWithPath:@"/Applications/NezhaLocation.app" isDirectory:YES]);
    }
}
#endif
