/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Native Adreno 840 BinaryArchive & Library loader (data/file/source/stitched). */
#import "A840MTLLibraryAndArchive.h"
#import "A840MTLDiagnostics.h"
#import "A840MTLPipelineProvider.h"

#import <CommonCrypto/CommonDigest.h>
#import <mach-o/dyld.h>
#import <mach-o/getsect.h>
#import <mach-o/loader.h>
#import <objc/message.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

@protocol A840DeviceDagStitchingSPI
- (NSString *)newDagStringWithGraphs:(NSArray<MTLFunctionStitchingGraph *> *)graphs;
@end

typedef void (^A840CodeGenReply)(int32_t status, const void *reply, size_t length, const char *message);

static void *a840RunLargeStackBlock(void *block)
{
    @autoreleasepool {
        ((__bridge void (^)(void))block)();
    }
    return nullptr;
}

static void a840OnLargeStack(void (^body)(void))
{
    pthread_attr_t attr{};
    pthread_t thread{};
    if (pthread_attr_init(&attr) != 0) {
        body();
        return;
    }
    const bool created = (pthread_attr_setstacksize(&attr, 16u << 20) == 0) &&
                         (pthread_create(&thread, &attr, a840RunLargeStackBlock, (__bridge void *)body) == 0);
    pthread_attr_destroy(&attr);
    if (created) pthread_join(thread, nullptr);
    else body();
}

static NSData *a840SystemCodeGenRequest(uint32_t requestType,
                                        const std::vector<uint8_t> &request,
                                        NSString **warningsOut,
                                        NSString **failureOut)
{
    static void *service = nullptr;
    static void (*buildFn)(void *, void *, uint32_t, const void *, size_t, A840CodeGenReply) = nullptr;
    static dispatch_once_t onceToken;
    static std::mutex codegenLock;

    dispatch_once(&onceToken, ^{
        static const char kFramework[] = "/System/Library/PrivateFrameworks/MTLCompiler.framework";
        std::string path = std::string(kFramework) + "/MTLCompiler";
        void *lib = dlopen(path.c_str(), RTLD_NOW);
        if (!lib) {
            static const char kPrefix[] = "/System/Library/PrivateFrameworks/MTLCompiler.framework/Versions/";
            for (uint32_t i = 0u, n = _dyld_image_count(); i < n && !lib; ++i) {
                const char *imgName = _dyld_get_image_name(i);
                if (!imgName || !std::strstr(imgName, "/Metal.framework/")) continue;
                const char *slash = std::strrchr(imgName, '/');
                if (!slash || std::strcmp(slash, "/Metal") != 0) continue;
                unsigned long secSize = 0u;
                const char *strings = reinterpret_cast<const char *>(
                    getsectiondata(reinterpret_cast<const struct mach_header_64 *>(_dyld_get_image_header(i)),
                                   "__TEXT", "__cstring", &secSize));
                for (unsigned long at = 0u; strings && at + sizeof(kPrefix) < secSize;) {
                    const char *text = strings + at;
                    const size_t len = strnlen(text, secSize - at);
                    if (std::strncmp(text, kPrefix, sizeof(kPrefix) - 1u) == 0 &&
                        len > sizeof(kPrefix) &&
                        std::strcmp(text + len - 12u, "/MTLCompiler") == 0) {
                        path.assign(text, len);
                        lib = dlopen(path.c_str(), RTLD_NOW);
                        break;
                    }
                    at += len + 1u;
                }
            }
        }
        if (!lib && access((std::string(kFramework) + "/Versions/Current/MTLCompiler").c_str(), R_OK) == 0) {
            path = std::string(kFramework) + "/Versions/Current/MTLCompiler";
            lib = dlopen(path.c_str(), RTLD_NOW);
        }
        if (!lib) {
            unsigned long highest = 0u;
            NSString *versionsDir = [@(kFramework) stringByAppendingString:@"/Versions"];
            for (NSString *entry in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:versionsDir error:nil]) {
                const unsigned long ver = std::strtoul(entry.UTF8String ?: "", nullptr, 10);
                if (ver > highest && [entry isEqualToString:[NSString stringWithFormat:@"%lu", ver]]) {
                    highest = ver;
                }
            }
            if (highest > 0u) {
                path = std::string(kFramework) + "/Versions/" + std::to_string(highest) + "/MTLCompiler";
                lib = dlopen(path.c_str(), RTLD_NOW);
            }
        }
        auto createFn = lib ? reinterpret_cast<void *(*)(const char *)>(dlsym(lib, "MTLCodeGenServiceCreate")) : nullptr;
        buildFn = lib ? reinterpret_cast<void (*)(void *, void *, uint32_t, const void *, size_t, A840CodeGenReply)>(
                            dlsym(lib, "MTLCodeGenServiceBuildRequest"))
                      : nullptr;
        service = (createFn && buildFn) ? createFn("adreno840-native") : nullptr;
    });

    if (!service || !buildFn) {
        if (failureOut) *failureOut = @"System MTLCompiler.framework is unavailable";
        return nil;
    }

    __block NSData *metallib = nil;
    __block NSString *warned = nil;
    __block NSString *failure = nil;
    __block bool answered = false;
    {
        std::lock_guard<std::mutex> guard(codegenLock);
        a840OnLargeStack(^{
            buildFn(service, nullptr, requestType, request.data(), request.size(),
                    ^(int32_t status, const void *reply, size_t length, const char *why) {
                        answered = true;
                        uint32_t headerBytes = 0u, warningBytes = 0u;
                        if (reply && length >= 16u) {
                            std::memcpy(&headerBytes, static_cast<const uint8_t *>(reply) + 8u, 4u);
                            std::memcpy(&warningBytes, static_cast<const uint8_t *>(reply) + 12u, 4u);
                        }
                        if (status != 0 || !reply || length < 16u ||
                            static_cast<uint64_t>(headerBytes) + warningBytes > length) {
                            failure = (why && why[0])
                                          ? [NSString stringWithUTF8String:why]
                                          : [NSString stringWithFormat:@"MTLCompiler failed (status %d)", status];
                            return;
                        }
                        const uint8_t *bytes = static_cast<const uint8_t *>(reply);
                        if (warningBytes > 0u) {
                            warned = [[NSString alloc] initWithBytes:bytes + headerBytes
                                                              length:warningBytes
                                                            encoding:NSUTF8StringEncoding];
                        }
                        const size_t bodyOff = static_cast<size_t>(headerBytes) + warningBytes;
                        metallib = [NSData dataWithBytes:bytes + bodyOff length:length - bodyOff];
                    });
        });
    }
    if (!answered && !failure) failure = @"MTLCompiler did not answer";
    if (warningsOut) *warningsOut = warned;
    if (failureOut) *failureOut = failure;
    return failure ? nil : metallib;
}

