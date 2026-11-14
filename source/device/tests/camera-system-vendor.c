/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <objc/runtime.h>
static const char *process_name="test";
const char *getprogname(void) {return process_name;}
#define access model_test_access
#include "../package/camera/userland/NezhaCameraSystemVendor.m"

static bool class_available;
static unsigned replacements,original_calls;
static int backend_status;
static unsigned model_original_calls;
static bool bundle_available,model_readable=true;
static CameraCreate seen_factory;
static void (*arrived)(const struct mach_header *,intptr_t);
struct TestMethod {IMP implementation;};
static id vendor_original(id self,SEL selector,CameraCreate factory)
{ assert(self==(id)1 && selector==(SEL)2);++original_calls;seen_factory=factory;return (id)3; }
static struct TestMethod method={(IMP)vendor_original};
static id resource_original(id self,SEL selector,id resource,id extension,id directory)
{assert(self && selector==(SEL)3 && resource && directory);(void)extension;++model_original_calls;return (id)7;}
static struct TestMethod resource_method={(IMP)resource_original};
Class objc_getClass(const char *name) {
 if (!strcmp(name,"BWFigCaptureDeviceVendor"))return class_available ? (Class)1:NULL;
 if (!strcmp(name,"NSBundle"))return bundle_available ? (Class)4:NULL;
 assert(!strcmp(name,"NSURL"));return (Class)5;
}
SEL sel_registerName(const char *name) {
 if (!strcmp(name,selector_name))return (SEL)2;
 if (!strcmp(name,model_selector_name))return (SEL)3;
 if (!strcmp(name,"bundleIdentifier"))return (SEL)4;
 if (!strcmp(name,"isEqualToString:"))return (SEL)5;
 assert(!strcmp(name,"fileURLWithPath:"));return (SEL)6;
}
Method class_getClassMethod(Class cls,SEL selector) {assert(cls==(Class)1 && selector==(SEL)2);return &method;}
Method class_getInstanceMethod(Class cls,SEL selector) {assert(cls==(Class)4 && selector==(SEL)3);return &resource_method;}
IMP method_getImplementation(Method m) {return m->implementation;}
IMP method_setImplementation(Method m,IMP replacement) {IMP old=m->implementation;m->implementation=replacement;++replacements;return old;}
CFStringRef nezha_test_cfstr(const char *s) {return (CFStringRef)s;}
int model_test_access(const char *path,int flags) {assert(!strcmp(path,"/private/var/ngpu/camera/NezhaCameraModel.plist") && flags==R_OK);return model_readable ? 0:-1;}
/* Real objc_msgSend is an assembly dispatch entry, cast to each method's
 * typed ABI. The fixture's variadic C entry must not attach one function
 * sanitizer signature to those different, legitimate dispatch calls. */
__attribute__((no_sanitize("function"))) id objc_msgSend(id self,SEL selector,...)
{
 va_list args;va_start(args,selector);id result=NULL;
 if (selector==(SEL)4)result=self; /* bundle fixture is its identifier */
 else if (selector==(SEL)5)result=(id)(uintptr_t)(strcmp(self,va_arg(args,const char *))==0);
 else {assert(self==(Class)5 && selector==(SEL)6);assert(!strcmp(va_arg(args,const char *),"/private/var/ngpu/camera/NezhaCameraModel.plist"));result=(id)9;}
 va_end(args);return result;
}
void _dyld_register_func_for_add_image(void (*callback)(const struct mach_header *,intptr_t)) {arrived=callback;}
int nezha_camera_device_backend_bind(void) {return backend_status;}
NezhaFigStatus nezha_camera_device_create(NezhaFigAllocator a,NezhaFigDictionary o,NezhaFigObject *d)
{ (void)a;(void)o;(void)d;return 0; }
int main(void)
{
 process_name="SpringBoard";initialize();assert(!arrived);
 process_name="cameracaptured";initialize();assert(arrived);
 struct {struct mach_header_64 h;struct uuid_command u;} image={.h={.magic=MH_MAGIC_64,.ncmds=1,.sizeofcmds=sizeof(struct uuid_command)},.u={.cmd=LC_UUID,.cmdsize=sizeof(struct uuid_command)}};
 intptr_t slide=(intptr_t)(uintptr_t)vendor_original-(intptr_t)0x1b05ce8e4ULL;
 arrived((const struct mach_header *)&image,slide);assert(!replacements);
 memcpy(image.u.uuid,capture_uuid,16);
 arrived((const struct mach_header *)&image,slide);assert(capture_present && !replacements);
 class_available=true;
 struct mach_header_64 other={0};
 arrived((const struct mach_header *)&other,0);assert(replacements==1);
 id (*called)(id,SEL,CameraCreate)=(void *)method.implementation;
 assert(called((id)1,(SEL)2,NULL)==(id)3 && seen_factory==nezha_camera_device_create);
 backend_status=-1;
 assert(called((id)1,(SEL)2,NULL)==(id)3 && !seen_factory && original_calls==2);
 arrived((const struct mach_header *)&image,slide);assert(replacements==1);
 bundle_available=true;capture_slide=(intptr_t)(uintptr_t)resource_original-(intptr_t)0x180b9cf90ULL;
 install_model_resource();assert(model_installed && replacements==2);
 id (*lookup)(id,SEL,id,id,id)=(void *)resource_method.implementation;
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"AVCaptureSession.plist",NULL,(id)"D16")== (id)9);
 assert(!model_original_calls);
 assert(lookup((id)"com.apple.CoreMedia",(SEL)3,(id)"AVCaptureSession.plist",NULL,(id)"D16")== (id)7);
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"CameraParameters.plist",NULL,(id)"D16")== (id)7);
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"AVCaptureSession.plist",(id)"plist",(id)"D16")== (id)7);
 model_readable=false;
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"AVCaptureSession.plist",NULL,(id)"D16")== (id)7);
 assert(model_original_calls==4);
 puts("Camera system vendor: exact process/UUID/method, delayed class arrival, factory replacement and original fallback passed");
 puts("Camera model resource: exact camera bundle/file only, original unrelated resource and unavailable-model fallback passed");
}
