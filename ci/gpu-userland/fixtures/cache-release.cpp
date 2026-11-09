// Computer peer for the production Darwin retirement/cache functions.
// Native allocations, fence completion and IOKit calls are explicitly simulated.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>
#include "NativeAdrenoABI.h"
#include "tu_cpu_profile.h"
using VkResult=int; using kern_return_t=int; using mach_vm_address_t=uint64_t;
constexpr int VK_SUCCESS=0,VK_NOT_READY=1,VK_TIMEOUT=2,VK_ERROR_DEVICE_LOST=-4;
constexpr int KERN_SUCCESS=0,kIOReturnBusy=7,kIOReturnTimeout=8;
struct dw_connection {uint32_t conn=41;};
struct darwin_va {uint64_t holes=0;};
struct tu_device {void *darwin_retire=nullptr;int vk=0;};
static std::atomic<uint64_t> fixture_clock{10000000000ull};
static uint64_t os_time_get_nano(){return fixture_clock.load();}
static volatile uint32_t darwin_memory_pressure=0;
#define p_atomic_read(p) __atomic_load_n(p,__ATOMIC_RELAXED)
#define MIN2(a,b) std::min((a),(b))
static thread_local bool retire_owned=false;
static int checked_lock(pthread_mutex_t *m){int rc=pthread_mutex_lock(m);assert(!rc&&!retire_owned);retire_owned=true;return rc;}
static int checked_unlock(pthread_mutex_t *m){assert(retire_owned);retire_owned=false;return pthread_mutex_unlock(m);}
#define pthread_mutex_lock checked_lock
#define pthread_mutex_unlock checked_unlock
template<typename... A>static VkResult vk_device_set_lost(int*,const char*,A...){return VK_ERROR_DEVICE_LOST;}
struct darwin_retired_bo {
   uint64_t handle, iova, size;
   void *mapping;
   uint32_t token, fence;
   bool cacheable, read_only;
};

struct darwin_cached_bo {
   uint64_t handle, iova, size, stamp, freed_s;   /* freed_s: monotonic seconds when it entered the cache */
   void *mapping;
   uint32_t token;
   bool read_only;
   bool evicting;   /* a completion caller owns the native release outside lock */
};

struct darwin_bo_cache_stats {
   uint64_t lookups, hits, inserts, evictions, flushes, bytes_peak;
};

struct darwin_retire_state {
   struct dw_connection connection;
   struct darwin_retire_state *next;
   uint64_t va_holes_sample;
   pthread_mutex_t lock;
   struct darwin_retired_bo *entries;   /* a ring of `capacity` records, grown when full */
   unsigned capacity, head, count;
   VkResult failure;
   uint64_t enqueued, reaped, pressure_waits;
   struct darwin_cached_bo *cache;
   unsigned cache_count, cache_capacity;
   unsigned cache_trim_cursor, cache_trim_left;
   uint64_t cache_bytes, cache_stamp, cache_cleaned_s;
   uint32_t pressure_seen;
   struct darwin_bo_cache_stats cache_stats;
   struct darwin_va va;
};