static NSString *a840EscapeCompileArg(NSString *text)
{
    NSCharacterSet *special = [NSCharacterSet characterSetWithCharactersInString:@" \t\"'"];
    if ([text rangeOfCharacterFromSet:special].location == NSNotFound) return text;
    NSString *esc = [[text stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
        stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    return [NSString stringWithFormat:@"\"%@\"", esc];
}

static NSString *a840BuildCompileArguments(MTLCompileOptions *options)
{
    NSMutableArray<NSString *> *args = [NSMutableArray arrayWithObjects:@"--driver-mode=metal", @"-x", @"metal", nil];
    const NSUInteger ver = (options && options.languageVersion) ? (NSUInteger)options.languageVersion
                                                                : (NSUInteger)MTLLanguageVersion3_1;
    const unsigned major = (unsigned)(ver >> 16u), minor = (unsigned)(ver & 0xffffu);
    [args addObject:[NSString stringWithFormat:(major >= 3u ? @"-std=metal%u.%u" : @"-std=ios-metal%u.%u"), major, minor]];
    BOOL fastMath = YES;
    if (options && [options respondsToSelector:sel_registerName("fastMathEnabled")]) {
        fastMath = reinterpret_cast<BOOL (*)(id, SEL)>(objc_msgSend)(options, sel_registerName("fastMathEnabled"));
    }
    [args addObject:(fastMath ? @"-ffast-math" : @"-fno-fast-math")];
    if (options && options.preserveInvariance) [args addObject:@"-fpreserve-invariance"];
    if (options && options.optimizationLevel == MTLLibraryOptimizationLevelSize) [args addObject:@"-Os"];
    if (options && options.preprocessorMacros) {
        [options.preprocessorMacros enumerateKeysAndObjectsUsingBlock:^(NSString *name, NSObject *value, BOOL *stop) {
            (void)stop;
            [args addObject:a840EscapeCompileArg([NSString stringWithFormat:@"-D%@=%@", name, value])];
        }];
    }
    NSString *cacheDir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"a840-msl-cache"];
    [args addObject:a840EscapeCompileArg([@"-fmodules-cache-path=" stringByAppendingString:cacheDir])];
    return [args componentsJoinedByString:@" "];
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"

static A840PipelineKey hashStringToPipelineKey(NSString *tag)
{
    A840PipelineKey key{};
    const char *utf8 = tag.UTF8String ?: "";
    uint64_t h1 = UINT64_C(14695981039346656037);
    uint64_t h2 = UINT64_C(11400714819323198485);
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(utf8); *p; ++p) {
        h1 ^= (uint64_t)(*p);
        h1 *= UINT64_C(1099511628211);
        h2 ^= ((uint64_t)(*p) + 17u);
        h2 *= UINT64_C(1099511628211);
    }
    for (size_t i = 0u; i < 4u; ++i) {
        const uint64_t v = (i & 1u) ? (h2 + i * 31u) : (h1 + i * 17u);
        std::memcpy(key.sha256 + i * 8u, &v, sizeof(v));
    }
    return key;
}

@implementation A840MTLBinaryArchive
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                   descriptor:(MTLBinaryArchiveDescriptor *)descriptor
                        error:(NSError **)error
{
    if (!(self = [super init]) || !owner || !owner.systemFrontend) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid binary archive device");
        return nil;
    }
    /* Reject foreign Apple binary archives (e.g. QuartzCore/default.metallib) rather
     * than returning a fake empty archive, which would trigger QuartzCore's offline
     * archive manifest warmup and FailOnBinaryArchiveMiss path. Only accept our own
     * A840BinaryArchiveV1 JSON archives when descriptor.url is non-nil. */
    if (descriptor && descriptor.url) {
        NSData *raw = [NSData dataWithContentsOfURL:descriptor.url options:0 error:nil];
        id parsed = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
        if (![parsed isKindOfClass:[NSDictionary class]] ||
            ![((NSDictionary *)parsed)[@"format"] isEqualToString:@"A840BinaryArchiveV1"]) {
            if (error) {
                *error = [NSError errorWithDomain:@"MTLBinaryArchiveDomain"
                                             code:2 /* MTLBinaryArchiveErrorInvalidFile */
                                         userInfo:@{NSLocalizedDescriptionKey: @"Incompatible binary archive format for Adreno 840"}];
            }
            return nil;
        }
    }
    _owner = owner;
    const uint64_t stackKey = owner.systemFrontend ? a840_sys_frontend_pack_cache(owner.systemFrontend)->expected_stack_key : 1u;
    const uint64_t abiFp = ADRENO_COMPILE_ABI_VERSION;
    A840SysArchiveHandle handle = A840_SYS_INVALID_HANDLE;
    if (a840_sys_archive_create(owner.systemFrontend, stackKey ? stackKey : 1u, abiFp, &handle) != A840_SYS_OK) {
        if (error) *error = a840_metal_error(A840_RT_CAPACITY, @"Could not create native binary archive");
        return nil;
    }
    _archiveHandle = handle;
    return self;
}

