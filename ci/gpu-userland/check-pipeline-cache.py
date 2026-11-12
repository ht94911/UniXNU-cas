"""Check the actual old/new Turnip sharing functions against counted Vulkan cache ownership."""
from pathlib import Path
import hashlib
import json
import subprocess

import argparse
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True);p.add_argument('--patch',type=Path,required=True)
p.add_argument('--out',type=Path,required=True);p.add_argument('--cxx',default='clang++');a=p.parse_args()
O=a.out;O.mkdir(parents=True,exist_ok=False)
new_source=a.source.read_text()
with tempfile.TemporaryDirectory(prefix='old-pipeline-',dir=O) as temporary:
    work=Path(temporary);old=work/'src/freedreno/vulkan/tu_pipeline.cc';old.parent.mkdir(parents=True)
    old.write_text(new_source)
    subprocess.run(['git','apply','--reverse','--check',str(a.patch.resolve())],cwd=work,check=True)
    subprocess.run(['git','apply','--reverse',str(a.patch.resolve())],cwd=work,check=True)
    old_source=old.read_text()
prefix = r'''
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_map>
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
struct vk_device { unsigned number; };
struct vk_pipeline_cache_object { unsigned refs; std::string key; };
struct vk_pipeline_cache {
 struct { vk_device *device; } base;
 std::unordered_map<std::string,vk_pipeline_cache_object *> objects;
 bool retain=true;
 explicit vk_pipeline_cache(vk_device *d):base{d}{}
};
struct tu_device { vk_device vk; vk_pipeline_cache *mem_cache; };
struct tu_shader { vk_pipeline_cache_object base; unsigned serial; };
static unsigned live=0,created=0,destroyed=0;
static const int tu_shader_ops=1;
static vk_pipeline_cache_object *vk_pipeline_cache_object_ref(vk_pipeline_cache_object *o) {
 assert(o && o->refs);++o->refs;return o;
}
static void vk_pipeline_cache_object_unref(vk_device *,vk_pipeline_cache_object *o) {
 assert(o && o->refs);if(!--o->refs){--live;++destroyed;delete container_of(o,tu_shader,base);}
}
static vk_pipeline_cache_object *vk_pipeline_cache_lookup_object(vk_pipeline_cache *c,
 const void *key,size_t n,const int *,bool *hit) {
 if(hit)*hit=false;
 auto i=c->objects.find(std::string((const char *)key,n));
 if(i==c->objects.end())return nullptr;
 if(hit){*hit=true;}
 return vk_pipeline_cache_object_ref(i->second);
}
static vk_pipeline_cache_object *vk_pipeline_cache_add_object(vk_pipeline_cache *c,
 vk_pipeline_cache_object *o) {
 auto i=c->objects.find(o->key);
 if(i!=c->objects.end()) {
  auto *result=vk_pipeline_cache_object_ref(i->second);
  vk_pipeline_cache_object_unref(c->base.device,o);return result;
 }
 if(c->retain)c->objects.emplace(o->key,vk_pipeline_cache_object_ref(o));
 return o;
}
static tu_shader *make(const char *key) {
 ++live;return new tu_shader{{1,key},++created};
}
static void close(vk_pipeline_cache &c) {
 for(auto &x:c.objects)vk_pipeline_cache_object_unref(c.base.device,x.second);
 c.objects.clear();
}
'''
fixture = r'''
static void drop(tu_device &d,tu_shader *s){vk_pipeline_cache_object_unref(&d.vk,&s->base);}
static tu_shader *lookup(vk_pipeline_cache &c,const char *key,bool *hit=nullptr){
 return tu_pipeline_cache_lookup(&c,key,std::char_traits<char>::length(key),hit);
}
int main() {
 tu_device d{{1},nullptr};vk_pipeline_cache shared(&d.vk);d.mem_cache=&shared;
 vk_pipeline_cache a(&d.vk),b(&d.vk),c(&d.vk);
 auto *first=tu_pipeline_cache_insert(&a,make("same-stage"));
 const auto serial=first->serial;assert(serial);drop(d,first);
 bool hit=true;auto *second=lookup(b,"same-stage",&hit);
#if SHARED
 assert(second && second->serial==serial && !hit);drop(d,second);
 second=lookup(b,"same-stage",&hit);assert(second && hit);drop(d,second);
 second=lookup(c,"same-stage",&hit);assert(second && !hit && second->serial==serial);drop(d,second);
 assert(created==1);
#else
 assert(!second && !hit);
 second=tu_pipeline_cache_insert(&b,make("same-stage"));drop(d,second);
 second=tu_pipeline_cache_insert(&c,make("same-stage"));drop(d,second);
 assert(created==3);
#endif
 const unsigned objects_for_three_caches=created;
 close(a);close(b);close(c);
#if SHARED
 assert(live==1);
 second=lookup(a,"same-stage",&hit);assert(second && !hit && second->serial==serial);drop(d,second);
#else
 assert(live==0 && !lookup(a,"same-stage"));
#endif
 // An unmatched key is never supplied by another stage's cache entry.
 assert(!lookup(b,"different-stage"));
 auto *different=tu_pipeline_cache_insert(&b,make("different-stage"));drop(d,different);
 // Duplicate insertion owns and disposes the redundant caller object once.
 const auto prior_destroyed=destroyed;
 different=tu_pipeline_cache_insert(&b,make("different-stage"));
 assert(destroyed==prior_destroyed+1);drop(d,different);
 // A device cache passed directly needs no extra cache and keeps normal feedback.
 auto *direct=tu_pipeline_cache_insert(&shared,make("direct"));drop(d,direct);
 direct=lookup(shared,"direct",&hit);assert(direct && hit);drop(d,direct);
 // Cache insertion may decline retention; the caller still owns a valid result.
 vk_pipeline_cache transient(&d.vk);transient.retain=false;
 auto *one=tu_pipeline_cache_insert(&transient,make("transient"));assert(one && transient.objects.empty());drop(d,one);
 // Device identity keeps equal keys in independent caches.
 tu_device other{{2},nullptr};vk_pipeline_cache other_shared(&other.vk);other.mem_cache=&other_shared;
 vk_pipeline_cache other_app(&other.vk);assert(!lookup(other_app,"same-stage"));
 one=tu_pipeline_cache_insert(&other_app,make("same-stage"));drop(other,one);
 close(other_app);close(other_shared);
 close(a);close(b);close(c);close(shared);close(transient);
 assert(live==0 && created==destroyed);
 printf("{\"shared\":%s,\"objects_for_three_caches\":%u,\"created\":%u,\"destroyed\":%u,\"live\":%u,\"cases\":8}\n",
        SHARED?"true":"false",objects_for_three_caches,created,destroyed,live);
}
'''
rows = []
for name, shared in [('before-tu_pipeline.cc', 0), ('tu_pipeline.cc', 1)]:
    source = old_source if not shared else new_source
    start = source.index('static struct tu_shader *\ntu_pipeline_cache_lookup(')
    end = source.index('\nstatic bool\ntu_nir_shaders_serialize', start)
    fragment = source[start:end]
    path = O / ('shared.cc' if shared else 'before.cc')
    path.write_text(prefix + fragment + fixture)
    binary = path.with_suffix('')
    command = [a.cxx, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-DSHARED=' + str(shared), str(path), '-o', str(binary)]
    compile_result = subprocess.run(command, capture_output=True, text=True)
    (path.with_suffix('.compile.log')).write_text(compile_result.stdout + compile_result.stderr)
    assert compile_result.returncode == 0, compile_result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    path.with_suffix('.run.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    row = json.loads(result.stdout)
    row.update(source_sha256=hashlib.sha256(source.encode()).hexdigest(),
               fragment_sha256=hashlib.sha256(fragment.encode()).hexdigest())
    rows.append(row)
record = dict(passed=True, actual_Turnip_functions=True, mock_Vulkan_cache_callbacks=True,
              cache_callback_contract_source='src/vulkan/runtime/vk_pipeline_cache.c',
              ASan_UBSan=True, hardware_performance=False,
              actual_prepared_source=str(a.source), actual_prepared_source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(), patch_sha256=hashlib.sha256(a.patch.read_bytes()).hexdigest(),
              compiled_shader_substitution_is_fixture=True, rows=rows)
(O / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
