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
struct TestIvar {ptrdiff_t offset;};
static struct {uintptr_t header;CameraCreate factory;} retained_vendor;
static struct TestIvar factory_ivar={8};
static bool wrong_vendor_class;
static bool sources_available;
static struct {uintptr_t header;id backings;uint8_t shared;} source_manager;
static struct TestIvar manager_backings_ivar={8},manager_shared_ivar={16};
static bool wrong_sources_encoding;
static unsigned sources_calls;
static CameraAuditToken expected_audit;
static id sources_result;
static struct TestMethod source_init_method;
static unsigned source_initializations,backing_references;
static id expected_init_backings;
static bool expected_init_shared;
static id source_init_original(id self,SEL selector,id backings,bool thermal,bool calibration,bool shared)
{assert(self==&source_manager && selector==(SEL)27 && backings==expected_init_backings &&
 thermal && calibration && shared==expected_init_shared);++source_initializations;return self;}
static id sources_original(id self,SEL selector,int32_t type,CameraAuditToken audit,bool bincompat)
{assert(self==&source_manager && selector==(SEL)17 && type==1 && bincompat);
 assert(!memcmp(&audit,&expected_audit,sizeof(audit)));++sources_calls;return sources_result;}
static struct TestMethod sources_method={(IMP)sources_original};
static id vendor_original(id self,SEL selector,CameraCreate factory)
{ assert(self==(id)1 && selector==(SEL)2);++original_calls;seen_factory=factory;retained_vendor.factory=factory;return &retained_vendor; }
static struct TestMethod method={(IMP)vendor_original};
static id resource_original(id self,SEL selector,id resource,id extension,id directory)
{assert(self && selector==(SEL)3 && resource);(void)extension;(void)directory;++model_original_calls;return (id)7;}
static struct TestMethod resource_method={(IMP)resource_original};
static unsigned list_calls;
static id ids_original(id self,SEL selector)
{assert(self==&retained_vendor && selector==(SEL)7);++list_calls;return (id)11;}
static struct TestMethod list_method={(IMP)ids_original};
static unsigned publishing_calls;
static id publishing_original(id self,SEL selector,id device_id,int32_t *error)
{assert(self==&retained_vendor && selector==(SEL)14 && !strcmp(device_id,"Default"));++publishing_calls;if(error)*error=-12780;return NULL;}
static struct TestMethod publishing_method={(IMP)publishing_original};
static unsigned stream_request_calls;
static bool publish_success;
static id publishing_succeeded(id self,SEL selector,id device_id,int32_t *error)
{assert(publish_success && self==&retained_vendor && selector==(SEL)14 && !strcmp(device_id,"Default"));if(error)*error=0;return (id)22;}
static id streams_original(id self,SEL selector,id device,id positions,id types,int32_t priority,bool loss,int32_t *error)
{assert(self==&retained_vendor && selector==(SEL)15 && device==(id)22 && positions==(id)11 && types==(id)11 && priority==3 && !loss);++stream_request_calls;if(error)*error=-12783;return NULL;}
static struct TestMethod streams_method={(IMP)streams_original};
static bool backings_available;
static unsigned backing_inits,backing_lists;
static id backings_init_original(id self,SEL selector,id input,id common)
{assert(self==(id)20 && selector==(SEL)12 && input==(id)11 && common==(id)18);++backing_inits;return (id)21;}
static id backings_list_original(id self,SEL selector)
{assert(self==(id)21 && selector==(SEL)13);++backing_lists;return (id)11;}
static struct TestMethod backings_init_method={(IMP)backings_init_original};
static struct TestMethod backings_list_method={(IMP)backings_list_original};
Ivar class_getInstanceVariable(Class cls,const char *name)
{
 if(cls==(Class)24 && !strcmp(name,"_captureSourceBackings"))return &manager_backings_ivar;
 if(cls==(Class)24 && !strcmp(name,"_useSharedCaptureSourceBackingsProvider"))return &manager_shared_ivar;
 assert(cls==(Class)1 && !strcmp(name,"_defaultDeviceCreateFunction"));return &factory_ivar;
}
Class object_getClass(id value)
{if(value==&source_manager)return (Class)24;
 assert(value==&retained_vendor);return (Class)(uintptr_t)(wrong_vendor_class ? 2:1);}