- (void)dealloc
{
    if (_owner && _owner.systemFrontend && _archiveHandle != A840_SYS_INVALID_HANDLE) {
        (void)a840_sys_archive_destroy(_owner.systemFrontend, _archiveHandle);
        _archiveHandle = A840_SYS_INVALID_HANDLE;
    }
}

- (id<MTLDevice>)device { return _owner; }

- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)descriptor
                                            error:(NSError **)error
{
    if (!descriptor || !descriptor.computeFunction) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Compute descriptor missing function");
        return NO;
    }
    const A840PipelineKey key = hashStringToPipelineKey(descriptor.computeFunction.name ?: @"compute");
    if (a840_sys_archive_add_key(_owner.systemFrontend, _archiveHandle, &key) != A840_SYS_OK) {
        if (error) *error = a840_metal_error(A840_RT_CAPACITY, @"Binary archive capacity exceeded");
        return NO;
    }
    return YES;
}

- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                           error:(NSError **)error
{
    if (!descriptor || !descriptor.vertexFunction) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Render descriptor missing vertex function");
        return NO;
    }
    NSString *tag = [NSString stringWithFormat:@"%@|%@",
        descriptor.vertexFunction.name ?: @"vs",
        descriptor.fragmentFunction.name ?: @"fs"];
    const A840PipelineKey key = hashStringToPipelineKey(tag);
    if (a840_sys_archive_add_key(_owner.systemFrontend, _archiveHandle, &key) != A840_SYS_OK) {
        if (error) *error = a840_metal_error(A840_RT_CAPACITY, @"Binary archive capacity exceeded");
        return NO;
    }
    return YES;
}

- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error
{
    if (!url) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Archive URL is required");
        return NO;
    }
    uint32_t count = 0u;
    (void)a840_sys_archive_count(_owner.systemFrontend, _archiveHandle, &count);
    NSDictionary *payload = @{
        @"format": @"A840BinaryArchiveV1",
        @"abi_version": @(ADRENO_COMPILE_ABI_VERSION),
        @"pipeline_count": @(count)
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:payload options:0 error:error];
    if (!json) return NO;
    return [json writeToURL:url options:NSDataWritingAtomic error:error];
}
- (NSUInteger)options { return 0u; }
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLBinaryArchive", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
@end

@implementation A840MTLStitchedLibraryLoader

+ (id<MTLLibrary>)libraryWithDispatchData:(dispatch_data_t)data
                                   device:(A840MTLDevice *)device
                                    error:(NSError **)error
{
    if (!data || !device) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid dispatch_data_t metallib");
        return nil;
    }
    const void *buffer = nullptr;
    size_t size = 0u;
    dispatch_data_t contiguous = dispatch_data_create_map(data, &buffer, &size);
    if (!contiguous || !buffer || size == 0u) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Empty metallib data");
        return nil;
    }
    NSData *nsData = [NSData dataWithBytes:buffer length:size];
    return [[A840MTLLibrary alloc] initWithDevice:device data:nsData error:error];
}

+ (id<MTLLibrary>)libraryWithSource:(NSString *)source
                            options:(MTLCompileOptions *)options
                             device:(A840MTLDevice *)device
                              error:(NSError **)error
{
    if (!source.length || !device) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"MSL source is required");
        return nil;
    }
    if (options && options.libraryType == MTLLibraryTypeDynamic) {
        if (error) {
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorUnsupported
                                     userInfo:@{NSLocalizedDescriptionKey: @"Dynamic libraries are not supported"}];
        }
        return nil;
    }
    NSData *textData = [source dataUsingEncoding:NSUTF8StringEncoding];
    NSString *argStr = a840BuildCompileArguments(options);
    const char *optUtf8 = argStr.UTF8String ?: "";
    const uint64_t sourceBytes = (textData.length + 1u + 3u) & ~UINT64_C(3);
    const uint64_t optBytes = std::strlen(optUtf8) + 1u;
    std::vector<uint8_t> req(16u + static_cast<size_t>(sourceBytes + optBytes), 0u);
    std::memcpy(req.data(), &sourceBytes, 8u);
    std::memcpy(req.data() + 8u, &optBytes, 8u);
    if (textData.length) std::memcpy(req.data() + 16u, textData.bytes, textData.length);
    std::memcpy(req.data() + 16u + static_cast<size_t>(sourceBytes), optUtf8, static_cast<size_t>(optBytes));

    NSString *warnings = nil, *failure = nil;
    NSData *metallib = a840SystemCodeGenRequest(13u, req, &warnings, &failure);
    if (!metallib || metallib.length < 4u) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840SRC fail %.72s", failure.UTF8String ?: "no-compiler");
        a840diag::reportText(msg);
        if (error) {
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{NSLocalizedDescriptionKey: failure ?: @"MSL compilation failed"}];
        }
        return nil;
    }
    id<MTLLibrary> lib = [[A840MTLLibrary alloc] initWithDevice:device data:metallib error:error];
    if (lib && warnings.length && error && !*error) {
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorCompileWarning
                                 userInfo:@{NSLocalizedDescriptionKey: warnings}];
    }
    return lib;
}

