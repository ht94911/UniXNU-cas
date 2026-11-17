/* SPDX-License-Identifier: MIT */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <ptrauth.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
- (BOOL)hasServer;
- (BOOL)isInSyncBubble;
- (BOOL)isLightweightSystemServer;
- (BOOL)isInstalled;
- (BOOL)isPlaceholder;
- (NSString *)applicationIdentifier;
- (NSURL *)bundleURL;
- (BOOL)registerApplication:(NSURL *)url;
- (id)initWithLegacyDictionaries:(NSArray *)dictionaries personasWithAttributes:(id)personas;
- (BOOL)hasInfoForBundleIdentifier:(NSString *)identifier;
- (NSString *)serviceName;
- (int)processIdentifier;
- (id)XPCConnection;
+ (unsigned short)connectionType;
@end

static IMP original_seed,original_completion,original_listener_init,original_listener_accept,original_listener_resume;
static IMP original_read_queries,original_read_store,original_rebuilt;
static intptr_t ls_slide;
static bool ls_present, installed,listener_observer_installed,listener_accept_installed,listener_resume_installed;
static bool read_observers_installed;
static unsigned registration_attempted,completion_registration_attempted;
static id (*original_defaults)(void);
static bool (*original_register_url)(void *,NSURL *,uint32_t,id,NSError *__autoreleasing *);
static const uint8_t ls_uuid[16] = {
    0x51,0xe9,0xd1,0x4e,0x5d,0xb6,0x37,0x4f,
    0x99,0x08,0xc9,0x53,0x86,0xd5,0x4c,0x0d
};
typedef id (*LSListenerInit)(id __attribute__((ns_consumed)),SEL,id)
    __attribute__((ns_returns_retained));

static BOOL camera_installed(id proxy)
{
    return [proxy respondsToSelector:@selector(applicationIdentifier)] &&
        [[proxy applicationIdentifier] isEqualToString:@"com.apple.camera"] &&
        [proxy respondsToSelector:@selector(isInstalled)] && [proxy isInstalled] &&
        [proxy respondsToSelector:@selector(isPlaceholder)] && ![proxy isPlaceholder];
}

static BOOL register_camera(id configuration, Class install_info, Class proxies, NSURL *url,unsigned *attempted)
{
    if (__atomic_exchange_n(attempted,1u,__ATOMIC_RELAXED)) return NO;
    id before=[proxies applicationProxyForIdentifier:@"com.apple.camera"];
    const BOOL server=[configuration respondsToSelector:@selector(isServer)] && [configuration isServer];
    fprintf(stderr,"nezha-camera-ls: native bootstrap uid=%u server=%u installed_before=%u\n",
        (unsigned)geteuid(),(unsigned)server,(unsigned)camera_installed(before));
    if (!server || camera_installed(before)) return NO;
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
        return NO;
    }
    NSDictionary *record=@{@"ApplicationType":@"System",@"BundleNameIsLocalized":@1,
        @"CFBundleIdentifier":@"com.apple.camera",@"CompatibilityState":@0,
        @"IsDeletable":@0,@"Path":url.path};
    id installation=[[install_info alloc] initWithLegacyDictionaries:@[record] personasWithAttributes:nil];
    if (![installation respondsToSelector:@selector(hasInfoForBundleIdentifier:)] ||
        ![installation hasInfoForBundleIdentifier:@"com.apple.camera"] || !original_register_url) {
        fputs("nezha-camera-ls: original Camera install info unavailable\n",stderr);
        return NO;
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
    return accepted;
}

static void report_server_state(const char *stage)
{
    id configuration=original_defaults ? original_defaults():nil;
    fprintf(stderr,"nezha-camera-ls: %s uid=%u server=%u hasServer=%u syncBubble=%u lightweight=%u\n",
        stage,(unsigned)geteuid(),(unsigned)[configuration isServer],
        (unsigned)[configuration hasServer],(unsigned)[configuration isInSyncBubble],
        (unsigned)[configuration isLightweightSystemServer]);
}

/* Actual080 has real incoming LS clients and a locally installed Camera.
 * Record the original read replies and rebuilt publication; do not send a
 * second notification, synthesize a record, or change database selection. */
static int read_client_pid(id client)
{
    id connection=[client respondsToSelector:@selector(XPCConnection)] ? [client XPCConnection]:nil;
    return [connection respondsToSelector:@selector(processIdentifier)] ? [connection processIdentifier]:0;
}

