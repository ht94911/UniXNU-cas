// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotPermitted=-1;
#include "RootExtents.h"  // the selected ShengUfs source, not a second translator
static struct {bool ready=true,poisoned=false;} gHost;
static bool gHostPanic;
static struct {bool valid=true;uint64_t first=100,last=999;} gStoreGeometry;
static constexpr unsigned kDataMax=65536;
static UfsExtentMap gExtentMap={},gDataExtentMap={},gProtectedExtentMap={};
static int enabled=1,loads,reads,sha_size=32;
extern "C" int nezha_baseband_enabled(){return enabled;}
struct OSData {unsigned getLength(){return sha_size;}const void *getBytesNoCopy(){static uint8_t sha[32]={7};return sha;}};
static OSData sha;
struct IORegistryEntry {static IORegistryEntry *fromPath(const char *){static IORegistryEntry e;return &e;}void *getProperty(const char *){return &sha;}void release(){}};
#define OSDynamicCast(T,p) static_cast<T *>(p)
static uint64_t words[]={0,50,1,1,80,2};
static int loadExtentProperties(UfsExtentMap &m,const char *table,const char *count,const char **why){
    assert(!strcmp(table,"nezha-baseband-extents")&&!strcmp(count,"nezha-baseband-blocks"));loads++;
    static UfsExtent e[4];static uint16_t order[4];m.ext=e;m.byPart=order;
    *why=ufsExtentLoad(m,words,6,3);return *why?-1:1;
}
static IOReturn hostRead4kRaw(uint8_t lun,uint64_t l,uint32_t n,uint8_t *p){
    assert(lun==0 && ((l==150&&n==1)||(l>=180&&l+n<=182)));reads++;memset(p,(int)l,n*4096);return 0;
}
#include "../../package/baseband27/integration/NbUfsReadOnly.inc"
int main(){
    UfsExtent protected_e[3]={{0,0,10},{10,20,10},{20,30,10}},root_e[1]={{0,0,10}},data_e[1]={{0,10,10}};
    uint16_t po[3]={0,1,2},ro[1]={0},da[1]={0};
    gProtectedExtentMap={protected_e,po,3,30,100,900,true,true};gExtentMap={root_e,ro,1,10,100,900,true,true};gDataExtentMap={data_e,da,1,10,100,900,true,true};
    uint64_t blocks=0;uint8_t digest[32],buffer[12288];enabled=0;
    assert(native_ufs_bb_open(&blocks,digest)<0&&loads==0&&reads==0);enabled=1;
    gHost.poisoned=true;assert(native_ufs_bb_open(&blocks,digest)<0&&loads==0);gHost.poisoned=false;
    assert(!native_ufs_bb_open(&blocks,digest)&&blocks==3&&digest[0]==7&&loads==1&&reads==0);
    assert(!native_ufs_bb_read(0,3,buffer)&&reads==2&&buffer[0]==150&&buffer[4096]==180);
    assert(native_ufs_bb_read(2,2,buffer)<0&&reads==2);assert(native_ufs_bb_read(UINT64_MAX,1,buffer)<0);
    gHostPanic=true;assert(native_ufs_bb_read(0,1,buffer)<0);gHostPanic=false;
    nbUfsOpened=0;words[1]=10;assert(native_ufs_bb_open(&blocks,digest)<0); // Data overlap
    nbUfsOpened=0;words[1]=20;assert(native_ufs_bb_open(&blocks,digest)<0); // other protected file
    nbUfsOpened=0;words[1]=50;sha_size=31;assert(native_ufs_bb_open(&blocks,digest)<0);
    puts("PASS selected ShengUfs reader: switch, map/GPT/protected bounds, fragmented reads, no write API");
}
