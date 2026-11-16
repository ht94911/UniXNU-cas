/* SPDX-License-Identifier: MIT */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <ptrauth.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Original24A446 LSDatabaseBuilder is called inside
 * LSDBExecutionContext.beginServerDBBootstrap:, with the real server's
 * mutable database and per-thread context. LSApplicationWorkspace's
 * registerApplication: calls LSRegisterURL directly in this context.
 * Camera074's client-side modifydb service was absent. Camera075's class
 * LSDefaults accessor is forbidden;076 uses the original C singleton getter. Register only
 * the existing Camera here; keep the original database seed and jobs.
 * Camera077 supplies the original LSInstallInfo used by the native
 * post-install registrar. The BOOL workspace wrapper supplied nil and
 * discarded NSError. */
@interface NSObject (NezhaCameraLSOriginal)
+ (id)defaultWorkspace;
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (BOOL)isServer;
- (BOOL)isInstalled;
- (BOOL)isPlaceholder;
- (NSString *)applicationIdentifier;
- (NSURL *)bundleURL;
- (BOOL)registerApplication:(NSURL *)url;
- (id)initWithLegacyDictionaries:(NSArray *)dictionaries personasWithAttributes:(id)personas;
- (BOOL)hasInfoForBundleIdentifier:(NSString *)identifier;
@end

static IMP original_seed;
static intptr_t ls_slide;
static bool ls_present, installed;
static unsigned registration_attempted;
static id (*original_defaults)(void);
static bool (*original_register_url)(void *,NSURL *,uint32_t,id,NSError *__autoreleasing *);
static const uint8_t ls_uuid[16] = {
    0x51,0xe9,0xd1,0x4e,0x5d,0xb6,0x37,0x4f,
    0x99,0x08,0xc9,0x53,0x86,0xd5,0x4c,0x0d
};

static BOOL camera_installed(id proxy)
{
    return [proxy respondsToSelector:@selector(applicationIdentifier)] &&
        [[proxy applicationIdentifier] isEqualToString:@"com.apple.camera"] &&
        [proxy respondsToSelector:@selector(isInstalled)] && [proxy isInstalled] &&
        [proxy respondsToSelector:@selector(isPlaceholder)] && ![proxy isPlaceholder];
}

static void register_camera(id configuration, Class install_info, Class proxies, NSURL *url)
{
    if (__atomic_exchange_n(&registration_attempted,1u,__ATOMIC_RELAXED)) return;
    id before=[proxies applicationProxyForIdentifier:@"com.apple.camera"];
    const BOOL server=[configuration respondsToSelector:@selector(isServer)] && [configuration isServer];
    fprintf(stderr,"nezha-camera-ls: native bootstrap uid=%u server=%u installed_before=%u\n",
        (unsigned)geteuid(),(unsigned)server,(unsigned)camera_installed(before));
    if (!server || camera_installed(before)) return;
    NSError *error=nil;
    NSData *data=[NSData dataWithContentsOfURL:[url URLByAppendingPathComponent:@"Info.plist"]
        options:0 error:&error];
    NSDictionary *info=data ? [NSPropertyListSerialization propertyListWithData:data
        options:NSPropertyListImmutable format:NULL error:&error]:nil;
    if (![info isKindOfClass:[NSDictionary class]] ||
        ![info[@"CFBundleIdentifier"] isEqualToString:@"com.apple.camera"] ||
        ![info[@"CFBundleExecutable"] isEqualToString:@"Camera"] ||
        access([url URLByAppendingPathComponent:@"Camera"].fileSystemRepresentation,R_OK)) {
        fprintf(stderr,"nezha-camera-ls: original bundle unavailable error=%ld\n",(long)error.code);
        return;
    }
    NSDictionary *record=@{@"ApplicationType":@"System",@"BundleNameIsLocalized":@1,
        @"CFBundleIdentifier":@"com.apple.camera",@"CompatibilityState":@0,
        @"IsDeletable":@0,@"Path":url.path};
    id installation=[[install_info alloc] initWithLegacyDictionaries:@[record] personasWithAttributes:nil];
    if (![installation respondsToSelector:@selector(hasInfoForBundleIdentifier:)] ||
        ![installation hasInfoForBundleIdentifier:@"com.apple.camera"] || !original_register_url) {
        fputs("nezha-camera-ls: original Camera install info unavailable\n",stderr);
        return;
    }
    error=nil;
    /* LSMIResultRegistrantTrueDatabaseContext uses 0x02000001 with the
     * native install info for this same FindOrRegisterBundleNode path.
     * The URL registrar resolves the real mutable server database in the
     * original bootstrap context. */
    const bool accepted=original_register_url(NULL,url,0x02000001u,installation,&error);
    id after=[proxies applicationProxyForIdentifier:@"com.apple.camera"];
    NSURL *registered=[after respondsToSelector:@selector(bundleURL)] ? [after bundleURL]:nil;
    fprintf(stderr,"nezha-camera-ls: original Camera registration=%u installed_after=%u path=%s error=%ld domain=%s\n",
        (unsigned)accepted,(unsigned)camera_installed(after),registered.path.UTF8String ?: "<absent>",
        (long)error.code,error.domain.UTF8String ?: "<none>");
    if (error) {
        fprintf(stderr,"nezha-camera-ls: registration-detail=%s\n",error.description.UTF8String ?: "<absent>");
        NSError *underlying=error.userInfo[NSUnderlyingErrorKey];
        if ([underlying isKindOfClass:[NSError class]])
            fprintf(stderr,"nezha-camera-ls: registration-underlying=%s\n",
                underlying.description.UTF8String ?: "<absent>");
    }
}