static void read_queries(id self,SEL selector,id queries,BOOL legacy,id completion)
{
    static unsigned reported,board_reported;
    const int pid=read_client_pid(self);
    const bool report=__atomic_fetch_add(pid==30 ? &board_reported:&reported,1u,__ATOMIC_RELAXED)<12;
    if (report) fprintf(stderr,"nezha-camera-ls: original read query entered pid=%d queries=%lu legacy=%u completion=%u\n",
        pid,(unsigned long)[queries count],(unsigned)legacy,(unsigned)(completion!=nil));
    /* The original resolveQueries reply passes its dictionary and NSError
     * in x1/x2. The copied native callback remains owned by the wrapper. */
    id reply=report && completion ? (id)^(id results,NSError *error) {
        fprintf(stderr,"nezha-camera-ls: original read query reply pid=%d results=%u count=%lu error=%ld domain=%s\n",
            pid,(unsigned)(results!=nil),(unsigned long)[results count],(long)error.code,
            error.domain.UTF8String ?: "<none>");
        ((void (^)(id,NSError *))completion)(results,error);
    }:completion;
    ((void (*)(id,SEL,id,BOOL,id))original_read_queries)(self,selector,queries,legacy,reply);
}

static void read_store(id self,SEL selector,id completion)
{
    static unsigned reported,board_reported;
    const int pid=read_client_pid(self);
    const bool report=__atomic_fetch_add(pid==30 ? &board_reported:&reported,1u,__ATOMIC_RELAXED)<12;
    if (report) fprintf(stderr,"nezha-camera-ls: original read store entered pid=%d completion=%u\n",
        pid,(unsigned)(completion!=nil));
    /* Native callback forwards store, catalogue, blocking endpoint, manual
     * rebuild BOOL and NSError. Preserve all five values and nil callback. */
    id reply=report && completion ? (id)^(id store,id catalogue,id endpoint,BOOL manual,NSError *error) {
        fprintf(stderr,"nezha-camera-ls: original read store reply pid=%d store=%u catalogue=%u endpoint=%u manual=%u error=%ld domain=%s\n",
            pid,(unsigned)(store!=nil),(unsigned)(catalogue!=nil),(unsigned)(endpoint!=nil),
            (unsigned)manual,(long)error.code,error.domain.UTF8String ?: "<none>");
        ((void (^)(id,id,id,BOOL,NSError *))completion)(store,catalogue,endpoint,manual,error);
    }:completion;
    ((void (*)(id,SEL,id))original_read_store)(self,selector,reply);
}

static void database_rebuilt(id self,SEL selector)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) fputs("nezha-camera-ls: original database rebuilt notification entered\n",stderr);
    ((void (*)(id,SEL))original_rebuilt)(self,selector);
    if (report) fputs("nezha-camera-ls: original database rebuilt notification request returned\n",stderr);
}

static void install_read_observers(void)
{
    if (!ls_present || read_observers_installed) return;
    Class reader=objc_getClass("_LSDReadClient"),progress=objc_getClass("_LSInstallProgressService");
    struct {Class type;const char *selector,*encoding;uintptr_t address;IMP replacement;IMP *original;} rows[]={
        {reader,"resolveQueries:legacySPI:completionHandler:","v36@0:8@16B24@?28",0x186ef40e0ULL,(IMP)read_queries,&original_read_queries},
        {reader,"getServerStoreNonBlockingWithCompletionHandler:","v24@0:8@?16",0x186df65b0ULL,(IMP)read_store,&original_read_store},
        {progress,"sendDatabaseRebuiltNotification","v16@0:8",0x186f74180ULL,(IMP)database_rebuilt,&original_rebuilt},
    };
    Method methods[3];
    for (unsigned i=0;i<3;++i) {
        methods[i]=rows[i].type ? class_getInstanceMethod(rows[i].type,sel_registerName(rows[i].selector)):NULL;
        IMP current=methods[i] ? method_getImplementation(methods[i]):NULL;
        const char *encoding=methods[i] ? method_getTypeEncoding(methods[i]):NULL;
        if (!current || !encoding || strcmp(encoding,rows[i].encoding) ||
            (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)!=(uintptr_t)(rows[i].address+ls_slide)) return;
    }
    for (unsigned i=0;i<3;++i) {
        *rows[i].original=method_getImplementation(methods[i]);
        method_setImplementation(methods[i],rows[i].replacement);
    }
    read_observers_installed=true;
    fputs("nezha-camera-ls: original read/rebuilt observers installed\n",stderr);
}

static bool ls_service_name(NSString *name)
{
    return [name hasPrefix:@"com.apple.lsd."];
}

