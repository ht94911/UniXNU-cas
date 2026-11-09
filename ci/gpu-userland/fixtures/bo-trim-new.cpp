#include "tu_cpu_profile.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <algorithm>
using VkResult=int;
constexpr int VK_SUCCESS=0,VK_NOT_READY=1;
struct tu_device {};
struct darwin_cached_bo {uint64_t handle,iova,size,stamp,freed_s;void *mapping;uint32_t token;bool read_only;};
struct darwin_bo_cache_stats {uint64_t lookups,hits,inserts,evictions,flushes,bytes_peak;};
struct darwin_retire_state {
 darwin_cached_bo *cache;unsigned cache_count,cache_capacity,cache_trim_cursor,cache_trim_left;
 uint64_t cache_bytes,cache_stamp,cache_cleaned_s;uint32_t pressure_seen;darwin_bo_cache_stats cache_stats;
 struct {uint64_t holes;} va;
};
static uint32_t darwin_memory_pressure;
static uint64_t now_ns;
static unsigned attempts;
static std::set<uint64_t> live,busy;
static uint32_t p_atomic_read(uint32_t *p){return *p;}
static uint64_t os_time_get_nano(){return now_ns;}
static VkResult darwin_release_native(tu_device *,darwin_retire_state *,uint64_t handle,uint64_t,uint64_t,void **,uint32_t *) {
 ++attempts;assert(live.count(handle));if(busy.count(handle))return VK_NOT_READY;
 live.erase(handle);return VK_SUCCESS;
}
static void seed(darwin_retire_state &r,darwin_cached_bo *cache,unsigned n,uint64_t freed=10){
 memset(&r,0,sizeof(r));r.cache=cache;r.cache_count=n;r.cache_capacity=n;r.cache_bytes=4096ull*n;
 live.clear();busy.clear();attempts=0;darwin_memory_pressure=0;now_ns=20000000000ull;
 for(unsigned i=0;i<n;++i){cache[i]={i+1,4096ull*i,4096,i+1,freed,nullptr,0,false};live.insert(i+1);}
}
static VkResult
darwin_cache_evict(struct tu_device *dev, struct darwin_retire_state *retire, unsigned at)
{
   struct darwin_cached_bo *e = &retire->cache[at];
   VkResult result = darwin_release_native(dev, retire, e->handle, e->iova, e->size, &e->mapping, &e->token);
   if (result != VK_SUCCESS)
      return result;
   retire->cache_bytes -= e->size;
   *e = retire->cache[--retire->cache_count];
   ++retire->cache_stats.evictions;
   return VK_SUCCESS;
}

static bool
darwin_cache_flush(struct tu_device *dev, struct darwin_retire_state *retire)
{
   bool any = retire->cache_count != 0;
   for (unsigned i = retire->cache_count; i-- > 0;)
      darwin_cache_evict(dev, retire, i);
   if (any)
      ++retire->cache_stats.flushes;
   return any;
}
static void
darwin_cache_trim(struct tu_device *dev, struct darwin_retire_state *retire)
{
   TU_CPU_PHASE(TU_CPU_CACHE_TRIM);
   const uint32_t pressure = p_atomic_read(&darwin_memory_pressure);
   if (pressure != retire->pressure_seen) {
      retire->pressure_seen = pressure;
      darwin_cache_flush(dev, retire);
      retire->cache_trim_cursor = retire->cache_trim_left = 0;
      return;
   }
   const uint64_t now = (uint64_t)(os_time_get_nano() / 1000000000);
   if (!retire->cache_trim_left) {
      if (now == retire->cache_cleaned_s)
         return;
      retire->cache_cleaned_s = now;
      retire->cache_trim_left = retire->cache_count;
   }
   for (unsigned scanned = 0; scanned < 64 && retire->cache_trim_left && retire->cache_count; ++scanned) {
      if (retire->cache_trim_cursor >= retire->cache_count)
         retire->cache_trim_cursor = 0;
      const unsigned at = retire->cache_trim_cursor;
      --retire->cache_trim_left;
      const uint64_t freed = retire->cache[at].freed_s;
      if (now >= freed && now - freed > 1) {
         /* Successful eviction swaps the last entry into at. Examine that
          * entry on a later call; a busy entry retains its native identity. */
         if (darwin_cache_evict(dev, retire, at) != VK_SUCCESS)
            ++retire->cache_trim_cursor;
         return;
      }
      ++retire->cache_trim_cursor;
   }
   if (!retire->cache_count)
      retire->cache_trim_cursor = retire->cache_trim_left = 0;
}

int main(){darwin_cached_bo cache[512];darwin_retire_state r;tu_device d;
 seed(r,cache,512);darwin_cache_trim(&d,&r);assert(attempts==1 && r.cache_count==511);
 for(unsigned i=0;i<511;++i){unsigned prior=attempts;darwin_cache_trim(&d,&r);assert(attempts-prior<=1);}
 assert(!r.cache_count && !r.cache_bytes && live.empty());
 seed(r,cache,512,20);darwin_cache_trim(&d,&r);assert(!attempts && r.cache_trim_left==448);
 for(unsigned i=0;i<7;++i)darwin_cache_trim(&d,&r);assert(!attempts && !r.cache_trim_left);
 darwin_cache_trim(&d,&r);assert(!attempts && !r.cache_trim_left);
 now_ns+=3000000000ull;for(unsigned i=0;i<512;++i)darwin_cache_trim(&d,&r);
 assert(!r.cache_count && live.empty());
 seed(r,cache,64);busy.insert(1);darwin_cache_trim(&d,&r);assert(attempts==1 && live.count(1) && r.cache_count==64);
 for(unsigned i=0;i<63;++i)darwin_cache_trim(&d,&r);assert(r.cache_count==1 && live.count(1));
 busy.clear();now_ns+=1000000000ull;for(unsigned i=0;i<65 && r.cache_count;++i)darwin_cache_trim(&d,&r);
 assert(!r.cache_count && live.empty());
 seed(r,cache,512);darwin_memory_pressure=1;darwin_cache_trim(&d,&r);
 assert(attempts==512 && !r.cache_count && !r.cache_trim_left && !r.cache_trim_cursor);
 seed(r,cache,8);busy.insert(4);darwin_memory_pressure=1;darwin_cache_trim(&d,&r);
 assert(attempts==8 && r.cache_count==1 && live.count(4));
 busy.clear();darwin_memory_pressure=2;darwin_cache_trim(&d,&r);assert(live.empty());
 // A cache hit swaps the final element while an age sweep is in progress.
 seed(r,cache,512,20);darwin_cache_trim(&d,&r);assert(r.cache_trim_cursor==64);
 for(unsigned i=0;i<256;++i){live.erase(r.cache[i].handle);r.cache[i]=r.cache[--r.cache_count];r.cache_bytes-=4096;}
 now_ns+=3000000000ull;for(unsigned i=0;i<1024 && r.cache_count;++i)darwin_cache_trim(&d,&r);
 assert(!r.cache_count && !r.cache_bytes && live.empty());
 puts("PASS actual cache cleanup: ordinary calls<=1 native release/64 visits; complete eventual cleanup; busy BO retention; pressure flush; swap removal during a sweep");
}
