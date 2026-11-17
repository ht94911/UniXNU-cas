// Receive the one IPA selected for this ephemeral AppSync test.
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#import <objc/runtime.h>
#import <dlfcn.h>
#import <arpa/inet.h>
#import <errno.h>
#import <fcntl.h>
#import <ifaddrs.h>
#import <net/if.h>
#import <poll.h>
#import <sys/socket.h>
#import <sys/time.h>
#import <unistd.h>

typedef void (*ProbeReport)(const char *, ...);
extern int unixnu_appinst_install_received(NSString *path, NSString *identifier);
extern void unixnu_appsync_probe_error(const char *stage, NSError *error);

@interface NSObject (WiFiAppSync)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (NSURL *)bundleURL;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
@end

static BOOL findLANAddress(struct in_addr peer, struct in_addr *local)
{
    struct ifaddrs *interfaces;
    if (getifaddrs(&interfaces) != 0) return NO;
    BOOL found = NO;
    for (struct ifaddrs *item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || !item->ifa_netmask || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) continue;
        struct in_addr address = ((struct sockaddr_in *)item->ifa_addr)->sin_addr;
        in_addr_t mask = ((struct sockaddr_in *)item->ifa_netmask)->sin_addr.s_addr;
        if (mask && (address.s_addr & mask) == (peer.s_addr & mask)) {
            *local = address;
            found = YES;
            break;
        }
    }
    freeifaddrs(interfaces);
    return found;
}

static int acceptIPA(struct in_addr peer, uint16_t port, NSData *token, ProbeReport report)
{
    struct in_addr local = {0};
    for (unsigned i = 0; i < 60; ++i) {
        if (findLANAddress(peer, &local)) break;
        sleep(1);
    }
    if (!local.s_addr) {
        report("APPSYNC_PROBE wifi-error stage=lan-address\n");
        return -1;
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        report("APPSYNC_PROBE wifi-error stage=socket errno=%d\n", errno);
        return -1;
    }
    int enabled = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = {
        .sin_len = sizeof(address),
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = local,
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) {
        report("APPSYNC_PROBE wifi-error stage=listen errno=%d\n", errno);
        close(listener);
        return -1;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local, ip, sizeof(ip));
    report("APPSYNC_PROBE wifi-ready ip=%s port=%u\n", ip, (unsigned)port);

    int connection = -1;
    double deadline = NSProcessInfo.processInfo.systemUptime + 240;
    while (NSProcessInfo.processInfo.systemUptime < deadline) {
        struct pollfd event = {.fd = listener, .events = POLLIN};
        int ready = poll(&event, 1, 1000);
        if (ready <= 0) {
            if (ready < 0 && errno != EINTR) break;
            continue;
        }
        struct sockaddr_in sender;
        socklen_t length = sizeof(sender);
        connection = accept(listener, (struct sockaddr *)&sender, &length);
        if (connection < 0) {
            if (errno == EINTR) continue;
            break;
        }
        struct timeval auth = {.tv_sec = 5};
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &auth, sizeof(auth));
        uint8_t presented[32];
        if (sender.sin_addr.s_addr == peer.s_addr &&
            recv(connection, presented, sizeof(presented), MSG_WAITALL) == (ssize_t)sizeof(presented) &&
            [token isEqualToData:[NSData dataWithBytes:presented length:sizeof(presented)]]) break;
        close(connection);
        connection = -1;
    }
    close(listener);
    if (connection < 0) report("APPSYNC_PROBE wifi-error stage=accept\n");
    return connection;
}

static BOOL receiveIPA(int connection, int out, uint64_t size, NSData *expected, ProbeReport report)
{
    uint8_t buffer[128 * 1024];
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    struct timeval timeout = {.tv_sec = 25};
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    double started = NSProcessInfo.processInfo.systemUptime;
    uint64_t received = 0;
    uint64_t next = 16 << 20;
    while (received < size) {
        size_t want = (size_t)MIN(sizeof(buffer), size - received);
        ssize_t count = recv(connection, buffer, want, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            report("APPSYNC_PROBE wifi-error stage=receive bytes=%llu errno=%d\n",
                   (unsigned long long)received, count < 0 ? errno : 0);
            return NO;
        }
        for (ssize_t offset = 0; offset < count;) {
            ssize_t written = write(out, buffer + offset, (size_t)(count - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                report("APPSYNC_PROBE wifi-error stage=write bytes=%llu errno=%d\n",
                       (unsigned long long)received, errno);
                return NO;
            }
            offset += written;
        }
        CC_SHA256_Update(&hash, buffer, (CC_LONG)count);
        received += (uint64_t)count;
        if (received >= next) {
            report("APPSYNC_PROBE wifi-progress bytes=%llu seconds=%.3f\n",
                   (unsigned long long)received, NSProcessInfo.processInfo.systemUptime - started);
            next += 64 << 20;
        }
    }
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &hash);
    if (![expected isEqualToData:[NSData dataWithBytes:digest length:sizeof(digest)]]) {
        report("APPSYNC_PROBE wifi-error stage=sha256\n");
        return NO;
    }
    double elapsed = NSProcessInfo.processInfo.systemUptime - started;
    report("APPSYNC_PROBE wifi-transfer bytes=%llu seconds=%.3f sha256=ok\n",
           (unsigned long long)received, elapsed);
    return YES;
}

