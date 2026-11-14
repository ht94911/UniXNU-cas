/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <objc/runtime.h>
static const char *process_name="test";
const char *getprogname(void) {return process_name;}
#include "../package/camera/userland/NezhaCameraSystemVendor.m"

static bool class_available;
static unsigned replacements,original_calls;
static int backend_status;
static CameraCreate seen_factory;
static void (*arrived)(const struct mach_header *,intptr_t);
struct TestMethod {IMP implementation;};
static id vendor_original(id self,SEL selector,CameraCreate factory)
{ assert(self==(id)1 && selector==(SEL)2);++original_calls;seen_factory=factory;return (id)3; }
static struct TestMethod method={(IMP)vendor_original};
Class objc_getClass(const char *name) {assert(!strcmp(name,"BWFigCaptureDeviceVendor"));return class_available ? (Class)1:NULL;}
SEL sel_registerName(const char *name) {assert(!strcmp(name,selector_name));return (SEL)2;}
Method class_getClassMethod(Class cls,SEL selector) {assert(cls==(Class)1 && selector==(SEL)2);return &method;}
IMP method_getImplementation(Method m) {return m->implementation;}
IMP method_setImplementation(Method m,IMP replacement) {IMP old=m->implementation;m->implementation=replacement;++replacements;return old;}
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
 puts("Camera system vendor: exact process/UUID/method, delayed class arrival, factory replacement and original fallback passed");
}
