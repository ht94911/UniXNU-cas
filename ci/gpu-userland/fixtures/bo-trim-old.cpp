
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
   const uint32_t pressure = p_atomic_read(&darwin_memory_pressure);
   if (pressure != retire->pressure_seen) {
      retire->pressure_seen = pressure;
      darwin_cache_flush(dev, retire);
      return;
   }
   const uint64_t now = (uint64_t)(os_time_get_nano() / 1000000000);
   if (now == retire->cache_cleaned_s)
      return;
   retire->cache_cleaned_s = now;
   for (unsigned i = retire->cache_count; i-- > 0;)
      if (now - retire->cache[i].freed_s > 1)
         darwin_cache_evict(dev, retire, i);
}

int main(){darwin_cached_bo cache[512];darwin_retire_state r;tu_device d;seed(r,cache,512);darwin_cache_trim(&d,&r);
 assert(attempts==512 && !r.cache_count && live.empty());puts("PASS baseline: one ordinary collection synchronously releases512 expired BOs");}