ptrdiff_t ivar_getOffset(Ivar value) {return value->offset;}
Class objc_getClass(const char *name) {
 if (!strcmp(name,"BWFigCaptureDeviceVendor"))return class_available ? (Class)1:NULL;
 if (!strcmp(name,"NSBundle"))return bundle_available ? (Class)4:NULL;
 if (!strcmp(name,"FigCaptureSourceBackingsProvider"))return backings_available ? (Class)16:NULL;
 if (!strcmp(name,"FigCaptureSourceManager"))return sources_available ? (Class)24:NULL;
 if (!strcmp(name,"NSDictionary"))return (Class)17;
 assert(!strcmp(name,"NSURL"));return (Class)5;
}
SEL sel_registerName(const char *name) {
 if (!strcmp(name,selector_name))return (SEL)2;
 if (!strcmp(name,model_selector_name))return (SEL)3;
 if (!strcmp(name,"bundleIdentifier"))return (SEL)4;
 if (!strcmp(name,"isEqualToString:"))return (SEL)5;
 if (!strcmp(name,device_ids_selector_name))return (SEL)7;
 if (!strcmp(name,"count"))return (SEL)8;
 if (!strcmp(name,"containsObject:"))return (SEL)9;
 if (!strcmp(name,"dictionaryWithContentsOfFile:"))return (SEL)10;
 if (!strcmp(name,"objectForKey:"))return (SEL)11;
 if (!strcmp(name,backings_init_selector))return (SEL)12;
 if (!strcmp(name,backings_list_selector))return (SEL)13;
 if (!strcmp(name,publishing_selector))return (SEL)14;
 if (!strcmp(name,stream_request_selector))return (SEL)15;
 if (!strcmp(name,"streams"))return (SEL)16;
 if (!strcmp(name,sources_selector))return (SEL)17;
 if (!strcmp(name,"sharedCaptureSourceBackingsProvider"))return (SEL)18;
 if (!strcmp(name,"objectAtIndex:"))return (SEL)19;
 if (!strcmp(name,"attributes"))return (SEL)20;
 if (!strcmp(name,"formats"))return (SEL)21;
 if (!strcmp(name,"missingFormatNames"))return (SEL)22;
 if (!strcmp(name,"mediaType"))return (SEL)23;
 if (!strcmp(name,"deviceType"))return (SEL)24;
 if (!strcmp(name,"position"))return (SEL)25;
 if (!strcmp(name,"uniqueID"))return (SEL)26;
 if (!strcmp(name,source_init_selector))return (SEL)27;
 assert(!strcmp(name,"fileURLWithPath:"));return (SEL)6;
}
Method class_getClassMethod(Class cls,SEL selector) {assert(cls==(Class)1 && selector==(SEL)2);return &method;}
Method class_getInstanceMethod(Class cls,SEL selector) {
 if(cls==(Class)1 && selector==(SEL)7)return &list_method;
 if(cls==(Class)1 && selector==(SEL)14)return &publishing_method;
 if(cls==(Class)1 && selector==(SEL)15)return &streams_method;
 if(cls==(Class)16 && selector==(SEL)12)return &backings_init_method;
 if(cls==(Class)16 && selector==(SEL)13)return &backings_list_method;
 if(cls==(Class)24 && selector==(SEL)17)return &sources_method;
 if(cls==(Class)24 && selector==(SEL)27)return &source_init_method;
 assert(cls==(Class)4 && selector==(SEL)3);return &resource_method;
}
IMP method_getImplementation(Method m) {return m->implementation;}
const char *method_getTypeEncoding(Method m)
{if(m==&source_init_method)return "@36@0:8@16B24B28B32";
 assert(m==&sources_method);return wrong_sources_encoding ? "@32@0:8i16^v20B28":"@56@0:8i16{?=[8I]}20B52";}
IMP method_setImplementation(Method m,IMP replacement) {IMP old=m->implementation;m->implementation=replacement;++replacements;return old;}
CFStringRef nezha_test_cfstr(const char *s) {return (CFStringRef)s;}
int model_test_access(const char *path,int flags) {assert(!strcmp(path,"/System/Library/MediaCapture/NezhaCameraModel.plist") && flags==R_OK);return model_readable ? 0:-1;}
/* Real objc_msgSend is an assembly dispatch entry, cast to each method's
 * typed ABI. The fixture's variadic C entry must not attach one function
 * sanitizer signature to those different, legitimate dispatch calls. */
