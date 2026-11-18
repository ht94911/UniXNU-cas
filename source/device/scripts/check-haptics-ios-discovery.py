#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the real product publisher through the original default-name consumer.

MobileGestalt's fixed24A446 callback enumerates IODeviceTree and calls
IORegistryEntryGetName. XNU IORegistryEntry returns the metaclass name when
only the plane name is set. The fixture preserves that distinct behavior.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=False)
    source=Path(__file__).resolve().parents[1]/'package/haptics27/NezhaHapticsIOS.cpp'
    body='\n'.join(line for line in source.read_text().splitlines() if not line.startswith('#include'))
    fixture=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <map>
#include <vector>
static void *gIODTPlane=(void *)1;
struct OSData {
 std::vector<uint8_t> bytes;
 static OSData *withBytes(const void *p,size_t n) {
  auto *v=new OSData;v->bytes.assign((const uint8_t *)p,(const uint8_t *)p+n);return v;
 }
 void release(){delete this;}
};
struct OSDictionary {
 std::map<std::string,std::vector<uint8_t>> values;
 static OSDictionary *withCapacity(unsigned){return new OSDictionary;}
 bool setObject(const char *k,OSData *v){values[k]=v->bytes;return true;}
 void release(){delete this;}
};
struct IORegistryEntry {
 std::string defaultName,planeName;
 std::map<std::string,std::vector<uint8_t>> values;
 IORegistryEntry *child=nullptr;
 static IORegistryEntry *product;
 bool init(OSDictionary *v){values=v->values;return true;}
 static IORegistryEntry *fromPath(const char *p,void *plane){
  return !strcmp(p,"/product") && plane==gIODTPlane?product:nullptr;
 }
 IORegistryEntry *childFromPath(const char *p,void *plane){
  return child && !strcmp(child->getName(plane),p)?child:nullptr;
 }
 // Public XNU IORegistryEntry::getName: plane name, default name, metaclass.
 const char *getName(void *plane=nullptr)const{
  if(plane && !planeName.empty())return planeName.c_str();
  return defaultName.empty()?"IORegistryEntry":defaultName.c_str();
 }
 void setName(const char *name,void *plane=nullptr){(plane?planeName:defaultName)=name;}
 bool attachToParent(IORegistryEntry *p,void *plane){assert(plane==gIODTPlane);p->child=this;return true;}
 void release(){} // Registry ownership is released explicitly by this fixture.
};
IORegistryEntry *IORegistryEntry::product;
static uint32_t selectedStage;
static int runtimeReady;
static unsigned assetsReadyCalls;
static bool PE_parse_boot_argn(const char *key,void *p,size_t n){
 assert(!strcmp(key,"nezha-haptics-ios") && n==4);memcpy(p,&selectedStage,4);return true;
}
static void IOLog(const char *,...){}
extern "C" int nezha_haptics_runtime_ready(){return runtimeReady;}
extern "C" int nezha_haptics_ios_assets_ready(){++assetsReadyCalls;return 0;}
// Actual original consumer boundary: iterator over thedevice-tree children,
// then IORegistryEntryGetName(default), comparing the exact string haptics.
static bool originalSupportsHaptics(const IORegistryEntry &product){
 return product.child && !strcmp(product.child->getName(),"haptics");
}
'''
    cases=r'''
int main(){
 IORegistryEntry product;IORegistryEntry::product=&product;
 selectedStage=2;runtimeReady=0;
 assert(nezha_haptics_ios_commit(1,1)==ENXIO && !product.child);
 runtimeReady=1;
 assert(nezha_haptics_ios_commit(0,1)==ENXIO && !product.child);
 assert(nezha_haptics_ios_commit(1,0)==ENXIO && !product.child);
 assert(nezha_haptics_ios_commit(1,1)==0);
 assert(assetsReadyCalls==1);
 assert(originalSupportsHaptics(product));
 assert(!strcmp(product.child->getName(gIODTPlane),"haptics"));
 uint32_t value=0;auto &third=product.child->values["supports-3rd-party-haptics"];
 assert(third.size()==4);memcpy(&value,third.data(),4);assert(value==1);
 auto &closed=product.child->values["closed-loop"];
 assert(closed.size()==4);memcpy(&value,closed.data(),4);assert(value==1);
 auto &name=product.child->values["name"];
 assert(name.size()==8 && !memcmp(name.data(),"haptics",8));
 // Reproduce013: lookup through theplane works, but GetName(default) fails.
 product.child->defaultName.clear();
 assert(product.childFromPath("haptics",gIODTPlane)==product.child);
 assert(!originalSupportsHaptics(product));
 product.child->setName("haptics");assert(originalSupportsHaptics(product));
 assert(nezha_haptics_ios_commit(1,1)==0);
 assert(assetsReadyCalls==2);
 delete product.child;
 puts("PASS original default-name discovery: old013 plane-only publication rejected; current production publisher accepted; readiness/preset gates and raw properties preserved");
}
'''
    program=args.out/'discovery.cpp'
    program.write_text(fixture+'\n'+body+'\n'+cases)
    binary=args.out/'discovery'
    command=[os.environ.get('CXX','clang++'),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(program),'-o',str(binary)]
    subprocess.run(command,check=True)
    result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
    (args.out/'execution.log').write_text(result.stdout+result.stderr)
    print(result.stdout,end='')


if __name__=='__main__':main()
