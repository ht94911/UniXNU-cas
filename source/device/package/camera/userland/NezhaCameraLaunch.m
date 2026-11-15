/* SPDX-License-Identifier: MIT */
/* Launch only the original Camera.app through the existing AppSync
 * ProbeRunner LaunchServices entry. The helper never reads camera data,
 * asks for TCC permission, sends touches, or marks the app successful. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

@interface NSObject (NezhaCameraLaunchServices)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:identifier;
- (NSURL *)bundleURL;
- (BOOL)openApplicationWithBundleID:identifier;
@end

static void deadline(int value)
{
    (void)value;
    static const char text[]="nezha-camera-launch: bounded LaunchServices deadline\n";
    (void)write(STDERR_FILENO,text,sizeof(text)-1);
    _exit(124);
}

int main(void)
{
    @autoreleasepool {
        setvbuf(stderr,NULL,_IONBF,0);
        signal(SIGALRM,deadline);
        alarm(25);
        const char *framework="/System/Library/Frameworks/CoreServices.framework/CoreServices";
        void *handle=dlopen(framework,RTLD_NOW|RTLD_LOCAL);
        Class proxy=objc_getClass("LSApplicationProxy");
        Class workspace=objc_getClass("LSApplicationWorkspace");
        NSString *identifier=@"com.apple.camera";
        if (!handle || !proxy || !workspace ||
            ![proxy respondsToSelector:@selector(applicationProxyForIdentifier:)] ||
            ![workspace respondsToSelector:@selector(defaultWorkspace)]) {
            fputs("nezha-camera-launch: original LaunchServices interface unavailable\n",stderr);
            return 3;
        }
        id application=[proxy applicationProxyForIdentifier:identifier];
        NSURL *bundle=[application respondsToSelector:@selector(bundleURL)] ? [application bundleURL] : nil;
        NSDictionary *info=bundle ? [NSDictionary dictionaryWithContentsOfURL:[bundle URLByAppendingPathComponent:@"Info.plist"]] : nil;
        if (![info[@"CFBundleIdentifier"] isEqualToString:identifier] ||
            ![info[@"CFBundleExecutable"] isEqualToString:@"Camera"]) {
            fputs("nezha-camera-launch: original Camera bundle is not registered\n",stderr);
            return 4;
        }
        id service=[workspace defaultWorkspace];
        if (![service respondsToSelector:@selector(openApplicationWithBundleID:)]) {
            fputs("nezha-camera-launch: original launch method unavailable\n",stderr);
            return 5;
        }
        /* The original24A446 BOOL result acknowledges a request. Actual
         * Camera PID, graph callbacks and pixels still qualify the run. */
        const BOOL requested=[service openApplicationWithBundleID:identifier];
        fprintf(stderr,"nezha-camera-launch: originalCamera registered=1 launch_request=%u preview_verified=0 photo_verified=0\n",(unsigned)requested);
        return requested ? 0 : 6;
    }
}