int unixnu_appsync_wifi_install(NSDictionary *config, ProbeReport report)
{
    NSString *identifier = config[@"identifier"];
    NSString *version = config[@"version"];
    NSData *token = config[@"token"], *sha = config[@"sha256"];
    NSString *source = config[@"peer"];
    uint64_t size = [config[@"bytes"] unsignedLongLongValue];
    unsigned port = [config[@"port"] unsignedIntValue];
    struct in_addr peer;
    if (!identifier.length || !version.length || !source.length || token.length != 32 ||
        sha.length != CC_SHA256_DIGEST_LENGTH || !size || !port || port > UINT16_MAX ||
        inet_pton(AF_INET, source.UTF8String, &peer) != 1) {
        report("APPSYNC_PROBE wifi-error stage=config\n");
        return 1;
    }
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSError *error = nil;
    if (![files createDirectoryAtPath:directory withIntermediateDirectories:NO
        attributes:@{NSFilePosixPermissions: @0755} error:&error]) {
        unixnu_appsync_probe_error("create-transfer-directory", error);
        return 1;
    }
    @try {
        NSString *ipa = [directory stringByAppendingPathComponent:@"install.ipa"];
        BOOL received = NO;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            int connection = acceptIPA(peer, (uint16_t)port, token, report);
            if (connection < 0) return 1;
            int out = open(ipa.fileSystemRepresentation, O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (out < 0) {
                report("APPSYNC_PROBE wifi-error stage=create-file errno=%d\n", errno);
                close(connection);
                return 1;
            }
            report("APPSYNC_PROBE wifi-receiving attempt=%u\n", attempt + 1);
            received = receiveIPA(connection, out, size, sha, report);
            close(connection);
            if (close(out) != 0) return 1;
            if (received) break;
            if (attempt == 0) report("APPSYNC_PROBE wifi-retry\n");
        }
        if (!received) return 1;

        report("APPSYNC_PROBE wifi-install-start id=%s\n", identifier.UTF8String);
        int result = unixnu_appinst_install_received(ipa, identifier);
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
        Class proxyClass = objc_getClass("LSApplicationProxy");
        Class workspace = objc_getClass("LSApplicationWorkspace");
        NSDictionary *info = nil;
        if (!result && [proxyClass respondsToSelector:@selector(applicationProxyForIdentifier:)]) {
            for (unsigned i = 0; i < 5; ++i) {
                id proxy = [proxyClass applicationProxyForIdentifier:identifier];
                NSURL *plist = [[proxy bundleURL] URLByAppendingPathComponent:@"Info.plist"];
                if (plist) info = [NSDictionary dictionaryWithContentsOfURL:plist];
                if ([info[@"CFBundleShortVersionString"] isEqual:version]) break;
                sleep(1);
            }
        }
        BOOL registered = [info[@"CFBundleIdentifier"] isEqual:identifier] &&
            [info[@"CFBundleShortVersionString"] isEqual:version];
        BOOL opened = NO;
        id applicationWorkspace = [workspace defaultWorkspace];
        if (!result && registered && [applicationWorkspace respondsToSelector:@selector(openApplicationWithBundleID:)])
            opened = [applicationWorkspace openApplicationWithBundleID:identifier];
        report("APPSYNC_PROBE wifi-install code=%d registered=%d id=%s version=%s build=%s open=%d\n",
               result, registered, identifier.UTF8String,
               [info[@"CFBundleShortVersionString"] UTF8String] ?: "-",
               [info[@"CFBundleVersion"] UTF8String] ?: "-", opened);
        return result || !registered;
    } @finally {
        [files removeItemAtPath:directory error:NULL];
    }
}
