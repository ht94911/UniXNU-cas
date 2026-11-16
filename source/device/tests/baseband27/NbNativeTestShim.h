// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <openssl/evp.h>
using IOOptionBits=uint64_t;using IOByteCount=uint64_t;using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOMapAnywhere=1,kIOMapWriteCombineCache=2,kIOMapInhibitCache=4;
constexpr int kIOMemoryPhysicallyContiguous=8,kIODirectionInOut=16,kIOMemoryMapperNone=32,TRUE=1;
extern void *kernel_task;
using DTEntry=void *;
constexpr int kSuccess=0;
int SecureDTLookupEntry(void *,const char *,DTEntry *);
int SecureDTGetProperty(DTEntry,const char *,const void **,unsigned *);
void *mock_alloc(size_t,size_t=16);void mock_free(void *);void *mock_map(uint64_t,uint32_t);
void mock_mmio_write32(volatile uint8_t *,size_t,uint32_t);
void *IOMalloc(size_t);void IOSleep(unsigned);void flush_dcache64(uint64_t,unsigned,int);
uint64_t mach_absolute_time();void absolutetime_to_nanoseconds(uint64_t,uint64_t *);
class IOMemoryMap {
    void *p;
public:
    explicit IOMemoryMap(void *v):p(v){}uintptr_t getVirtualAddress(){return (uintptr_t)p;}
    static void *operator new(size_t n){return mock_alloc(n);}void operator delete(void *p){mock_free(p);}
    void release(){delete this;}
};
class IODeviceMemory {
    uint64_t pa;uint32_t bytes;
public:
    IODeviceMemory(uint64_t a,uint32_t n):pa(a),bytes(n){}
    static IODeviceMemory *withRange(uint64_t a,uint32_t n){return new IODeviceMemory(a,n);}
    static void *operator new(size_t n){return mock_alloc(n);}void operator delete(void *p){mock_free(p);}
    IOMemoryMap *map(IOOptionBits){return new IOMemoryMap(mock_map(pa,bytes));}void release(){delete this;}
};
class IOBufferMemoryDescriptor {
    uint8_t *p;uint64_t pa;uint32_t bytes;
public:
    IOBufferMemoryDescriptor(uint32_t,uint32_t,bool);
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *,IOOptionBits,uint32_t n,uint32_t a){return new IOBufferMemoryDescriptor(n,a,false);}
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(void *,IOOptionBits,uint32_t n,uint64_t mask){return new IOBufferMemoryDescriptor(n,(uint32_t)((~mask&0xffffffffULL)+1),true);}
    static void *operator new(size_t n){return mock_alloc(n);}void operator delete(void *p){mock_free(p);}
    uint64_t getPhysicalSegment(uint64_t,IOByteCount *n,unsigned){if(n)*n=bytes;return pa;}
    void *getBytesNoCopy(){return p;}IOMemoryMap *map(IOOptionBits){return new IOMemoryMap(p);}
    void release(){mock_free(p);delete this;}
};
class IORegistryEntry {
public:
    static IORegistryEntry *fromPath(const char *){static IORegistryEntry e;return &e;}
    void *getProperty(const char *name){return !strcmp(name,"IOBSD")?this:nullptr;}void release(){}
};
struct kmod_info_t{const char *name;};
class OSMetaClass{public:static void *preModLoad(const char *){return (void *)1;}static int postModLoad(void *){return 0;}};
struct ccdigest_info{};
inline const ccdigest_info *ccsha256_di(){static ccdigest_info di;return &di;}
#define ccdigest_di_decl(di,n) EVP_MD_CTX *n[1]={nullptr}
inline void ccdigest_init(const ccdigest_info *,EVP_MD_CTX **c){*c=EVP_MD_CTX_new();assert(*c&&EVP_DigestInit_ex(*c,EVP_sha256(),nullptr));}
inline void ccdigest_update(const ccdigest_info *,EVP_MD_CTX **c,size_t n,const void *p){assert(EVP_DigestUpdate(*c,p,n));}
inline void ccdigest_final(const ccdigest_info *,EVP_MD_CTX **c,uint8_t *p){unsigned n=32;assert(EVP_DigestFinal_ex(*c,p,&n));}
inline void ccdigest_di_clear(const ccdigest_info *,EVP_MD_CTX **c){EVP_MD_CTX_free(*c);*c=nullptr;}