+ (id<MTLLibrary>)libraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor
                                         device:(A840MTLDevice *)device
                                          error:(NSError **)error
{
    if (!descriptor || !device) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid stitched library descriptor");
        return nil;
    }
    NSArray<MTLFunctionStitchingGraph *> *graphs = descriptor.functionGraphs;
    NSArray<id<MTLFunction>> *functions = descriptor.functions;
    if (!graphs.count || !functions.count) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Stitched library requires graphs and functions");
        return nil;
    }

    /* Fast path: if every requested graph function already exists in a supplied library, reuse it. */
    for (id<MTLFunction> fn in functions) {
        if ([(id)fn isKindOfClass:A840MTLFunction.class]) {
            A840MTLLibrary *ownerLib = [(A840MTLFunction *)fn ownerLibrary];
            if (!ownerLib) continue;
            BOOL allGraphsPresent = YES;
            for (MTLFunctionStitchingGraph *g in graphs) {
                if (g.functionName.length && ![ownerLib.functionNames containsObject:g.functionName]) {
                    allGraphsPresent = NO;
                    break;
                }
            }
            if (allGraphsPresent) return ownerLib;
        }
    }

    /* Real iOS 27 DAG stitching path (RenderBox custom_blend & CoreImage DAG kernels)
     * via Apple system MTLCompiler.framework request type 14 -> AIR MTLB -> A840MTLLibrary. */
    if (![device respondsToSelector:@selector(newDagStringWithGraphs:)]) {
        if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Device lacks newDagStringWithGraphs: SPI");
        return nil;
    }
    NSString *dag = [(id<A840DeviceDagStitchingSPI>)device newDagStringWithGraphs:graphs];
    if (!dag.length) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Stitching graphs produced empty DAG text");
        return nil;
    }

    NSMutableArray<NSData *> *modules = [NSMutableArray array];
    NSMutableArray<NSData *> *hashes = [NSMutableArray array];
    NSMutableSet<NSData *> *seen = [NSMutableSet set];
    for (id fn in functions) {
        if (![fn isKindOfClass:[A840MTLFunction class]]) {
            if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Stitched function is not an A840MTLFunction");
            return nil;
        }
        A840MTLFunction *aFn = static_cast<A840MTLFunction *>(fn);
        NSData *bitcode = [aFn extractBitcodeData];
        if (!bitcode || bitcode.length == 0u) {
            if (error) {
                *error = a840_metal_error(A840_RT_UNSUPPORTED,
                                          [NSString stringWithFormat:@"Stitched function %@ lacks AIR bitcode", aFn.name ?: @"?"]);
            }
            return nil;
        }
        if ([seen containsObject:bitcode]) continue;
        const void *hash = [aFn bitCodeHash];
        if (!hash) {
            if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Stitched function lacks bitcode hash");
            return nil;
        }
        [seen addObject:bitcode];
        [modules addObject:bitcode];
        [hashes addObject:[NSData dataWithBytes:hash length:CC_SHA256_DIGEST_LENGTH]];
    }

    std::vector<uint8_t> req;
    auto putWord = [&](uint32_t v) {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&v);
        req.insert(req.end(), b, b + 4u);
    };
    const char *dagUtf8 = dag.UTF8String;
    if (!dagUtf8 || !dagUtf8[0]) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Stitching DAG UTF8 conversion failed");
        return nil;
    }
    putWord(0x64616720u /* 'dag ' */);
    req.insert(req.end(), dagUtf8, dagUtf8 + std::strlen(dagUtf8) + 1u);
    putWord(0x6e756d66u /* 'numf' */);
    putWord(static_cast<uint32_t>(modules.count));
    for (NSUInteger i = 0u; i < modules.count; ++i) {
        NSData *bc = modules[i];
        const uint8_t *h = static_cast<const uint8_t *>(hashes[i].bytes);
        putWord(0x62686173u /* 'bhas' */);
        req.insert(req.end(), h, h + CC_SHA256_DIGEST_LENGTH);
        putWord(0x62697463u /* 'bitc' */);
        putWord(static_cast<uint32_t>(bc.length));
        const uint8_t *b = static_cast<const uint8_t *>(bc.bytes);
        req.insert(req.end(), b, b + bc.length);
    }
    /* iOS 27 MTLCompilerObject::stitchFunctionDag trailing slots: nums=0, ptfm=0, arid=0. */
    putWord(0x6e756d73u /* 'nums' */); putWord(0u);
    putWord(0x7074666du /* 'ptfm' */); putWord(0u);
    putWord(0x61726964u /* 'arid' */); putWord(0u);

    NSString *warnings = nil, *failure = nil;
    NSData *metallib = a840SystemCodeGenRequest(14u, req, &warnings, &failure);
    if (!metallib || metallib.length < 4u) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840STITCH fail g=%lu f=%lu %.52s",
                 (unsigned long)graphs.count, (unsigned long)functions.count,
                 failure.UTF8String ?: "err");
        a840diag::reportText(msg);
        if (error) {
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{NSLocalizedDescriptionKey: failure ?: @"Function stitching failed"}];
        }
        return nil;
    }
    return [[A840MTLLibrary alloc] initWithDevice:device data:metallib error:error];
}

@end
#pragma clang diagnostic pop
