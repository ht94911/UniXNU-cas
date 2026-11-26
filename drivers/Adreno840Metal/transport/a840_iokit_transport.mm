/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Loads IOKit dynamically via dlopen/dlsym so iPhoneOS SDK builds do not require
 * private framework stubs, and matches the proven IOAcceleratorES 0x4147 open
 * type plus sandbox selector tunneling / mailbox fallback. */
#import "a840_iokit_transport.h"

#include <dlfcn.h>
#include <mach/error.h>
#include <mach/vm_types.h>
#include <unistd.h>

#include <cstring>

namespace {

using IOServiceMatchingFn = void *(*)(const char *);
using IOServiceGetMatchingServiceFn = mach_port_t (*)(mach_port_t, void *);
using IOServiceOpenFn = kern_return_t (*)(mach_port_t, task_port_t, uint32_t, mach_port_t *);
using IOServiceCloseFn = kern_return_t (*)(mach_port_t);
using IOConnectCallStructMethodFn = kern_return_t (*)(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);
using IOConnectMapMemory64Fn = kern_return_t (*)(mach_port_t, uint32_t, task_port_t, mach_vm_address_t *, mach_vm_size_t *, uint32_t);
using IOConnectUnmapMemory64Fn = kern_return_t (*)(mach_port_t, uint32_t, task_port_t, mach_vm_address_t);

struct IokitSymbols {
    IOServiceMatchingFn matching;
    IOServiceGetMatchingServiceFn getService;
    IOServiceOpenFn openService;
    IOServiceCloseFn closeService;
    IOConnectCallStructMethodFn callStruct;
    IOConnectMapMemory64Fn mapMemory;
    IOConnectUnmapMemory64Fn unmapMemory;
};

constexpr uint32_t kMapAnywhere = 1u;
constexpr uint32_t kNativeOpenType = 16711u;       /* 'AG' */
constexpr uint32_t kTunnelMagic = 1314147916u;     /* 'NTNL' */
constexpr uint32_t kTunnelProbe = UINT32_MAX;
constexpr uint32_t kMailboxToken = 49374u;         /* 0xC0DE */
constexpr uint32_t kMailboxMagic = 1296124236u;    /* 'MAIL' */

static const IokitSymbols *loadIokitSymbols()
{
    static IokitSymbols syms{};
    static BOOL loaded = NO;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        void *handle = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
        if (!handle) return;
        syms.matching = reinterpret_cast<IOServiceMatchingFn>(dlsym(handle, "IOServiceMatching"));
        syms.getService = reinterpret_cast<IOServiceGetMatchingServiceFn>(dlsym(handle, "IOServiceGetMatchingService"));
        syms.openService = reinterpret_cast<IOServiceOpenFn>(dlsym(handle, "IOServiceOpen"));
        syms.closeService = reinterpret_cast<IOServiceCloseFn>(dlsym(handle, "IOServiceClose"));
        syms.callStruct = reinterpret_cast<IOConnectCallStructMethodFn>(dlsym(handle, "IOConnectCallStructMethod"));
        syms.mapMemory = reinterpret_cast<IOConnectMapMemory64Fn>(dlsym(handle, "IOConnectMapMemory64"));
        syms.unmapMemory = reinterpret_cast<IOConnectUnmapMemory64Fn>(dlsym(handle, "IOConnectUnmapMemory64"));
        loaded = syms.callStruct && syms.mapMemory && syms.unmapMemory && syms.closeService;
    });
    return loaded ? &syms : nullptr;
}

static A840TransportStatus transportStatus(kern_return_t error)
{
    if (error == KERN_SUCCESS) return A840_TRANSPORT_OK;
    if (err_get_system(error) != 56 || err_get_sub(error) != 0)
        return A840_TRANSPORT_STATE_ERROR;
    switch (err_get_code(error)) {
    case 701:
    case 702:
        return A840_TRANSPORT_OOM;
    case 704:
    case 752:
        return A840_TRANSPORT_NOT_FOUND;
    case 706:
    case 720:
        return A840_TRANSPORT_INVALID_ARG;
    case 711:
        return A840_TRANSPORT_UNSUPPORTED;
    case 725:
        return A840_TRANSPORT_BUSY;
    case 726:
        return A840_TRANSPORT_TIMEOUT;
    default:
        return A840_TRANSPORT_STATE_ERROR;
    }
}

} /* namespace */

@interface A840IokitTransport ()
- (A840TransportStatus)call:(uint32_t)method input:(const void *)input size:(size_t)inBytes
                     output:(void *)output size:(size_t)outBytes;
- (A840TransportStatus)map:(uint32_t)token bytes:(uint64_t)bytes address:(void **)out;
- (A840TransportStatus)unmap:(uint32_t)token;
@end

static A840TransportStatus callMethod(void *cookie, uint32_t method,
    const void *input, size_t inBytes, void *output, size_t outBytes)
{
    return [(__bridge A840IokitTransport *)cookie call:method input:input size:inBytes
                                                output:output size:outBytes];
}

