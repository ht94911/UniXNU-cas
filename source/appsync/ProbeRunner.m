// AppSync device qualification in an isolated RAM Data boot.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <spawn.h>
#import <signal.h>
#import <sys/wait.h>
#import <unistd.h>

extern char **environ;
@interface NSObject (ProbeInstallation)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (NSURL *)bundleURL;
- (NSURL *)dataContainerURL;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
@end

static void installAlarm(int signalNumber)
{
    (void)signalNumber;
    static const char line[] = "APPSYNC_PROBE timeout\n";
    (void)write(STDERR_FILENO, line, sizeof(line)-1);
    _exit(124);
}

static int runInstall(const char *name)
{
    NSString *path = [@"/private/var/ngpu/appsync/probe" stringByAppendingPathComponent:@(name)];
    const char *tool = "/usr/libexec/unixnu-appinst";
    char *const command[] = {(char *)tool, (char *)path.fileSystemRepresentation, NULL};
    pid_t child;
    int status;
    const int spawnError = posix_spawn(&child, tool, NULL, NULL, command, environ);
    if (spawnError) return -spawnError;
    for (unsigned attempt = 0; attempt < 45; ++attempt) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (result < 0) return 129;
        sleep(1);
    }
    (void)kill(child, SIGTERM);
    return 124;
}

static NSString *versionFor(id proxy)
{
    if (![proxy respondsToSelector:@selector(bundleURL)]) return nil;
    NSURL *url = [proxy bundleURL];
    return [NSDictionary dictionaryWithContentsOfURL:[url URLByAppendingPathComponent:@"Info.plist"]][@"CFBundleVersion"];
}

static NSInteger launchesFor(id proxy, NSString *identifier)
{
    if (![proxy respondsToSelector:@selector(dataContainerURL)]) return 0;
    NSURL *url = [proxy dataContainerURL];
    NSString *relative = [NSString stringWithFormat:@"Library/Preferences/%@.plist", identifier];
    return [[NSDictionary dictionaryWithContentsOfURL:[url URLByAppendingPathComponent:relative]]
        [@"AppSyncProbeLaunches"] integerValue];
}

int main(void)
{
    @autoreleasepool {
        setvbuf(stderr, NULL, _IONBF, 0);
        signal(SIGALRM, installAlarm);
        alarm(230);
        fprintf(stderr, "APPSYNC_PROBE start ram-data\n");
        sleep(60);
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
        Class proxyClass = objc_getClass("LSApplicationProxy");
        Class workspaceClass = objc_getClass("LSApplicationWorkspace");
        if (![proxyClass respondsToSelector:@selector(applicationProxyForIdentifier:)] ||
            ![workspaceClass respondsToSelector:@selector(defaultWorkspace)]) {
            fprintf(stderr, "APPSYNC_PROBE unavailable launch-services\n");
            return 3;
        }
        id workspace = [workspaceClass defaultWorkspace];
        struct Variant { const char *name, *identifier, *version; };
        static const struct Variant variants[] = {
            {"appsync-probe-signed-v1.ipa", "org.unixnu.nezha.appsync.probe.signed", "1"},
            {"appsync-probe-unsigned-v1.ipa", "org.unixnu.nezha.appsync.probe.unsigned", "1"},
            {"appsync-probe-unsigned-v2.ipa", "org.unixnu.nezha.appsync.probe.unsigned", "2"},
        };
        unsigned installs = 0, launches = 0;
        for (unsigned i = 0; i < sizeof(variants)/sizeof(*variants); ++i) {
            const struct Variant *variant = &variants[i];
            NSString *identifier = @(variant->identifier);
            const int result = runInstall(variant->name);
            id proxy = [proxyClass applicationProxyForIdentifier:identifier];
            NSString *version = versionFor(proxy);
            const BOOL installed = result == 0 && [version isEqualToString:@(variant->version)];
            installs += installed;
            BOOL opened = NO, observed = NO;
            if (installed && [workspace respondsToSelector:@selector(openApplicationWithBundleID:)]) {
                const NSInteger before = launchesFor(proxy, identifier);
                opened = [workspace openApplicationWithBundleID:identifier];
                for (unsigned attempt = 0; opened && attempt < 12; ++attempt) {
                    if (launchesFor(proxy, identifier) > before) { observed = YES; break; }
                    sleep(1);
                }
            }
            launches += observed;
            fprintf(stderr, "APPSYNC_PROBE case=%u install=%d version=%s open=%u launch=%u\n", i,
                    result, version.UTF8String ?: "-", (unsigned)opened, (unsigned)observed);
        }
        fprintf(stderr, "APPSYNC_PROBE end installed=%u launched=%u total=3\n", installs, launches);
        return installs == 3 ? 0 : 1;
    }
}