static void report_listener(const char *stage,id listener)
{
    NSString *name=[listener respondsToSelector:@selector(serviceName)] ? [listener serviceName]:nil;
    if (!ls_service_name(name)) return;
    fprintf(stderr,"nezha-camera-ls: original listener %s name=%s uid=%u service_base=%s\n",
        stage,name.UTF8String,(unsigned)geteuid(),getenv("LSD_SERVICE_BASE") ?: "<unset>");
    /* Native Foundation stores its real libxpc listener connection here.
     * A created Objective-C listener is not proof of a successful check-in. */
    Class native=objc_getClass("NSXPCListener");
    Ivar connection_ivar=native ? class_getInstanceVariable(native,"_connection"):NULL;
    char *(*describe)(const void *)=(char *(*)(const void *))dlsym(RTLD_DEFAULT,"xpc_copy_description");
    if (object_getClass(listener)==native && connection_ivar && ivar_getOffset(connection_ivar)==8 && describe) {
        const void *connection=NULL;
        memcpy(&connection,(const uint8_t *)(__bridge const void *)listener+8,sizeof(connection));
        char *description=connection ? describe(connection):NULL;
        fprintf(stderr,"nezha-camera-ls: original listener connection %s %.512s\n",
            stage,description ?: "<absent>");
        free(description);
    }
}

static void listener_resume(id self,SEL selector)
{
    static unsigned reported;
    NSString *name=[self respondsToSelector:@selector(serviceName)] ? [self serviceName]:nil;
    const bool report=ls_service_name(name) && __atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<16;
    if (report) report_listener("resume entered",self);
    ((void (*)(id,SEL))original_listener_resume)(self,selector);
    if (report) report_listener("resume returned",self);
}

static BOOL listener_accept(id self,SEL selector,id listener,id connection)
{
    const BOOL accepted=((BOOL (*)(id,SEL,id,id))original_listener_accept)(self,selector,listener,connection);
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<24) {
        NSString *name=[listener respondsToSelector:@selector(serviceName)] ? [listener serviceName]:nil;
        fprintf(stderr,"nezha-camera-ls: original incoming connection service=%s pid=%d accepted=%u\n",
            name.UTF8String ?: "<absent>",[connection processIdentifier],(unsigned)accepted);
    }
    return accepted;
}

static id __attribute__((ns_returns_retained)) listener_init(id __attribute__((ns_consumed)) self,SEL selector,id listener)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<16;
    const char *name=report ? class_getName(object_getClass(self)):NULL;
    if (report) fprintf(stderr,"nezha-camera-ls: original listener enter class=%s listener=%u\n",
        name,(unsigned)(listener!=nil));
    if (report) report_listener("initialized",listener);
    id service=((LSListenerInit)original_listener_init)(self,selector,listener);
    if (report) fprintf(stderr,"nezha-camera-ls: original listener returned class=%s created=%u\n",
        name,(unsigned)(service!=nil));
    return service;
}

/* The native BOOL* reports database changes to LSServerMain. Registration
 * occurs after the original builder has set that output, so report the
 * additional accepted registration without clearing a native change. */
static void publish_registration_change(BOOL *changed,BOOL registered)
{
    if (changed && registered) *changed=YES;
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
        const BOOL registered=register_camera(original_defaults ? original_defaults():nil,objc_getClass("LSInstallInfo"),
            objc_getClass("LSApplicationProxy"),
            [NSURL fileURLWithPath:@"/Applications/Camera.app" isDirectory:YES],&registration_attempted);
        publish_registration_change(changed,registered);
        fprintf(stderr,"nezha-camera-ls: original seed returned registration_changed=%u native_changed=%u\n",
            (unsigned)registered,(unsigned)(changed && *changed));
        report_server_state("bootstrap state");
    }
}