__attribute__((no_sanitize("function"))) id objc_msgSend(id self,SEL selector,...)
{
 va_list args;va_start(args,selector);id result=NULL;
 if (selector==(SEL)4)result=self; /* bundle fixture is its identifier */
 else if (selector==(SEL)5)result=(id)(uintptr_t)(strcmp(self,va_arg(args,const char *))==0);
 else if (selector==(SEL)8) {assert(self==(id)11);result=(id)1;}
 else if (selector==(SEL)9) {assert(self==(id)11 && !strcmp(va_arg(args,const char *),"Default"));result=(id)1;}
 else if (selector==(SEL)10) {assert(self==(Class)17 && !strcmp(va_arg(args,const char *),"/System/Library/MediaCapture/NezhaCameraModel.plist"));result=(id)18;}
 else if (selector==(SEL)11) {assert(self==(id)18 && !strcmp(va_arg(args,const char *),"AVCaptureDevices"));result=(id)11;}
 else if (selector==(SEL)16) {assert(self==(id)22);result=(id)11;}
 else if (selector==(SEL)18) {assert(self==(Class)16);result=(id)21;}
 else if (selector==(SEL)13) {assert(self==(id)21);result=(id)11;}
 else if (selector==(SEL)19) {assert(self==(id)11 && va_arg(args,unsigned long)==0);result=(id)25;}
 else if (selector==(SEL)20) {assert(self==(id)25);result=(id)26;}
 else if (selector==(SEL)21) {assert(self==(id)25);result=(id)11;}
 else if (selector==(SEL)22) {assert(self==(id)25);result=NULL;}
 else if (selector==(SEL)23) {assert(self==(id)25);result=(id)0x76696465;}
 else if (selector==(SEL)24) {assert(self==(id)26);result=(id)2;}
 else if (selector==(SEL)25) {assert(self==(id)26);result=(id)1;}
 else if (selector==(SEL)26) {assert(self==(id)26);result=(id)"com.apple.avfoundation.avcapturedevice.built-in_video:0";}
 else {assert(self==(Class)5 && selector==(SEL)6);assert(!strcmp(va_arg(args,const char *),"/System/Library/MediaCapture/NezhaCameraModel.plist"));result=(id)9;}
 va_end(args);return result;
}
void _dyld_register_func_for_add_image(void (*callback)(const struct mach_header *,intptr_t)) {arrived=callback;}
CFTypeRef CFRetain(CFTypeRef value) {assert(value==(id)11);++backing_references;return value;}
void CFRelease(CFTypeRef value) {assert(value==(id)11 && backing_references);--backing_references;}
int nezha_camera_device_backend_bind(void) {return backend_status;}
NezhaFigStatus nezha_camera_device_create(NezhaFigAllocator a,NezhaFigString o,NezhaFigObject *d)
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
 assert(called((id)1,(SEL)2,NULL)==&retained_vendor && seen_factory==nezha_camera_device_create);
 backend_status=-1;
 assert(called((id)1,(SEL)2,NULL)==&retained_vendor && !seen_factory && original_calls==2);
 /* Wrong class/offset must prevent the diagnostic's ivar read. */
 wrong_vendor_class=true;assert(called((id)1,(SEL)2,NULL)==&retained_vendor);
 wrong_vendor_class=false;factory_ivar.offset=16;assert(called((id)1,(SEL)2,NULL)==&retained_vendor);
 factory_ivar.offset=8;
 arrived((const struct mach_header *)&image,slide);assert(replacements==1);
 bundle_available=true;capture_slide=(intptr_t)(uintptr_t)resource_original-(intptr_t)0x180b9cf90ULL;
 install_model_resource();assert(model_installed && replacements==2);
 id (*lookup)(id,SEL,id,id,id)=(void *)resource_method.implementation;
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"AVCaptureSession.plist",NULL,(id)"D16")== (id)9);
 /* camera038's actual service requests the same file with no subdirectory. */
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"AVCaptureSession.plist",NULL,NULL)==(id)9);
 assert(!model_original_calls);
 assert(lookup((id)"com.apple.CoreMedia",(SEL)3,(id)"AVCaptureSession.plist",NULL,(id)"D16")== (id)7);
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"CameraParameters.plist",NULL,(id)"D16")== (id)7);
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"AVCaptureSession.plist",(id)"plist",(id)"D16")== (id)7);
 model_readable=false;
 assert(lookup((id)"com.apple.CMCapture",(SEL)3,(id)"AVCaptureSession.plist",NULL,(id)"D16")== (id)7);
 assert(model_original_calls==4);
 capture_slide=(intptr_t)(uintptr_t)ids_original-(intptr_t)0x1b080b2b8ULL;
 install_device_ids();assert(device_ids_installed && replacements==3);
 id (*list)(id,SEL)=(void *)list_method.implementation;
 assert(list(&retained_vendor,(SEL)7)==(id)11 && list_calls==1);
 backings_available=true;
 capture_slide=(intptr_t)(uintptr_t)backings_init_original-(intptr_t)0x1b07a9d9cULL;
 install_backings();assert(backings_init_installed && replacements==4);
 capture_slide=(intptr_t)(uintptr_t)backings_list_original-(intptr_t)0x1b07a9e60ULL;
 install_backings();assert(backings_list_installed && replacements==5);
 id (*make)(id,SEL,id,id)=(void *)backings_init_method.implementation;
 assert(make((id)20,(SEL)12,(id)11,(id)18)==(id)21 && backing_inits==1);
 id (*get)(id,SEL)=(void *)backings_list_method.implementation;
 assert(get((id)21,(SEL)13)==(id)11 && backing_lists==2);
 capture_slide=(intptr_t)(uintptr_t)publishing_original-(intptr_t)0x1b080b458ULL;
 install_publishing();assert(publishing_installed && replacements==6);
 id (*publish)(id,SEL,id,int32_t *)=(void *)publishing_method.implementation;
 int32_t error=0;assert(publish(&retained_vendor,(SEL)14,(id)"Default",&error)==NULL && error==-12780 && publishing_calls==1);
 capture_slide=(intptr_t)(uintptr_t)streams_original-(intptr_t)0x1b0436bf8ULL;
 install_stream_request();assert(stream_request_installed && replacements==7);
 id (*request)(id,SEL,id,id,id,int32_t,bool,int32_t *)=(void *)streams_method.implementation;
 error=0;assert(request(&retained_vendor,(SEL)15,(id)22,(id)11,(id)11,3,false,&error)==NULL && error==-12783 && stream_request_calls==1);
 publish_success=true;original_publishing=(IMP)publishing_succeeded;
 assert(publish(&retained_vendor,(SEL)14,(id)"Default",&error)==(id)22 && error==0);
 sources_available=true;wrong_sources_encoding=true;
 source_init_method.implementation=(IMP)source_init_original;
 capture_slide=(intptr_t)(uintptr_t)sources_original-(intptr_t)0x1b0451d48ULL;
 install_sources();assert(!sources_installed && replacements==7);
 wrong_sources_encoding=false;install_sources();assert(sources_installed && replacements==8);
 id (*copy)(id,SEL,int32_t,CameraAuditToken,bool)=(void *)sources_method.implementation;
 for(unsigned i=0;i<8;++i)expected_audit.val[i]=0x12340000+i;
 source_manager.backings=(id)11;sources_result=NULL;
 assert(copy(&source_manager,(SEL)17,1,expected_audit,true)==NULL && sources_calls==1);
 source_manager.shared=1;sources_result=(id)11;
 assert(copy(&source_manager,(SEL)17,1,expected_audit,true)==(id)11 && sources_calls==2);
 capture_slide=(intptr_t)(uintptr_t)source_init_original-(intptr_t)0x1b06e2928ULL;
 install_sources();assert(source_init_installed && replacements==9);
 id (*init_sources)(id,SEL,id,bool,bool,bool)=(void *)source_init_method.implementation;
 expected_init_backings=(id)11;expected_init_shared=false;
 assert(main_backings==(id)11 && backing_references==1);
 assert(init_sources(&source_manager,(SEL)27,NULL,true,true,true)==&source_manager && source_initializations==1 && backing_references==1);
 expected_init_backings=NULL;
 assert(init_sources(&source_manager,(SEL)27,NULL,true,true,false)==&source_manager && source_initializations==2);
 CFRelease(main_backings);main_backings=NULL;expected_init_shared=true;
 assert(init_sources(&source_manager,(SEL)27,NULL,true,true,true)==&source_manager && source_initializations==3 && !backing_references);
 puts("Camera independent main backing: native source initializer receives measured main list with shared=false, retains lifecycle; unchanged local and unavailable-main fallback passed");
 puts("Camera original sources: by-value 32-byte audit token, native empty and populated returns, local/shared backings and exact method encoding passed");
 puts("Camera system vendor: exact process/UUID/method, delayed class arrival, factory replacement and original fallback passed");
 puts("Camera model resource: exact camera bundle/file only, original unrelated resource and unavailable-model fallback passed");
}