static A840TransportStatus mapMemory(void *cookie, uint32_t token, uint64_t bytes,
    bool coherent, void **out)
{
    (void)coherent;
    return [(__bridge A840IokitTransport *)cookie map:token bytes:bytes address:out];
}

static A840TransportStatus unmapMemory(void *cookie, uint32_t token)
{
    return [(__bridge A840IokitTransport *)cookie unmap:token];
}

@implementation A840IokitTransport {
    mach_port_t _connection;
    BOOL _ownsConnection;
    BOOL _hasExternalBackend;
    A840BackendTransport _externalBackend;
    const IokitSymbols *_syms;
    BOOL _useTunnel;
    uint32_t _tunnelSelector;
    volatile uint32_t *_mailbox;
    mach_vm_address_t _mailboxAddr;
    uint32_t _mailboxSeq;
    A840IokitWire *_wire;
    NSMutableDictionary<NSNumber *, NSNumber *> *_views;
}

- (void)probeTransportMode
{
    if (!_syms || _connection == MACH_PORT_NULL) return;
    const uint32_t probe[2] = {kTunnelMagic, kTunnelProbe};
    for (uint32_t sel = 0u; sel < 256u; ++sel) {
        size_t outLen = 0u;
        if (_syms->callStruct(_connection, sel, probe, sizeof(probe), nullptr, &outLen) == KERN_SUCCESS) {
            _useTunnel = YES;
            _tunnelSelector = sel;
            return;
        }
    }
    mach_vm_address_t addr = 0;
    mach_vm_size_t len = 0;
    if (_syms->mapMemory(_connection, kMailboxToken, mach_task_self(), &addr, &len, kMapAnywhere) == KERN_SUCCESS &&
        addr != 0 && len >= 4096u) {
        auto *words = reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(addr));
        if (words[0] == kMailboxMagic) {
            _mailbox = words;
            _mailboxAddr = addr;
            return;
        }
        (void)_syms->unmapMemory(_connection, kMailboxToken, mach_task_self(), addr);
    }
}

- (instancetype)initWithConnection:(mach_port_t)connection
                    takesOwnership:(BOOL)ownsConnection
                             error:(NSError **)error
{
    if (!(self = [super init])) return nil;
    _connection = connection;
    _ownsConnection = ownsConnection;
    _views = [NSMutableDictionary new];
    _syms = loadIokitSymbols();
    if (!_syms || connection == MACH_PORT_NULL) {
        if (error) *error = [NSError errorWithDomain:@"Adreno840Metal.IOKit"
                                                code:A840_TRANSPORT_INVALID_ARG userInfo:nil];
        return nil;
    }
    [self probeTransportMode];
    const auto status = a840_wire_create(callMethod, mapMemory, unmapMemory, (__bridge void *)self, &_wire);
    if (status != A840_TRANSPORT_OK) {
        if (error) *error = [NSError errorWithDomain:@"Adreno840Metal.IOKit" code:status userInfo:nil];
        return nil;
    }
    return self;
}

- (instancetype)initWithDefaultAcceleratorError:(NSError **)error
{
    const IokitSymbols *syms = loadIokitSymbols();
    if (!syms || !syms->matching || !syms->getService || !syms->openService) {
        if (error) *error = [NSError errorWithDomain:@"Adreno840Metal.IOKit"
                                                code:A840_TRANSPORT_NOT_FOUND userInfo:nil];
        return nil;
    }
    mach_port_t service = MACH_PORT_NULL;
    for (unsigned attempt = 0u; attempt < 120u && service == MACH_PORT_NULL; ++attempt) {
        void *match = syms->matching("IOAcceleratorES");
        if (match) service = syms->getService(MACH_PORT_NULL, match);
        if (service == MACH_PORT_NULL) usleep(250000);
    }
    if (service == MACH_PORT_NULL) {
        if (error) *error = [NSError errorWithDomain:@"Adreno840Metal.IOKit"
                                                code:A840_TRANSPORT_NOT_FOUND userInfo:nil];
        return nil;
    }
    mach_port_t conn = MACH_PORT_NULL;
    const kern_return_t kr = syms->openService(service, mach_task_self(), kNativeOpenType, &conn);
    if (kr != KERN_SUCCESS || conn == MACH_PORT_NULL) {
        if (error) *error = [NSError errorWithDomain:@"Adreno840Metal.IOKit"
                                                code:transportStatus(kr) userInfo:nil];
        return nil;
    }
    return [self initWithConnection:conn takesOwnership:YES error:error];
}

- (instancetype)initWithBackend:(A840BackendTransport)backend
{
    if (!backend.ops) return nil;
    if (!(self = [super init])) return nil;
    _hasExternalBackend = YES;
    _externalBackend = backend;
    _views = [NSMutableDictionary new];
    return self;
}

- (A840IokitWire *)wire { return _wire; }
- (A840BackendTransport)backend
{
    return _hasExternalBackend ? _externalBackend : a840_wire_interface(_wire);
}