struct PeerBo {
    uint64_t iova=0,size=4096;void *mapping=nullptr;uint32_t token=0;
    int unmap_error=0,lease_error=0,free_error=0;
    unsigned unmaps=0,leases=0,frees=0,returned_iova=0;
    bool allocated=true;
};
struct Peer {
    darwin_retire_state state{};tu_device device{&state};
    std::mutex mutex;std::condition_variable gate;
    std::map<uint64_t,PeerBo> buffers;
    uint64_t block_handle=0;bool blocked=false,proceed=false,fence_ready=true;
    bool expect_unlocked_release=false;
    unsigned native_calls=0,locked_frees=0,unlocked_frees=0;
    Peer(){assert(!pthread_mutex_init(&state.lock,nullptr));state.connection.conn=41;fixture_clock=10000000000ull;darwin_memory_pressure=0;}
    ~Peer(){free(state.cache);free(state.entries);assert(!pthread_mutex_destroy(&state.lock));}
};
static Peer *peer=nullptr;
static uint32_t mach_task_self(){return 1;}
static int IOConnectUnmapMemory64(uint32_t conn,uint32_t token,uint32_t task,mach_vm_address_t address){
    assert(conn==41&&task==1);std::lock_guard<std::mutex> lock(peer->mutex);++peer->native_calls;
    if(peer->expect_unlocked_release)assert(!retire_owned);
    auto found=std::find_if(peer->buffers.begin(),peer->buffers.end(),[&](auto &x){return x.second.token==token;});
    assert(found!=peer->buffers.end());auto &bo=found->second;
    assert(bo.allocated&&reinterpret_cast<uintptr_t>(bo.mapping)==address);++bo.unmaps;
    if(bo.unmap_error)return bo.unmap_error;
    bo.mapping=nullptr;return KERN_SUCCESS;
}
static int dw_call(dw_connection &connection,uint32_t method,const void *input,size_t bytes,void*,size_t){
    assert(connection.conn==41);std::unique_lock<std::mutex> lock(peer->mutex);++peer->native_calls;
    if(method==NativeAdrenoMethodWaitFence){
        assert(bytes==sizeof(NativeAdrenoAbiWaitFence));auto &request=*(const NativeAdrenoAbiWaitFence*)input;
        assert(request.timeout_ns==0);return peer->fence_ready?KERN_SUCCESS:kIOReturnTimeout;
    }
    if(peer->expect_unlocked_release)assert(!retire_owned);
    if(method==NativeAdrenoMethodBoCpuUnmap){
        assert(bytes==sizeof(NativeAdrenoAbiBoCpuUnmap));auto &request=*(const NativeAdrenoAbiBoCpuUnmap*)input;
        auto &bo=peer->buffers.at(request.handle);assert(bo.allocated&&!bo.mapping&&bo.token==request.token);++bo.leases;
        if(bo.lease_error)return bo.lease_error;
        bo.token=0;return KERN_SUCCESS;
    }
    assert(method==NativeAdrenoMethodBoFree&&bytes==sizeof(NativeAdrenoAbiBoFree));
    auto handle=((const NativeAdrenoAbiBoFree*)input)->handle;auto &bo=peer->buffers.at(handle);
    assert(bo.allocated&&!bo.mapping&&!bo.token);++bo.frees;
    if(retire_owned)++peer->locked_frees;else ++peer->unlocked_frees;
    if(handle==peer->block_handle){
        assert(!retire_owned);peer->blocked=true;peer->gate.notify_all();
        assert(peer->gate.wait_for(lock,std::chrono::seconds(5),[]{return peer->proceed;}));
    }
    if(bo.free_error)return bo.free_error;
    bo.allocated=false;return KERN_SUCCESS;
}
static void darwin_free_iova(tu_device *device,uint64_t iova,uint64_t size){
    assert(device==&peer->device);std::lock_guard<std::mutex> lock(peer->mutex);
    auto found=std::find_if(peer->buffers.begin(),peer->buffers.end(),[&](auto &x){return x.second.iova==iova;});
    assert(found!=peer->buffers.end());auto &bo=found->second;
    assert(!bo.allocated&&bo.size==size&&bo.returned_iova++==0);
}
static void darwin_cache_log(darwin_retire_state*){}
static struct dw_connection &
dw_device(struct tu_device *dev)
{
   assert(dev->darwin_retire);
   return ((struct darwin_retire_state *)dev->darwin_retire)->connection;
}

static VkResult
dw_status_to_vk(struct tu_device *dev, kern_return_t kr, const char *what)
{
   if (kr == KERN_SUCCESS)
      return VK_SUCCESS;
   if (kr == kIOReturnTimeout)
      return VK_TIMEOUT;
   return vk_device_set_lost(&dev->vk, "%s failed: 0x%x", what, kr);
}