static void seed_database(id self,SEL selector,BOOL *changed)
{
    /* v24@0:8^B16 in the selected original method list: this returns void.
     * Preserve the seed's exact output pointer and perform registration
     * while its caller still holds the original server bootstrap context. */
    static unsigned reported;
    if (!__atomic_exchange_n(&reported,1u,__ATOMIC_RELAXED))
        fputs("nezha-camera-ls: original database seed entered\n",stderr);
    ((void (*)(id,SEL,BOOL *))original_seed)(self,selector,changed);
    @autoreleasepool {
        register_camera(original_defaults ? original_defaults():nil,objc_getClass("LSInstallInfo"),
            objc_getClass("LSApplicationProxy"),
            [NSURL fileURLWithPath:@"/Applications/Camera.app" isDirectory:YES]);
    }
}

static bool image_matches(const struct mach_header_64 *header)
{
    if (!header || header->magic!=MH_MAGIC_64 || header->ncmds>256 || header->sizeofcmds>65536) return false;
    const uint8_t *cursor=(const uint8_t *)(header+1),*end=cursor+header->sizeofcmds;
    for (unsigned i=0;i<header->ncmds;++i) {
        if ((size_t)(end-cursor)<sizeof(struct load_command)) return false;
        const struct load_command *command=(const void *)cursor;
        if (command->cmdsize<sizeof(*command) || command->cmdsize>(size_t)(end-cursor)) return false;
        if (command->cmd==LC_UUID)
            return command->cmdsize>=sizeof(struct uuid_command) &&
                !memcmp(((const struct uuid_command *)command)->uuid,ls_uuid,sizeof(ls_uuid));
        cursor+=command->cmdsize;
    }
    return false;
}

