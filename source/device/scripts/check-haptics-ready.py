#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the real marker through class matching and service retirement.

The original consumer matches IOProviderClass, not the entry's displayed name.
The fixture checks the actual producer's class identity, readiness transitions,
uptime units and retained ownership through termination errors.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parents[1]/'package/haptics27/NezhaHapticsReady.cpp'
    body = '\n'.join(line for line in source.read_text().splitlines()
                     if not line.startswith('#include'))
    fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <map>
#include <string>
#include <vector>
#define NEZHA_HAPTICS_READY_HOST 1
static constexpr unsigned kIOServiceRequired=1, kIOServiceSynchronous=2;
struct OSSymbol {
 std::string name;
 static const OSSymbol *withCString(const char *n){return new OSSymbol{n};}
 void release()const{delete this;}
};
struct OSMetaClass {
 std::string name;const OSMetaClass *parent;size_t bytes;
 static std::map<std::string,const OSMetaClass *> classes;
 const OSMetaClass *getSuperClass()const{return parent;}
 size_t getClassSize()const{return bytes;}
 static const OSMetaClass *getMetaClassWithName(const OSSymbol *s){
  auto it=classes.find(s->name);return it==classes.end()?nullptr:it->second;
 }
 static void *preModLoad(const char *){return (void *)1;}
 static int postModLoad(void *){return 0;}
};
std::map<std::string,const OSMetaClass *> OSMetaClass::classes;
static bool initFails,attachFails,startFails,terminateFails;
static unsigned releases,starts,registrations,terminated,constructorCalls;
static uint64_t absoluteTime=310000000;
struct IOService {
 static OSMetaClass gMetaClass;
 static std::vector<IOService *> registry;
 virtual ~IOService()=default;
 virtual const OSMetaClass *getMetaClass()const{return &gMetaClass;}
 bool active=false,attached=false;
 std::string name;
 std::map<std::string,uint64_t> values;
 bool init(){return !initFails;}
 bool attach(IOService *){attached=!attachFails;return attached;}
 void detach(IOService *){attached=false;}
 bool start(IOService *){++starts;return !startFails;}
 bool setProperty(const char *key,uint64_t value,unsigned bits){
  assert(bits==64);values[key]=value;return true;
 }
 void setName(const char *n){name=n;}
 void registerService(unsigned options){
  assert(options==kIOServiceSynchronous && attached);active=true;
  registry.push_back(this);++registrations;
 }
 bool terminate(unsigned options){
  assert(options==(kIOServiceRequired|kIOServiceSynchronous));++terminated;
  if(terminateFails)return false;
  active=false;return true;
 }
 void release(){++releases;delete this;}
 static IOService *getResourceService(){static IOService root;return &root;}
};
OSMetaClass IOService::gMetaClass={"IOService",nullptr,sizeof(IOService)};
std::vector<IOService *> IOService::registry;
static IOService *originalFirstMatch(const char *className){
 for(auto *service:IOService::registry){
  if(service->active && service->getMetaClass()->name==className)return service;
 }
 return nullptr;
}
static void IOLog(const char *,...){}
static uint64_t mach_absolute_time(){return absoluteTime;}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *ns){*ns=value*10;}
struct {const char *name;} g_kernel_kmod_info={"kernel"};
#define OSDeclareDefaultStructors(cls) \
 public:static OSMetaClass gMetaClass; \
 const OSMetaClass *getMetaClass()const override{return &gMetaClass;}
#define OSDefineMetaClassAndStructors(cls,base) \
 OSMetaClass cls::gMetaClass={#cls,&base::gMetaClass,sizeof(cls)};
'''
    cases = r'''
extern "C" void nezha_haptics_ready_static_init(){
 ++constructorCalls;
 OSMetaClass::classes["AHSFirmwareReadyNotifier"]=&AHSFirmwareReadyNotifier::gMetaClass;
}
int main(){
 assert(nezha_haptics_ready_update(0)==0 && !constructorCalls && !registrations);
 // A displayed IOService name alone does not satisfy the original consumer.
 IOService named;named.active=true;named.name="AHSFirmwareReadyNotifier";
 IOService::registry.push_back(&named);
 assert(!originalFirstMatch("AHSFirmwareReadyNotifier"));
 IOService::registry.clear();
 initFails=true;
 assert(nezha_haptics_ready_update(1)==ENXIO && !registrations && !published);
 initFails=false;attachFails=true;
 assert(nezha_haptics_ready_update(1)==ENXIO && !registrations && !published);
 attachFails=false;startFails=true;
 assert(nezha_haptics_ready_update(1)==EIO && !registrations && !published);
 startFails=false;
 assert(nezha_haptics_ready_update(1)==0 && constructorCalls==1);
 auto *first=originalFirstMatch("AHSFirmwareReadyNotifier");assert(first);
 assert(first->values["FirmwareReadyTimestamp"]==3100);
 assert(first->getMetaClass()->getSuperClass()==&IOService::gMetaClass);
 assert(nezha_haptics_ready_state()==1 && !nezha_haptics_ready_error());
 assert(nezha_haptics_ready_update(1)==0 && registrations==1);
 terminateFails=true;unsigned before=releases;
 assert(nezha_haptics_ready_update(0)==EIO && releases==before && published==first);
 assert(nezha_haptics_ready_state()==2 && nezha_haptics_ready_error()==EIO);
 assert(nezha_haptics_ready_update(1)==EIO && registrations==1);
 terminateFails=false;
 assert(nezha_haptics_ready_update(0)==0 && releases==before+1 && !published);
 IOService::registry.clear();
 assert(nezha_haptics_ready_state()==0 && !nezha_haptics_ready_error());
 absoluteTime+=1000000;
 assert(nezha_haptics_ready_update(1)==0 && constructorCalls==1 && registrations==2);
 assert(originalFirstMatch("AHSFirmwareReadyNotifier"));
 assert(nezha_haptics_ready_update(0)==0 && terminated==3);
 IOService::registry.clear();
 puts("PASS production ready service: class-based first match, no false-ready on init/attach/start failure, millisecond timestamp, single constructor, stop/wake and retained failed-termination ownership");
}
'''
    program = args.out/'ready.cpp'
    program.write_text(fixture+'\n'+body+'\n'+cases)
    binary = args.out/'ready'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g',
        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
        '-fno-sanitize-recover=all', str(program), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    (args.out/'execution.log').write_text(result.stdout+result.stderr)
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
