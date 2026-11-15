#ifndef CAMERA_VENDOR_TEST_OBJC
#define CAMERA_VENDOR_TEST_OBJC
typedef void *id,*Class,*SEL;
struct TestMethod;
typedef struct TestMethod *Method;
typedef struct TestIvar *Ivar;
typedef id (*IMP)(id,SEL,...);
Class objc_getClass(const char *);
SEL sel_registerName(const char *);
Method class_getClassMethod(Class,SEL);
Method class_getInstanceMethod(Class,SEL);
IMP method_getImplementation(Method);
IMP method_setImplementation(Method,IMP);
Ivar class_getInstanceVariable(Class,const char *);
Class object_getClass(id);
__PTRDIFF_TYPE__ ivar_getOffset(Ivar);
#endif
