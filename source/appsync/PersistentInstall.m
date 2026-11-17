// The persistent Data inbox accepts one install at a time from its paired PC.
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#import <arpa/inet.h>
#import <dlfcn.h>
#import <errno.h>
#import <fcntl.h>
#import <objc/runtime.h>
#import <sys/mount.h>
#import <sys/socket.h>
#import <sys/stat.h>
#import <sys/time.h>
#import <unistd.h>

typedef void (*InstallReport)(const char *, ...);
typedef id (*CreateTransaction)(const char *) NS_RETURNS_RETAINED;
extern int unixnu_appinst_install_received(NSString *path, NSString *identifier);
@interface NSObject (AppSyncRegistration)
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (NSURL *)bundleURL;
@end

enum InstallStatus { Ready, NoSpace, Failed, Installed };
enum { RequestBytes = 108, TokenBytes = 32, ReplyBytes = 16 };

static BOOL readAll(int fd, void *buffer, size_t bytes)
{
    uint8_t *position = buffer;
    while (bytes) {
        ssize_t count = read(fd, position, bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return NO;
        bytes -= (size_t)count;
        position += count;
    }
    return YES;
}

static BOOL writeAll(int fd, const void *buffer, size_t bytes)
{
    const uint8_t *position = buffer;
    while (bytes) {
        ssize_t count = write(fd, position, bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return NO;
        bytes -= (size_t)count;
        position += count;
    }
    return YES;
}

static BOOL sendReply(int connection, enum InstallStatus status, uint64_t value)
{
    uint8_t response[ReplyBytes] = {'A', 'S', '2', '7', status};
    uint64_t encoded = CFSwapInt64HostToBig(value);
    memcpy(response + 8, &encoded, sizeof(encoded));
    return writeAll(connection, response, sizeof(response));
}

static uint64_t readSize(const uint8_t *bytes)
{
    uint64_t value;
    memcpy(&value, bytes, sizeof(value));
    return CFSwapInt64BigToHost(value);
}

static NSString *hexName(const uint8_t *bytes, size_t count)
{
    NSMutableString *name = [NSMutableString stringWithCapacity:2 * count];
    for (size_t i = 0; i < count; ++i) [name appendFormat:@"%02x", bytes[i]];
    return name;
}

static BOOL verifyArchive(int fd, uint64_t bytes, const uint8_t *expected)
{
    if (lseek(fd, 0, SEEK_SET) < 0) return NO;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    uint8_t buffer[128 * 1024];
    while (bytes) {
        size_t count = (size_t)MIN(bytes, sizeof(buffer));
        if (!readAll(fd, buffer, count)) return NO;
        CC_SHA256_Update(&context, buffer, (CC_LONG)count);
        bytes -= count;
    }
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    return memcmp(digest, expected, sizeof(digest)) == 0;
}

static NSDictionary *registeredBundle(NSString *identifier)
{
    dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
    Class proxyClass = objc_getClass("LSApplicationProxy");
    id proxy = [proxyClass applicationProxyForIdentifier:identifier];
    NSURL *url = [[proxy bundleURL] URLByAppendingPathComponent:@"Info.plist"];
    return url ? [NSDictionary dictionaryWithContentsOfURL:url] : nil;
}

static BOOL registeredVersion(NSString *identifier, NSString *version)
{
    NSDictionary *info = registeredBundle(identifier);
    NSString *actual = info[@"CFBundleShortVersionString"] ?: info[@"CFBundleVersion"];
    return [info[@"CFBundleIdentifier"] isEqual:identifier] && [actual isEqual:version];
}

static void receiveInstall(int connection, NSString *inbox, NSData *token, InstallReport report)
{
    uint8_t header[RequestBytes];
    if (!readAll(connection, header, sizeof(header)) ||
        ![token isEqualToData:[NSData dataWithBytes:header + 8 length:TokenBytes]]) return;
    BOOL usbReplies = memcmp(header, "APPSYNU3", 8) == 0;
    if (!usbReplies && memcmp(header, "APPSYN03", 8)) return;
    NSString *request = hexName(header + 92, 16);
    BOOL (^reply)(enum InstallStatus, uint64_t) = ^BOOL(enum InstallStatus status, uint64_t value) {
        report("APPSYNC_PROBE persistent-reply request=%s status=%u value=%llu\n",
               request.UTF8String, (unsigned)status, (unsigned long long)value);
        return usbReplies || sendReply(connection, status, value);
    };
    const uint64_t bytes = readSize(header + 40), extraction = readSize(header + 48);
    const uint8_t *expected = header + 56;
    const size_t identifierBytes = (unsigned)header[88] * 256 + header[89];
    const size_t versionBytes = (unsigned)header[90] * 256 + header[91];
    uint8_t names[1024];
    if (!bytes || bytes > INT64_MAX || !extraction || !identifierBytes || !versionBytes ||
        identifierBytes + versionBytes > sizeof(names) ||
        !readAll(connection, names, identifierBytes + versionBytes)) {
        reply(Failed, EINVAL);
        return;
    }
    NSString *identifier = [[NSString alloc] initWithBytes:names length:identifierBytes encoding:NSUTF8StringEncoding];
    NSString *version = [[NSString alloc] initWithBytes:names + identifierBytes length:versionBytes encoding:NSUTF8StringEncoding];
    if (!identifier.length || !version.length) {
        reply(Failed, EINVAL);
        return;
    }
    NSString *name = [inbox stringByAppendingPathComponent:hexName(expected, CC_SHA256_DIGEST_LENGTH)];
    NSString *receipt = [name stringByAppendingPathExtension:@"plist"];
    NSDictionary *finished = [NSDictionary dictionaryWithContentsOfFile:receipt];
    if ([finished[@"identifier"] isEqual:identifier] && [finished[@"version"] isEqual:version] &&
        registeredVersion(identifier, version)) {
        reply(Installed, 0);
        return;
    }
    NSString *path = [name stringByAppendingPathExtension:@"ipa"];
    int fd = open(path.fileSystemRepresentation, O_NOFOLLOW | O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        reply(Failed, errno);
        return;
    }
    @try {
        struct stat current;
        struct statfs volume;
        if (fstat(fd, &current) || !S_ISREG(current.st_mode) || current.st_size < 0 ||
            (uint64_t)current.st_size > bytes || fstatfs(fd, &volume)) {
            reply(Failed, EIO);
            return;
        }
        uint64_t offset = (uint64_t)current.st_size;
        uint64_t available = volume.f_bavail * (uint64_t)volume.f_bsize;
        if (bytes - offset > available || extraction > available - (bytes - offset)) {
            reply(NoSpace, available);
            return;
        }
        if (lseek(fd, (off_t)offset, SEEK_SET) < 0 || !reply(Ready, offset)) return;
        double started = NSProcessInfo.processInfo.systemUptime;
        uint8_t buffer[128 * 1024];
        while (offset < bytes) {
            ssize_t count = read(connection, buffer, (size_t)MIN(sizeof(buffer), bytes - offset));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0 || !writeAll(fd, buffer, (size_t)count)) {
                (void)fsync(fd);
                return;
            }
            offset += (uint64_t)count;
        }
        if (fsync(fd)) {
            reply(Failed, errno);
            return;
        }
        if (!verifyArchive(fd, bytes, expected)) {
            unlink(path.fileSystemRepresentation);
            reply(Failed, EBADMSG);
            return;
        }
        report("APPSYNC_PROBE persistent-transfer request=%s bytes=%llu seconds=%.3f sha256=ok\n",
               request.UTF8String, (unsigned long long)bytes, NSProcessInfo.processInfo.systemUptime - started);
    } @finally {
        close(fd);
    }
    int result = unixnu_appinst_install_received(path, identifier);
    NSDictionary *info = nil;
    BOOL registered = NO;
    for (unsigned attempt = 0; !result && !registered && attempt < 5; ++attempt) {
        info = registeredBundle(identifier);
        NSString *actual = info[@"CFBundleShortVersionString"] ?: info[@"CFBundleVersion"];
        registered = [info[@"CFBundleIdentifier"] isEqual:identifier] && [actual isEqual:version];
        if (!registered) sleep(1);
    }
    report("APPSYNC_PROBE persistent-install request=%s code=%d registered=%d id=%s version=%s build=%s\n",
           request.UTF8String, result, registered, identifier.UTF8String, version.UTF8String,
           [info[@"CFBundleVersion"] UTF8String] ?: "-");
    if (result || !registered) {
        reply(Failed, result ?: EIO);
        return;
    }
    [@{@"identifier": identifier, @"version": version} writeToFile:receipt atomically:YES];
    unlink(path.fileSystemRepresentation);
    reply(Installed, 0);
}

int unixnu_appsync_persistent_install(NSDictionary *config, InstallReport report)
{
    NSData *token = config[@"token"];
    NSString *allowed = config[@"peer"];
    unsigned port = [config[@"port"] unsignedIntValue];
    struct in_addr peer;
    if (token.length != TokenBytes || !allowed.length || !port || port > UINT16_MAX ||
        inet_pton(AF_INET, allowed.UTF8String, &peer) != 1) return 1;
    NSString *inbox = @"/private/var/db/unixnu-appsync";
    if (mkdir(inbox.fileSystemRepresentation, 0700) && errno != EEXIST) return 1;
    struct stat storage;
    if (lstat(inbox.fileSystemRepresentation, &storage) || !S_ISDIR(storage.st_mode) ||
        storage.st_uid != getuid() || (storage.st_mode & 077)) return 1;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    const int one = 1;
    struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET, .sin_port = htons(port)};
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) {
        close(listener);
        return 1;
    }
    report("APPSYNC_PROBE persistent-ready port=%u\n", port);
    CreateTransaction create = (CreateTransaction)dlsym(RTLD_DEFAULT, "os_transaction_create");
    for (;;) {
        struct sockaddr_in incoming;
        socklen_t length = sizeof(incoming);
        int connection = accept(listener, (struct sockaddr *)&incoming, &length);
        if (connection < 0 && errno == EINTR) continue;
        if (connection < 0) break;
        if (incoming.sin_addr.s_addr == peer.s_addr) {
            @autoreleasepool {
                struct timeval timeout = {.tv_sec = 45};
                setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
                setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                __attribute__((objc_precise_lifetime)) id active = create ? create("AppSync persistent IPA") : nil;
                if (active) receiveInstall(connection, inbox, token, report);
            }
        }
        close(connection);
    }
    close(listener);
    return 1;
}
