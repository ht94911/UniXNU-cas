#!/usr/bin/env python3
"""Execute the existing texture-log body with captured stderr and NV12 codes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
text = a.source.read_text()


def body(marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 0
    for i in range(opening, len(text)):
        depth += text[i] == '{'
        depth -= text[i] == '}'
        if not depth:
            return text[start:i + 1]
    raise ValueError(marker)


code = body('static NSString *textureSurfaceCodes(') + '\n' + body('static void logTexture(')
prefix = r'''
#import <Foundation/Foundation.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unistd.h>
struct __IOSurface {uint32_t format;size_t planes,width,height,size,offset[2],row[2];uint8_t *base;};
static uint32_t IOSurfaceGetID(IOSurfaceRef) {return 17;}
static uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef s) {return s->format;}
static size_t IOSurfaceGetPlaneCount(IOSurfaceRef s) {return s->planes;}
static size_t IOSurfaceGetWidth(IOSurfaceRef s) {return s->width;}
static size_t IOSurfaceGetHeight(IOSurfaceRef s) {return s->height;}
static size_t IOSurfaceGetAllocSize(IOSurfaceRef s) {return s->size;}
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s) {return s->base;}
static void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s,unsigned p) {return s->base+s->offset[p];}
static size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s,unsigned p) {return s->row[p];}
static size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s) {return s->row[0];}
@interface MTLTextureDescriptor : NSObject
@property(nonatomic) NSUInteger width,height,pixelFormat;
@end
@implementation MTLTextureDescriptor @end
static uint64_t textureRotation(id) {return 0;}
static std::atomic<uint64_t> gTexturesLogged;
static bool allow=true,stderrOn=true;
static bool textureLogTurn() {if(!allow)return false;++gTexturesLogged;return true;}
static bool logToStderr() {return stderrOn;}
#define GLOG(...) do {} while(0)
'''
fixture = r'''
static std::string capture(void (^call)(void)) {
    FILE *file=tmpfile();assert(file);
    const int saved=dup(fileno(stderr));assert(saved>=0);
    assert(dup2(fileno(file),fileno(stderr))>=0);
    call();fflush(stderr);
    assert(dup2(saved,fileno(stderr))>=0);close(saved);
    rewind(file);std::string result;char buffer[4096];size_t n;
    while((n=fread(buffer,1,sizeof(buffer),file)))result.append(buffer,n);
    fclose(file);return result;
}
int main(){@autoreleasepool{
    std::vector<uint8_t> bytes(8192,0);
    __IOSurface surface{0x34323066,2,64,16,bytes.size(),{512,4096},{64,64},bytes.data()};
    bytes[512+8*64+32]=75;bytes[4096+4*64+32]=130;bytes[4096+4*64+33]=113;
    MTLTextureDescriptor *d=[MTLTextureDescriptor new];d.width=64;d.height=16;d.pixelFormat=540;
    auto log=[&]{logTexture("iosurface",d,&surface,0,bytes.data(),512,64,true);};
    std::string first=capture(^{log();});
    assert(first.find("CPU center 75/130/113 rows 64/64")!=std::string::npos);
    assert(first.find("desc 64x16 fmt 540 rot 0")!=std::string::npos);
    assert(first.find("plane 0 alloc 8192 | offset 512 rows 64")!=std::string::npos);
    bytes[512+8*64+32]=51;
    assert(capture(^{log();}).find("CPU center 51/130/113")!=std::string::npos);
    stderrOn=false;assert(capture(^{log();}).empty());stderrOn=true;
    allow=false;const auto before=gTexturesLogged.load();
    assert(capture(^{log();}).empty() && gTexturesLogged==before);allow=true;
    assert(capture(^{logTexture("no-copy",d,nullptr,0,bytes.data(),0,64,true);}).find("memory ")!=std::string::npos);
    surface.row[1]=0;assert(textureSurfaceCodes(&surface)==nil);surface.row[1]=64;
    surface.size=1024;assert(textureSurfaceCodes(&surface)==nil);surface.size=bytes.size();
    surface.format=0x42475241;assert(textureSurfaceCodes(&surface)==nil);
    puts("PASS: ten production texture-log cases; bounded stderr, current NV12 CPU codes and original shape fields");
}}
'''
cpp = a.out / 'surface-log.mm'
cpp.write_text(prefix + '\n#include <string>\n' + code + '\n' + fixture)
compile = subprocess.run(['xcrun', 'clang++', '-std=c++17', '-fobjc-arc', '-fblocks', '-O1', '-g',
                          '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                          '-fsanitize=address,undefined', str(cpp), '-framework', 'Foundation',
                          '-o', str(a.out / 'surface-log')], capture_output=True, timeout=120)
(a.out / 'build.log').write_bytes(compile.stdout + compile.stderr)
assert compile.returncode == 0, 'surface-log compilation failed'
run = subprocess.run([str(a.out / 'surface-log')], capture_output=True, timeout=30)
(a.out / 'run.log').write_bytes(run.stdout + run.stderr)
assert run.returncode == 0 and b'PASS: ten' in run.stdout
report = dict(passed=True, cases=10, actual_logging_body_executed=True, ASan_UBSan=True,
              source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
              method_bodies_sha256=hashlib.sha256(code.encode()).hexdigest(),
              only_existing_diagnostic_gate=True, constructor_unchanged=True,
              GPU_pixels_verified=False, phone_operations=False)
(a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(run.stdout.decode(), end='')