static void image_arrived(const struct mach_header *header,intptr_t slide)
{
    if (installed) return;
    if (image_matches((const struct mach_header_64 *)header)) { ls_present=true;ls_slide=slide; }
    if (!ls_present) return;
    Class builder=objc_getClass("LSDatabaseBuilder"),workspace=objc_getClass("LSApplicationWorkspace");
    Class defaults=objc_getClass("_LSDefaults"),proxies=objc_getClass("LSApplicationProxy");
    Class install_info=objc_getClass("LSInstallInfo");
    Method method=builder ? class_getInstanceMethod(builder,sel_registerName("createAndSeedLocalDatabase:")):NULL;
    Method registration=workspace ? class_getInstanceMethod(workspace,sel_registerName("registerApplication:")):NULL;
    IMP seed=method ? method_getImplementation(method):NULL;
    IMP request=registration ? method_getImplementation(registration):NULL;
    Method info_method=install_info ? class_getInstanceMethod(install_info,
        sel_registerName("initWithLegacyDictionaries:personasWithAttributes:")):NULL;
    IMP info_constructor=info_method ? method_getImplementation(info_method):NULL;
    if (!seed || !request || !defaults || !proxies || !info_constructor ||
        ![workspace respondsToSelector:@selector(defaultWorkspace)] ||
        ![proxies respondsToSelector:@selector(applicationProxyForIdentifier:)] ||
        (uintptr_t)ptrauth_strip(seed,ptrauth_key_function_pointer)!=(uintptr_t)(0x186ec695cULL+ls_slide) ||
        (uintptr_t)ptrauth_strip(request,ptrauth_key_function_pointer)!=(uintptr_t)(0x186f85c70ULL+ls_slide) ||
        (uintptr_t)ptrauth_strip(info_constructor,ptrauth_key_function_pointer)!=(uintptr_t)(0x186f5ad48ULL+ls_slide) ||
        !method_getTypeEncoding(method) || !method_getTypeEncoding(registration) ||
        strcmp(method_getTypeEncoding(method),"v24@0:8^B16") ||
        strcmp(method_getTypeEncoding(registration),"B24@0:8@16") ||
        !method_getTypeEncoding(info_method) ||
        strcmp(method_getTypeEncoding(info_method),"@32@0:8@16@24")) return;
    /* Actual075 +[_LSDefaults sharedInstance] invokes the original
     * forbidden-ls-internal-symbol-invoked-directly protection. Original
     * LSServerMain and ContextInit use this private C singleton getter,
     * which reads the same initialized object without the class accessor.
     * Only bind this measured original sequence after UUID/method checks. */
    const uintptr_t getter=(uintptr_t)(0x186dce8d4ULL+ls_slide);
    static const uint8_t getter_code[28] = {
        0x88,0x0a,0x30,0xf0,0x08,0xf1,0x42,0xf9,0x1f,0x05,0x00,0xb1,0x81,0x00,
        0x00,0x54,0x88,0x0a,0x30,0xf0,0x00,0x01,0x43,0xf9,0xc0,0x03,0x5f,0xd6
    };
    if (memcmp((const void *)getter,getter_code,sizeof(getter_code))) return;
    const uintptr_t register_url=(uintptr_t)(0x186f368b0ULL+ls_slide);
    static const uint8_t register_url_code[32]={
        0x7f,0x23,0x03,0xd5,0xf8,0x5f,0xbc,0xa9,0xf6,0x57,0x01,0xa9,0xf4,0x4f,0x02,0xa9,
        0xfd,0x7b,0x03,0xa9,0xfd,0xc3,0x00,0x91,0xf5,0x03,0x04,0xaa,0xf4,0x03,0x03,0xaa
    };
    if (memcmp((const void *)register_url,register_url_code,sizeof(register_url_code))) return;
    original_defaults=(id (*)(void))ptrauth_sign_unauthenticated(
        (void *)getter,ptrauth_key_function_pointer,0);
    original_register_url=(bool (*)(void *,NSURL *,uint32_t,id,NSError *__autoreleasing *))
        ptrauth_sign_unauthenticated((void *)register_url,ptrauth_key_function_pointer,0);
    original_seed=seed;
    method_setImplementation(method,(IMP)seed_database);
    installed=true;
    fputs("nezha-camera-ls: original database bootstrap registration installed\n",stderr);
}

__attribute__((constructor)) static void initialize_camera_ls(void)
{
    const char *name=getprogname();
    /* Kernel environment/dyld policy additionally binds the original
     * com.apple.lsd signature,CDHash,launchd parent and explicit mode6. */
    if (!name || strcmp(name,"lsd") || geteuid()!=501) return;
    setvbuf(stderr,NULL,_IONBF,0);
    _dyld_register_func_for_add_image(image_arrived);
}