static void complete_database(id self,SEL selector,BOOL complete)
{
    /* Actual077 registered inside the initial bootstrap, but app clients
     * still saw no entry. Original seeding then synchronizes with MI and
     * may replace that database content asynchronously. This callback runs
     * in the original syncWrite context before native commit/save/notify.
     * Recheck the real entry here and register only if it is absent; keep
     * the native completion value and all its publication operations. */
    @autoreleasepool {
        fprintf(stderr,"nezha-camera-ls: original seed completion complete=%u\n",(unsigned)complete);
        if (complete) register_camera(original_defaults ? original_defaults():nil,
            objc_getClass("LSInstallInfo"),objc_getClass("LSApplicationProxy"),
            [NSURL fileURLWithPath:@"/Applications/Camera.app" isDirectory:YES],&completion_registration_attempted);
    }
    ((void (*)(id,SEL,BOOL))original_completion)(self,selector,complete);
    fputs("nezha-camera-ls: original seed completion returned\n",stderr);
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
    if (installed && listener_observer_installed && listener_accept_installed && listener_resume_installed && read_observers_installed) return;
    if (image_matches((const struct mach_header_64 *)header)) { ls_present=true;ls_slide=slide; }
    if (!ls_present) return;
    install_read_observers();
    /* Foundation's native listener has a serviceName and resumes its real
     * libxpc connection. Observe that boundary only inside original lsd.
     * No new listener, endpoint, privilege option or bootstrap domain. */
    if (!listener_resume_installed) {
        Class listener=objc_getClass("NSXPCListener");
        Method method=listener ? class_getInstanceMethod(listener,sel_registerName("resume")):NULL;
        IMP resume=method ? method_getImplementation(method):NULL;
        const char *encoding=method ? method_getTypeEncoding(method):NULL;
        if (resume && encoding && !strcmp(encoding,"v16@0:8") &&
            (uintptr_t)ptrauth_strip(resume,ptrauth_key_function_pointer)==(uintptr_t)(0x180d32f60ULL+ls_slide)) {
            original_listener_resume=resume;method_setImplementation(method,(IMP)listener_resume);
            listener_resume_installed=true;
        }
    }
    if (!listener_accept_installed) {
        Class service=objc_getClass("_LSDService");
        Method method=service ? class_getInstanceMethod(service,sel_registerName("listener:shouldAcceptNewConnection:")):NULL;
        IMP accept=method ? method_getImplementation(method):NULL;
        const char *encoding=method ? method_getTypeEncoding(method):NULL;
        if (accept && encoding && !strcmp(encoding,"B32@0:8@16@24") &&
            (uintptr_t)ptrauth_strip(accept,ptrauth_key_function_pointer)==(uintptr_t)(0x186e04368ULL+ls_slide)) {
            original_listener_accept=accept;method_setImplementation(method,(IMP)listener_accept);
            listener_accept_installed=true;
        }
    }
    if (!listener_observer_installed) {
        Class service=objc_getClass("_LSDService");
        Method method=service ? class_getInstanceMethod(service,sel_registerName("initWithXPCListener:")):NULL;
        IMP initializer=method ? method_getImplementation(method):NULL;
        const char *encoding=method ? method_getTypeEncoding(method):NULL;
        if (initializer && encoding && !strcmp(encoding,"@24@0:8@16") &&
            (uintptr_t)ptrauth_strip(initializer,ptrauth_key_function_pointer)==(uintptr_t)(0x186ef91ecULL+ls_slide)) {
            original_listener_init=initializer;
            method_setImplementation(method,(IMP)listener_init);
            listener_observer_installed=true;
            fputs("nezha-camera-ls: original service listener observer installed\n",stderr);
        }
    }
    if (installed) return;
    Class builder=objc_getClass("LSDatabaseBuilder"),workspace=objc_getClass("LSApplicationWorkspace");
    Class defaults=objc_getClass("_LSDefaults"),proxies=objc_getClass("LSApplicationProxy");
    Class install_info=objc_getClass("LSInstallInfo");
    Method method=builder ? class_getInstanceMethod(builder,sel_registerName("createAndSeedLocalDatabase:")):NULL;
    Method completion=builder ? class_getInstanceMethod(builder,sel_registerName("setSeedingComplete:")):NULL;
    Method registration=workspace ? class_getInstanceMethod(workspace,sel_registerName("registerApplication:")):NULL;
    IMP seed=method ? method_getImplementation(method):NULL;
    IMP publish=completion ? method_getImplementation(completion):NULL;
    IMP request=registration ? method_getImplementation(registration):NULL;
    Method info_method=install_info ? class_getInstanceMethod(install_info,
        sel_registerName("initWithLegacyDictionaries:personasWithAttributes:")):NULL;
    IMP info_constructor=info_method ? method_getImplementation(info_method):NULL;
    if (!seed || !publish || !request || !defaults || !proxies || !info_constructor ||
        ![workspace respondsToSelector:@selector(defaultWorkspace)] ||
        ![proxies respondsToSelector:@selector(applicationProxyForIdentifier:)] ||
        (uintptr_t)ptrauth_strip(seed,ptrauth_key_function_pointer)!=(uintptr_t)(0x186ec695cULL+ls_slide) ||
        (uintptr_t)ptrauth_strip(publish,ptrauth_key_function_pointer)!=(uintptr_t)(0x186ec72ecULL+ls_slide) ||
        (uintptr_t)ptrauth_strip(request,ptrauth_key_function_pointer)!=(uintptr_t)(0x186f85c70ULL+ls_slide) ||
        (uintptr_t)ptrauth_strip(info_constructor,ptrauth_key_function_pointer)!=(uintptr_t)(0x186f5ad48ULL+ls_slide) ||
        !method_getTypeEncoding(method) || !method_getTypeEncoding(registration) ||
        strcmp(method_getTypeEncoding(method),"v24@0:8^B16") ||
        !method_getTypeEncoding(completion) || strcmp(method_getTypeEncoding(completion),"v20@0:8B16") ||
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
    original_completion=publish;
    method_setImplementation(method,(IMP)seed_database);
    method_setImplementation(completion,(IMP)complete_database);
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