static VkResult
dw_wait_fence_raw(struct tu_device *dev, uint32_t fence, uint64_t abs_timeout_ns)
{
   struct dw_connection &dw = dw_device(dev);
   for (;;) {
      uint64_t now = os_time_get_nano();
      /* IOKit's default IPC read lock outlives the driver's own lock.
       * A sleeping WaitFence external method therefore blocks CPU map/unmap,
       * which require the IPC write lock on this same connection. Return to
       * IOKit at least once per requested millisecond, preserving the caller's
       * original absolute deadline and completion/error handling. */
      uint64_t slice = 1000000; /* 1 ms per external method, not total timeout */
      if (abs_timeout_ns != UINT64_MAX)
         slice = abs_timeout_ns > now ? MIN2(abs_timeout_ns - now, slice) : 0;
      NativeAdrenoAbiWaitFence req = {.fence = fence, .timeout_ns = slice};
      kern_return_t kr = dw_call(dw, NativeAdrenoMethodWaitFence, &req, sizeof(req), NULL, 0);
      if (kr == KERN_SUCCESS)
         return VK_SUCCESS;
      if (kr != kIOReturnTimeout)
         return dw_status_to_vk(dev, kr, "wait fence");
      if (slice == 0)
         return VK_TIMEOUT;
      if (abs_timeout_ns != UINT64_MAX && os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;
   }
}

static VkResult
darwin_release_native_unlocked(struct tu_device *dev, uint64_t handle, uint64_t iova,
                      uint64_t size, void **mapping, uint32_t *token)
{
   struct dw_connection &dw = dw_device(dev);
   if (*token) {
      if (*mapping) {
         kern_return_t kr;
         {
            TU_CPU_PHASE(TU_CPU_CPU_UNMAP);
            kr = IOConnectUnmapMemory64(dw.conn, *token, mach_task_self(), (mach_vm_address_t)(uintptr_t)*mapping);
         }
         if (kr != KERN_SUCCESS)
            return vk_device_set_lost(&dev->vk, "deferred CPU unmap failed: 0x%x", kr);
         *mapping = NULL;
      }
      NativeAdrenoAbiBoCpuUnmap undo = {.handle = handle, .token = *token};
      kern_return_t kr = dw_call(dw, NativeAdrenoMethodBoCpuUnmap, &undo, sizeof(undo), NULL, 0);
      if (kr != KERN_SUCCESS)
         return vk_device_set_lost(&dev->vk, "deferred CPU lease release failed: 0x%x", kr);
      *token = 0;
   }
   NativeAdrenoAbiBoFree release = {.handle = handle};
   kern_return_t kr = dw_call(dw, NativeAdrenoMethodBoFree, &release, sizeof(release), NULL, 0);
   if (kr == kIOReturnBusy)
      return VK_NOT_READY;
   if (kr != KERN_SUCCESS)
      return vk_device_set_lost(&dev->vk, "deferred BoFree failed: 0x%x", kr);
   darwin_free_iova(dev, iova, size);
   return VK_SUCCESS;
}

static VkResult
darwin_release_native(struct tu_device *dev, struct darwin_retire_state *retire, uint64_t handle, uint64_t iova,
                      uint64_t size, void **mapping, uint32_t *token)
{
   VkResult result = darwin_release_native_unlocked(dev, handle, iova, size, mapping, token);
   if (result != VK_SUCCESS && result != VK_NOT_READY)
      retire->failure = result;
   return result;
}

static VkResult
darwin_cache_evict(struct tu_device *dev, struct darwin_retire_state *retire, unsigned at)
{
   struct darwin_cached_bo *e = &retire->cache[at];
   if (e->evicting)
      return VK_NOT_READY;
   VkResult result = darwin_release_native(dev, retire, e->handle, e->iova, e->size, &e->mapping, &e->token);
   if (result != VK_SUCCESS)
      return result;
   retire->cache_bytes -= e->size;
   *e = retire->cache[--retire->cache_count];
   ++retire->cache_stats.evictions;
   return VK_SUCCESS;
}

static bool
darwin_cache_put(struct tu_device *dev, struct darwin_retire_state *retire, const struct darwin_retired_bo *bo)
{
   if (retire->cache_count == retire->cache_capacity) {
      const unsigned capacity = retire->cache_capacity ? retire->cache_capacity * 2 : 64;
      struct darwin_cached_bo *cache = (struct darwin_cached_bo *)realloc(retire->cache, capacity * sizeof(*cache));
      if (!cache)
         return false;   /* the BO is released instead */
      retire->cache = cache;
      retire->cache_capacity = capacity;
   }
   retire->cache[retire->cache_count++] = {bo->handle, bo->iova, bo->size, ++retire->cache_stamp,
                                           (uint64_t)(os_time_get_nano() / 1000000000), bo->mapping, bo->token, bo->read_only, false};
   retire->cache_bytes += bo->size;
   if (retire->cache_bytes > retire->cache_stats.bytes_peak)
      retire->cache_stats.bytes_peak = retire->cache_bytes;
   ++retire->cache_stats.inserts;
   return true;
}

static bool
darwin_cache_get(struct darwin_retire_state *retire, uint64_t size, bool read_only, bool need_mapping, struct darwin_cached_bo *out)
{
   TU_CPU_PHASE(TU_CPU_CACHE_GET);
   ++retire->cache_stats.lookups;
   int best = -1;
   for (unsigned i = 0; i < retire->cache_count; ++i) {
      const struct darwin_cached_bo *e = &retire->cache[i];
      /* The class asked for, or a BO up to a quarter larger (the smallest such, then the most recent). */
      if (!e->evicting && e->size >= size && e->size - size <= size / 4 && e->read_only == read_only && (!need_mapping || e->mapping) &&
          (best < 0 || e->size < retire->cache[best].size ||
           (e->size == retire->cache[best].size && e->stamp > retire->cache[best].stamp)))
         best = (int)i;
   }
   if (best >= 0) {
      *out = retire->cache[best];
      retire->cache_bytes -= out->size;
      retire->cache[best] = retire->cache[--retire->cache_count];
      ++retire->cache_stats.hits;
   }
   if (!(retire->cache_stats.lookups & 4095))
      darwin_cache_log(retire);
   return best >= 0;
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

static bool
darwin_cache_trim(struct tu_device *dev, struct darwin_retire_state *retire,
                  bool allow_age, struct darwin_cached_bo *claimed)
{
   TU_CPU_PHASE(TU_CPU_CACHE_TRIM);
   const uint32_t pressure = p_atomic_read(&darwin_memory_pressure);
   if (pressure != retire->pressure_seen) {
      retire->pressure_seen = pressure;
      darwin_cache_flush(dev, retire);
      retire->cache_trim_cursor = retire->cache_trim_left = 0;
      return false;
   }
   if (!allow_age)
      return false;
   const uint64_t now = (uint64_t)(os_time_get_nano() / 1000000000);
   if (!retire->cache_trim_left) {
      if (now == retire->cache_cleaned_s)
         return false;
      retire->cache_cleaned_s = now;
      retire->cache_trim_left = retire->cache_count;
   }
   for (unsigned scanned = 0; scanned < 64 && retire->cache_trim_left && retire->cache_count; ++scanned) {
      if (retire->cache_trim_cursor >= retire->cache_count)
         retire->cache_trim_cursor = 0;
      struct darwin_cached_bo *entry = &retire->cache[retire->cache_trim_cursor++];
      --retire->cache_trim_left;
      if (!entry->evicting && now >= entry->freed_s && now - entry->freed_s > 1) {
         entry->evicting = true;
         *claimed = *entry;
         return true;
      }
   }
   if (!retire->cache_count)
      retire->cache_trim_cursor = retire->cache_trim_left = 0;
   return false;
}

static void
darwin_cache_release_complete(struct tu_device *dev, struct darwin_retire_state *retire,
                              const struct darwin_cached_bo *claimed, VkResult result)
{
   for (unsigned at = 0; at < retire->cache_count; ++at) {
      struct darwin_cached_bo *entry = &retire->cache[at];
      if (entry->handle != claimed->handle || entry->stamp != claimed->stamp || !entry->evicting)
         continue;
      if (result == VK_SUCCESS) {
         retire->cache_bytes -= entry->size;
         *entry = retire->cache[--retire->cache_count];
         ++retire->cache_stats.evictions;
      } else {
         entry->mapping = claimed->mapping;
         entry->token = claimed->token;
         entry->evicting = false;
         if (result != VK_NOT_READY)
            retire->failure = result;
      }
      return;
   }
   retire->failure = vk_device_set_lost(&dev->vk, "claimed cache release lost its native identity");
}

static VkResult
darwin_reap_locked(struct tu_device *dev, struct darwin_retire_state *retire)
{
   TU_CPU_PHASE(TU_CPU_RETIRE_REAP);
   if (retire->failure != VK_SUCCESS)
      return retire->failure;
   /* Cache only within this sweep. Persisting a bare 32-bit fence across sweeps
    * could mistake a later wrapped value for an old observed completion. */
   uint32_t completed_fence = 0, pending_fence = 0;
   bool have_completed_fence = false, have_pending_fence = false;
   /* Every entry is looked at, not only the oldest: one BO behind a fence still in flight must not hold back the completed ones queued
    * after it (they would miss the cache). Entries that are not done yet keep their order at the front of the ring. */
   const unsigned total = retire->count;
   unsigned kept = 0;
   for (unsigned i = 0; i < total; ++i) {
      struct darwin_retired_bo *bo = &retire->entries[(retire->head + i) % retire->capacity];
      bool ready = true;
      if (bo->fence) {
         if (have_pending_fence && bo->fence == pending_fence) {
            ready = false;
         } else if (!have_completed_fence || bo->fence != completed_fence) {
            VkResult result = dw_wait_fence_raw(dev, bo->fence, 0);
            if (result == VK_TIMEOUT) {
               ready = false;
               have_pending_fence = true;
               pending_fence = bo->fence;
            } else if (result != VK_SUCCESS) {
               return retire->failure = result;
            } else {
               have_completed_fence = true;
               completed_fence = bo->fence;
            }
         }
      }
      if (ready && !(bo->cacheable && darwin_cache_put(dev, retire, bo))) {
         VkResult released = darwin_release_native(dev, retire, bo->handle, bo->iova, bo->size, &bo->mapping, &bo->token);
         if (released == VK_NOT_READY)
            ready = false;   /* the kernel still holds it: try again at the next sweep */
         else if (released != VK_SUCCESS)
            return released;
      }
      if (ready) {
         ++retire->reaped;
      } else {
         retire->entries[(retire->head + kept) % retire->capacity] = *bo;
         ++kept;
      }
   }
   for (unsigned i = kept; i < total; ++i)
      retire->entries[(retire->head + i) % retire->capacity] = {};
   retire->count = kept;
   return VK_SUCCESS;
}

static VkResult
darwin_collect_retired(struct tu_device *dev, bool allow_age)
{
   struct darwin_retire_state *retire = (struct darwin_retire_state *)dev->darwin_retire;
   struct darwin_cached_bo claimed = {};
   { TU_CPU_PHASE(TU_CPU_RETIRE_LOCK); pthread_mutex_lock(&retire->lock); }
   VkResult result = darwin_reap_locked(dev, retire);
   const bool release = result == VK_SUCCESS && darwin_cache_trim(dev, retire, allow_age, &claimed);
   pthread_mutex_unlock(&retire->lock);
   if (release) {
      TU_CPU_PHASE(TU_CPU_CACHE_TRIM);
      VkResult released = darwin_release_native_unlocked(dev, claimed.handle, claimed.iova,
                                                       claimed.size, &claimed.mapping, &claimed.token);
      { TU_CPU_PHASE(TU_CPU_RETIRE_LOCK); pthread_mutex_lock(&retire->lock); }
      darwin_cache_release_complete(dev, retire, &claimed, released);
      if (retire->failure != VK_SUCCESS)
         result = retire->failure;
      pthread_mutex_unlock(&retire->lock);
   }
   return result;
}

static VkResult
dw_wait_fence_locked_free(struct tu_device *dev, uint32_t fence, uint64_t abs_timeout_ns)
{
   VkResult result = dw_wait_fence_raw(dev, fence, abs_timeout_ns);
   /* Fence observation is also a retirement opportunity, so a quiet application
    * need not allocate another BO to release completed deferred storage. */
   return result == VK_SUCCESS ? darwin_collect_retired(dev, true) : result;
}

static void add(Peer &p,uint64_t handle,uint64_t size=4096,bool old=true){
    assert(retire_owned);
    PeerBo bo;bo.size=size;bo.iova=handle*0x100000;bo.token=(uint32_t)handle+1000;
    bo.mapping=reinterpret_cast<void*>(handle*0x200000);
    {std::lock_guard<std::mutex> lock(p.mutex);assert(p.buffers.emplace(handle,bo).second);}
    darwin_retired_bo retired{};retired.handle=handle;retired.iova=bo.iova;retired.size=size;
    retired.mapping=bo.mapping;retired.token=bo.token;retired.cacheable=true;
    assert(darwin_cache_put(&p.device,&p.state,&retired));
    if(old)p.state.cache[p.state.cache_count-1].freed_s=1;
}
static void lock(Peer &p){pthread_mutex_lock(&p.state.lock);}
static void unlock(Peer &p){pthread_mutex_unlock(&p.state.lock);}
static void empty(Peer &p){
    assert(!p.state.cache_count&&!p.state.cache_bytes&&!p.state.count);
    for(auto &item:p.buffers){auto &bo=item.second;assert(!bo.allocated&&!bo.mapping&&!bo.token&&bo.returned_iova==1);}
}
static unsigned cases=0;
int main(){
    // Allocation/import still reap completed leases, but never start ordinary age eviction.
    {Peer p;peer=&p;lock(p);for(unsigned i=1;i<=3;++i)add(p,i);unlock(p);
     assert(darwin_collect_retired(&p.device,false)==0&&p.native_calls==0&&p.state.cache_count==3);
     p.expect_unlocked_release=true;
     for(unsigned i=0;i<3;++i)assert(darwin_collect_retired(&p.device,true)==0);
     assert(p.unlocked_frees==3&&!p.locked_frees);empty(p);++cases;}
    // Actual completion wrapper selects age cleanup; no new retirement thread.
    {Peer p;peer=&p;lock(p);add(p,1);unlock(p);p.expect_unlocked_release=true;
     assert(dw_wait_fence_locked_free(&p.device,77,0)==0);assert(p.unlocked_frees==1);empty(p);++cases;}
    // Hold one native release. Cache hit, growth, reorder and pressure must be independent.
    {Peer p;peer=&p;lock(p);add(p,1,8192);add(p,2);p.state.cache_trim_cursor=1;unlock(p);p.block_handle=2;
     std::thread completion([&]{assert(darwin_collect_retired(&p.device,true)==0);});
     {std::unique_lock<std::mutex> g(p.mutex);assert(p.gate.wait_for(g,std::chrono::seconds(5),[&]{return p.blocked;}));}
     assert(pthread_mutex_trylock(&p.state.lock)==0);assert(!retire_owned);retire_owned=true;
     darwin_cached_bo hit{};assert(!darwin_cache_get(&p.state,4096,false,true,&hit));
     assert(darwin_cache_evict(&p.device,&p.state,1)==VK_NOT_READY);
     assert(darwin_cache_get(&p.state,8192,false,true,&hit)&&hit.handle==1);
     assert(p.state.cache[0].handle==2&&p.state.cache[0].evicting);
     // Return the borrowed entry, causing ordinary cache mutation during release.
     darwin_retired_bo returned{};returned.handle=hit.handle;returned.iova=hit.iova;returned.size=hit.size;
     returned.mapping=hit.mapping;returned.token=hit.token;returned.cacheable=true;
     assert(darwin_cache_put(&p.device,&p.state,&returned));
     for(unsigned i=3;i<=70;++i)add(p,i);
     assert(p.state.cache_capacity>=128&&p.state.cache[0].handle==2&&p.state.cache[0].evicting);
     assert(darwin_cache_flush(&p.device,&p.state));
     assert(p.state.cache_count==1&&p.state.cache[0].handle==2&&p.state.cache[0].evicting);unlock(p);
     {std::lock_guard<std::mutex> g(p.mutex);p.proceed=true;p.gate.notify_all();}
     completion.join();assert(p.buffers.at(2).frees==1&&p.buffers.at(2).unmaps==1);empty(p);++cases;}
    // Native Busy retains identity and publishes the already-completed unmap/lease progress.
    {Peer p;peer=&p;lock(p);add(p,1);unlock(p);p.expect_unlocked_release=true;p.buffers.at(1).free_error=kIOReturnBusy;
     assert(darwin_collect_retired(&p.device,true)==0);auto &entry=p.state.cache[0];
     assert(entry.handle==1&&!entry.mapping&&!entry.token&&!entry.evicting&&p.state.failure==0);
     assert(!p.buffers.at(1).returned_iova&&p.buffers.at(1).allocated);
     fixture_clock+=2000000000ull;p.buffers.at(1).free_error=0;
     assert(darwin_collect_retired(&p.device,true)==0);
     assert(p.buffers.at(1).unmaps==1&&p.buffers.at(1).leases==1&&p.buffers.at(1).frees==2);empty(p);++cases;}
    // Each error stage keeps precisely the lease state that is still owned.
    for(int stage=0;stage<3;++stage){Peer p;peer=&p;lock(p);add(p,1);unlock(p);p.expect_unlocked_release=true;
     auto &bo=p.buffers.at(1);if(stage==0)bo.unmap_error=99;if(stage==1)bo.lease_error=99;if(stage==2)bo.free_error=99;
     assert(darwin_collect_retired(&p.device,true)==VK_ERROR_DEVICE_LOST&&p.state.cache_count==1);
     auto &entry=p.state.cache[0];assert(!entry.evicting&&entry.mapping==bo.mapping&&entry.token==bo.token);
     assert(p.state.failure==VK_ERROR_DEVICE_LOST&&bo.allocated&&!bo.returned_iova);
     unsigned calls=p.native_calls;assert(darwin_collect_retired(&p.device,false)==VK_ERROR_DEVICE_LOST&&calls==p.native_calls);++cases;}
    // Real pending-fence reaper: a cacheable retired allocation becomes reusable only on completion.
    {Peer p;peer=&p;lock(p);add(p,1);darwin_cached_bo item{};assert(darwin_cache_get(&p.state,4096,false,true,&item));
     p.state.capacity=1;p.state.count=1;p.state.entries=(darwin_retired_bo*)calloc(1,sizeof(darwin_retired_bo));
     auto &entry=p.state.entries[0];entry.handle=item.handle;entry.iova=item.iova;entry.size=item.size;
     entry.mapping=item.mapping;entry.token=item.token;entry.fence=77;entry.cacheable=true;unlock(p);
     p.fence_ready=false;assert(darwin_collect_retired(&p.device,false)==0&&p.state.count==1&&!p.state.cache_count);
     assert(p.buffers.at(1).allocated&&!p.buffers.at(1).unmaps);
     p.fence_ready=true;assert(darwin_collect_retired(&p.device,false)==0&&!p.state.count&&p.state.cache_count==1);
     assert(!p.buffers.at(1).unmaps);fixture_clock+=3000000000ull;p.expect_unlocked_release=true;
     assert(darwin_collect_retired(&p.device,true)==0);empty(p);++cases;}
    // Warning/critical pressure remains immediate even from allocation paths.
    {Peer p;peer=&p;lock(p);for(unsigned i=1;i<=32;++i)add(p,i);unlock(p);darwin_memory_pressure=1;
     assert(darwin_collect_retired(&p.device,false)==0&&p.locked_frees==32&&!p.unlocked_frees);empty(p);++cases;}
    // Young entries are visited in at most the existing 64-entry chunk.
    {Peer p;peer=&p;lock(p);for(unsigned i=1;i<=128;++i)add(p,i,4096,false);unlock(p);
     assert(darwin_collect_retired(&p.device,true)==0&&p.state.cache_trim_left==64&&p.native_calls==0);
     assert(darwin_collect_retired(&p.device,true)==0&&!p.state.cache_trim_left&&p.native_calls==0);
     darwin_memory_pressure=1;assert(darwin_collect_retired(&p.device,false)==0);empty(p);++cases;}
    // Concurrent fence observers race for the same finite cache; every native allocation returns once.
    {Peer p;peer=&p;lock(p);for(unsigned i=1;i<=256;++i)add(p,i);unlock(p);p.expect_unlocked_release=true;
     std::vector<std::thread> workers;for(unsigned n=0;n<4;++n)workers.emplace_back([&]{
       for(unsigned i=0;i<256;++i){fixture_clock.fetch_add(1000000000ull);assert(darwin_collect_retired(&p.device,true)==0);}
     });for(auto &t:workers)t.join();assert(p.unlocked_frees==256&&!p.locked_frees);
     for(auto &item:p.buffers)assert(item.second.frees==1&&item.second.unmaps==1&&item.second.leases==1);
     empty(p);++cases;}
    std::printf("PASS %u cache/reaper cases: normal allocation has no age release; unlocked native cleanup; blocked-release cache reuse/growth/pressure; Busy/error ownership; pending fences; 1024 concurrent collections\n",cases);
}