- (BOOL)reportMetadataLine:(NSString *)line
{
    if (!_wire || !line.length) return NO;
    const char *utf8 = line.UTF8String;
    if (!utf8) return NO;
    A840WireMetadata meta{};
    meta.handle = 1u;
    size_t len = std::strlen(utf8);
    if (len > A840_WIRE_METADATA_BYTES) len = A840_WIRE_METADATA_BYTES;
    meta.bytes = static_cast<uint32_t>(len);
    std::memcpy(meta.text, utf8, len);
    return a840_wire_call(_wire, A840_WIRE_SET_METADATA, &meta, sizeof(meta), nullptr, 0u) == A840_TRANSPORT_OK;
}

- (A840TransportStatus)callMailbox:(uint32_t)method input:(const void *)input size:(size_t)inBytes
                            output:(void *)output size:(size_t)outBytes
{
    if (!_mailbox || inBytes > 1792u || outBytes > 1792u) return A840_TRANSPORT_INVALID_ARG;
    @synchronized (self) {
        if (inBytes && input)
            std::memcpy(const_cast<uint32_t *>(_mailbox) + 16u, input, inBytes);
        _mailbox[3] = method;
        _mailbox[5] = static_cast<uint32_t>(inBytes);
        _mailbox[6] = static_cast<uint32_t>(outBytes);
        __sync_synchronize();
        const uint32_t seq = ++_mailboxSeq;
        _mailbox[1] = seq;
        __sync_synchronize();
        for (unsigned i = 0u; i < 6000u && _mailbox[2] != seq; ++i) usleep(500);
        if (_mailbox[2] != seq) return A840_TRANSPORT_TIMEOUT;
        __sync_synchronize();
        const kern_return_t kr = static_cast<kern_return_t>(_mailbox[4]);
        if (kr == KERN_SUCCESS && outBytes && output)
            std::memcpy(output, const_cast<uint32_t *>(_mailbox) + 512u, outBytes);
        return transportStatus(kr);
    }
}

- (A840TransportStatus)call:(uint32_t)method input:(const void *)input size:(size_t)inBytes
                     output:(void *)output size:(size_t)outBytes
{
    if (_mailbox) return [self callMailbox:method input:input size:inBytes output:output size:outBytes];
    if (!_syms) return A840_TRANSPORT_STATE_ERROR;
    size_t actual = outBytes;
    kern_return_t kr = KERN_FAILURE;
    if (_useTunnel) {
        if (inBytes > sizeof(A840WireSubmit)) return A840_TRANSPORT_INVALID_ARG;
        uint8_t buffer[8u + sizeof(A840WireSubmit)];
        const uint32_t header[2] = {kTunnelMagic, method};
        std::memcpy(buffer, header, sizeof(header));
        if (inBytes && input) std::memcpy(buffer + sizeof(header), input, inBytes);
        kr = _syms->callStruct(_connection, _tunnelSelector, buffer, sizeof(header) + inBytes,
                               outBytes ? output : nullptr, outBytes ? &actual : nullptr);
    } else {
        kr = _syms->callStruct(_connection, method, input, inBytes,
                               outBytes ? output : nullptr, outBytes ? &actual : nullptr);
    }
    const auto status = transportStatus(kr);
    if (status == A840_TRANSPORT_OK && outBytes && actual != outBytes)
        return A840_TRANSPORT_STATE_ERROR;
    return status;
}

- (A840TransportStatus)map:(uint32_t)token bytes:(uint64_t)bytes address:(void **)out
{
    if (!_syms) return A840_TRANSPORT_STATE_ERROR;
    @synchronized (_views) {
        if (_views[@(token)]) return A840_TRANSPORT_BUSY;
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        const auto status = transportStatus(
            _syms->mapMemory(_connection, token, mach_task_self(), &address, &size, kMapAnywhere));
        if (status != A840_TRANSPORT_OK) return status;
        if (!address || size < bytes) {
            if (address) (void)_syms->unmapMemory(_connection, token, mach_task_self(), address);
            return A840_TRANSPORT_STATE_ERROR;
        }
        _views[@(token)] = @(address);
        *out = reinterpret_cast<void *>(static_cast<uintptr_t>(address));
        return A840_TRANSPORT_OK;
    }
}

- (A840TransportStatus)unmap:(uint32_t)token
{
    if (!_syms) return A840_TRANSPORT_STATE_ERROR;
    @synchronized (_views) {
        NSNumber *address = _views[@(token)];
        if (!address) return A840_TRANSPORT_OK;
        const auto status = transportStatus(
            _syms->unmapMemory(_connection, token, mach_task_self(), address.unsignedLongLongValue));
        if (status == A840_TRANSPORT_OK) [_views removeObjectForKey:@(token)];
        return status;
    }
}

- (void)dealloc
{
    if (_wire) a840_wire_destroy(_wire);
    if (_syms && _connection != MACH_PORT_NULL) {
        if (_mailboxAddr)
            (void)_syms->unmapMemory(_connection, kMailboxToken, mach_task_self(), _mailboxAddr);
        for (NSNumber *token in _views)
            (void)_syms->unmapMemory(_connection, token.unsignedIntValue, mach_task_self(),
                                     _views[token].unsignedLongLongValue);
        if (_ownsConnection) (void)_syms->closeService(_connection);
    }
}
@end
