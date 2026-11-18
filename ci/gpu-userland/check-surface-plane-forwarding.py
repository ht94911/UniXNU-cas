#!/usr/bin/env python3
"""Exercise production surface construction, views and plane getters over captured imports.

The full surface_plane_host.mm consumer separately verifies actual IOSurface and
Vulkan bytes. This check isolates the Objective-C identity contract and rejects
the old constant-zero getter with the same constructed plane-one texture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess


def method(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 0
    for i in range(opening, len(text)):
        depth += text[i] == '{'
        depth -= text[i] == '}'
        if depth == 0:
            return text[start:i + 1] + '\n'
    raise ValueError(marker)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--before', type=Path)
args = parser.parse_args()
source = args.source.resolve()
text = source.read_text()
texture_methods = text[text.index('@implementation NBTexture\n'):]
getter = method(texture_methods, '- (NSUInteger)iosurfacePlane {')
surface_getter = method(texture_methods, '- (IOSurfaceRef)iosurface {')
view = method(texture_methods, '- (id<MTLTexture>)view:(NmbTexture *)handle {')
constructor = method(text, '- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:')
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)

prefix = r'''
#include <objc/Object.h>
#include <objc/runtime.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unistd.h>
typedef unsigned long NSUInteger;
struct Surface {size_t planes; void *base; size_t bytes; size_t offset[2], row[2];};
typedef Surface *IOSurfaceRef;
struct NmbTextureDescriptor {uint64_t width,height;};
struct NmbTexture {NmbTextureDescriptor descriptor;};
@protocol MTLTexture @end
@interface NBObject : Object
+ (id)new;
@end
static std::vector<id> objects;
@implementation NBObject
+ (id)new {id o=class_createInstance(self,0);objects.push_back(o);return o;}
@end
@interface MTLTextureDescriptor : NBObject @end
@implementation MTLTextureDescriptor @end
@interface NBBuffer : NBObject @end
@implementation NBBuffer @end
@interface NBTexture : NBObject <MTLTexture> {
 NmbTexture *_handle;NmbTextureDescriptor _descriptor;NBBuffer *_buffer;
 IOSurfaceRef _surface;NSUInteger _surfacePlane,_offset,_rowBytes;id<MTLTexture> _parent;
}
@property(nonatomic) NmbTexture *handle;
@property(nonatomic) NmbTextureDescriptor descriptor;
@property(nonatomic,assign) NBBuffer *buffer;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) NSUInteger surfacePlane,offset,rowBytes;
@property(nonatomic,assign) id<MTLTexture> parent;
- (IOSurfaceRef)iosurface;
- (NSUInteger)iosurfacePlane;
- (void)countUse;
- (id<MTLTexture>)view:(NmbTexture *)handle;
@end
@interface NBDeviceMethods : NBObject
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane;
@end
struct DetailSpan {template<class... T> DetailSpan(T...) {}};
#define NBLOG(...) do {} while(0)
static uint64_t gSurfaceRotations[5],gSurfacePlanar,gSurfaceTextures;
static bool gLogTextures=false,importRefused=false,textureRefused=false;
static unsigned imports,creates,retains,uses,refusals;
static uint64_t askedPlane,askedOffset,askedRow;
static bool askedPlanar;
static NmbTexture nativeTexture{{64,16}};
static NBBuffer *nativeBuffer;
static size_t IOSurfaceGetPlaneCount(IOSurfaceRef s) {return s->planes;}
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s) {return s->base;}
static size_t IOSurfaceGetAllocSize(IOSurfaceRef s) {return s->bytes;}
static void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s,size_t p) {return (char *)s->base+s->offset[p];}
static size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s,size_t p) {return s->row[p];}
static size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s) {return s->row[0];}
static uint64_t textureRotation(MTLTextureDescriptor *) {return 0;}
static void refuseSurface(unsigned,IOSurfaceRef,MTLTextureDescriptor *,NSUInteger,uint64_t,uint64_t) {++refusals;}
static bool b3a8Planes(IOSurfaceRef,size_t,size_t *) {return false;}
static NBBuffer *surfaceBuffer(IOSurfaceRef,void *,size_t) {++imports;return importRefused?nil:nativeBuffer;}
static NBTexture *made(NSUInteger p,NSUInteger offset,NSUInteger row,bool planar) {
 ++creates;askedPlane=p;askedOffset=offset;askedRow=row;askedPlanar=planar;
 if(textureRefused)return nil;
 NBTexture *t=[NBTexture new];t.handle=&nativeTexture;t.offset=offset;t.rowBytes=row;t.buffer=nativeBuffer;return t;
}
static NBTexture *textureOnSurface(NBBuffer *,MTLTextureDescriptor *,IOSurfaceRef,NSUInteger p,NSUInteger o,NSUInteger row) {return made(p,o,row,true);}
static NBTexture *textureOnBuffer(NBBuffer *,MTLTextureDescriptor *,NSUInteger o,NSUInteger row,bool) {return made(0,o,row,false);}
static IOSurfaceRef CFRetain(IOSurfaceRef s) {++retains;return s;}
static void nmb_texture_describe(NmbTexture *t,NmbTextureDescriptor *d) {*d=t->descriptor;}
static void logTexture(const char *,MTLTextureDescriptor *,IOSurfaceRef,NSUInteger,void *,NSUInteger,NSUInteger,bool) {}
'''
body = prefix + '''
@implementation NBTexture
@synthesize handle=_handle,descriptor=_descriptor,buffer=_buffer,surface=_surface;
@synthesize surfacePlane=_surfacePlane,offset=_offset,rowBytes=_rowBytes,parent=_parent;
- (void)countUse {++uses;}
''' + surface_getter + getter + view + '@end\n@implementation NBDeviceMethods\n' + constructor + '@end\n'
fixture = r'''
int main() {
 void *pages=nullptr;assert(!posix_memalign(&pages,(size_t)getpagesize(),8192));
 Surface planar{2,pages,8192,{0,4096},{64,64}};
 Surface plain{0,pages,8192,{0,0},{64,0}};
 NBDeviceMethods *device=[NBDeviceMethods new];
 MTLTextureDescriptor *descriptor=[MTLTextureDescriptor new];
 nativeBuffer=[NBBuffer new];unsigned checks=0;
 for(NSUInteger p:{0ul,1ul}) {
  unsigned beforeImports=imports,beforeRetains=retains,beforeUses=uses;
  NBTexture *texture=(NBTexture *)[device newTextureWithDescriptor:descriptor iosurface:&planar plane:p];
  assert(texture && [texture iosurface]==&planar && [texture iosurfacePlane]==p);
  assert(askedPlanar && askedPlane==p && askedOffset==planar.offset[p] && askedRow==64);
  assert(imports==beforeImports+1 && retains==beforeRetains+1 && uses==beforeUses+1);++checks;
  NBTexture *view=(NBTexture *)[texture view:&nativeTexture];
  NBTexture *nested=(NBTexture *)[view view:&nativeTexture];
  assert([view iosurface]==&planar && [nested iosurface]==&planar);
  assert([view iosurfacePlane]==p && [nested iosurfacePlane]==p);
  assert(view.parent==(id<MTLTexture>)texture && nested.parent==(id<MTLTexture>)view);
  assert(nested.offset==planar.offset[p] && nested.rowBytes==64 && nested.buffer==nativeBuffer);
  assert(retains==beforeRetains+3 && uses==beforeUses+3);checks+=2;
  unsigned before=retains;assert(![texture view:nullptr] && retains==before);++checks;
 }
 NBTexture *texture=(NBTexture *)[device newTextureWithDescriptor:descriptor iosurface:&plain plane:0];
 assert(texture && [texture iosurfacePlane]==0 && [texture iosurface]==&plain && !askedPlanar);++checks;
 NBTexture *plainTexture=[NBTexture new];assert(![plainTexture iosurface] && [plainTexture iosurfacePlane]==0);++checks;
 unsigned beforeImports=imports,beforeCreates=creates,beforeRetains=retains;
 assert(![device newTextureWithDescriptor:descriptor iosurface:&planar plane:2]);
 assert(![device newTextureWithDescriptor:descriptor iosurface:&plain plane:1]);
 assert(![device newTextureWithDescriptor:descriptor iosurface:nullptr plane:0]);
 assert(imports==beforeImports && creates==beforeCreates && retains==beforeRetains);checks+=3;
 importRefused=true;assert(![device newTextureWithDescriptor:descriptor iosurface:&planar plane:1]);
 assert(creates==beforeCreates && retains==beforeRetains);++checks;importRefused=false;
 textureRefused=true;assert(![device newTextureWithDescriptor:descriptor iosurface:&planar plane:1]);
 assert(retains==beforeRetains);++checks;
 for(id object:objects)object_dispose(object);
 free(pages);
 std::printf("PASS surface plane forwarding: %u production constructor/getter/view checks\n",checks);
}
'''
if platform.system() == 'Darwin':
    body = body.replace('#include <objc/Object.h>', '#import <Foundation/Foundation.h>')
    body = body.replace('@interface NBObject : Object', '@interface NBObject : NSObject')
    flags = ['xcrun', 'clang++', '-framework', 'Foundation']
else:
    include = subprocess.check_output(['gcc', '-print-file-name=include'], text=True).strip()
    flags = ['clang++', '-I', include, '-lobjc']


def run(name, code):
    harness = out / (name + '.mm')
    harness.write_text(code + fixture)
    built = subprocess.run(flags + ['-std=c++17', '-O1', '-g0', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-deprecated-declarations', str(harness), '-o', str(out / name)],
        capture_output=True, text=True)
    (out / (name + '-build.log')).write_text(built.stdout + built.stderr)
    assert built.returncode == 0, built.stderr
    result = subprocess.run([str(out / name)], capture_output=True, text=True, timeout=30)
    (out / (name + '-run.log')).write_text(result.stdout + result.stderr)
    return result


result = run('consumer', body)
assert result.returncode == 0 and 'PASS surface plane forwarding: 15 ' in result.stdout, result.stdout + result.stderr
before_failed = None
if args.before:
    old = method(args.before.read_text(), '- (NSUInteger)iosurfacePlane {')
    previous = run('old-getter', body.replace(getter, old))
    assert previous.returncode != 0, 'Old getter unexpectedly passed the plane-one consumer'
    before_failed = previous.returncode
report = dict(passed=True, checks=15, actual_ObjectiveC_bodies_executed=True,
    source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
    method_bodies_sha256=hashlib.sha256((constructor + surface_getter + getter + view).encode()).hexdigest(),
    old_getter_exit=before_failed, captured_surface_imports=True, real_Vulkan_device=False,
    physical_camera_preview_verified=False, phone_operations=False)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(result.stdout.strip())
