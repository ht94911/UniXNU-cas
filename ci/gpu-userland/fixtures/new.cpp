
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "NativeAdrenoABI.h"
using io_connect_t=uint32_t;using io_service_t=uint32_t;using kern_return_t=int;
constexpr int KERN_SUCCESS=0,kIOReturnBadArgument=(int)0xe00002c2u,kIOReturnUnsupported=(int)0xe00002c7u;
constexpr int VK_SUCCESS=0,VK_ERROR_OUT_OF_HOST_MEMORY=-1,VK_ERROR_INITIALIZATION_FAILED=-3,VK_ERROR_INCOMPATIBLE_DRIVER=-9;
constexpr int VK_SYSTEM_ALLOCATION_SCOPE_DEVICE=0,kIOMainPortDefault=0;
using VkResult=int;
#include "rb_tree.h"
template<typename... A> static void mesa_loge(const char*,A...){}
struct tu_instance{};
struct tu_physical_device {tu_instance *instance;struct {uint32_t chip_id;} dev_id;uint64_t va_start,va_size;};
struct tu_device {int fd=0;void *darwin_retire=nullptr;struct {int alloc=0;} vk;tu_physical_device *physical_device;};
static std::atomic<int> fail_kind{0},mutex_fail_countdown{0},allocations{0};
static std::mutex mock_lock;
struct MockPort {uint64_t next_bo=1;std::map<uint64_t,uint64_t> maps;NativeAdrenoAbiPriority priority{};};
static std::map<uint32_t,MockPort> ports;static uint32_t next_port=100;
static unsigned closes=0,requests=0;
static uint32_t mach_task_self(){return 1;}
static void *IOServiceMatching(const char*){return (void*)1;}
static io_service_t IOServiceGetMatchingService(int,void*){return 1;}
static void IOObjectRelease(io_service_t){}
static int IOServiceOpen(io_service_t,uint32_t,uint32_t,io_connect_t *out){
 std::lock_guard<std::mutex> lock(mock_lock);if(fail_kind==1)return kIOReturnBadArgument;
 *out=next_port++;ports.emplace(*out,MockPort{});return KERN_SUCCESS;
}
static int IOServiceClose(io_connect_t port){
 std::lock_guard<std::mutex> lock(mock_lock);assert(ports.erase(port)==1);++closes;return 0;
}
static int IOConnectCallStructMethod(io_connect_t port,uint32_t method,const void *input,size_t bytes,void *out,size_t *out_bytes){
 std::lock_guard<std::mutex> lock(mock_lock);++requests;auto found=ports.find(port);if(found==ports.end())return kIOReturnBadArgument;
 if(bytes>=8 && ((const uint32_t*)input)[0]==0x4e544e4cu){method=((const uint32_t*)input)[1];input=(const uint8_t*)input+8;bytes-=8;}
 if(method==0xffffffffu)return fail_kind==6?kIOReturnUnsupported:0;
 if(method==NativeAdrenoMethodQuery){
  if(fail_kind==2)return kIOReturnBadArgument;
  assert(out && out_bytes && *out_bytes==sizeof(NativeAdrenoAbiParams));NativeAdrenoAbiParams p{};
  p.abi_version=fail_kind==3?999:NativeAdrenoAbiVersion;p.input_bits=48;p.output_bits=36;p.max_bo_bytes=1ull<<30;
  memcpy(out,&p,sizeof(p));return 0;
 }
 if(method==NativeAdrenoMethodGetDeviceInfo){
  assert(out && out_bytes && *out_bytes==sizeof(NativeAdrenoAbiDeviceInfo));NativeAdrenoAbiDeviceInfo p{};
  p.chip_id=fail_kind==4?0:0x44050a31;p.va_start=fail_kind==5?0:0x20000000;p.va_size=1ull<<30;
  memcpy(out,&p,sizeof(p));return 0;
 }
 if(method==NativeAdrenoMethodSetPriority){found->second.priority=*(const NativeAdrenoAbiPriority*)input;return 0;}
 if(method==NativeAdrenoMethodBoAlloc){auto *p=(NativeAdrenoAbiBoAllocReply*)out;p->handle=found->second.next_bo++;return 0;}
 if(method==NativeAdrenoMethodBoMap){auto *p=(const NativeAdrenoAbiBoMap*)input;assert(!found->second.maps.count(p->iova));found->second.maps[p->iova]=p->handle;return 0;}
 return 0;
}
static void *vk_zalloc(void*,size_t size,size_t,int){if(fail_kind==7)return nullptr;void *p=calloc(1,size);if(p)++allocations;return p;}
static void vk_free(void*,void *p){if(p){--allocations;free(p);}}
template<typename T,typename... A> static VkResult vk_error(T*,VkResult r,A...){return r;}
template<typename T,typename... A> static VkResult vk_errorf(T*,VkResult r,A...){return r;}
template<typename... A> static VkResult vk_startup_errorf(tu_instance*,VkResult r,A...){return r;}
static bool debug_get_bool_option(const char*,bool){return false;}
template<typename... A> static void mesa_logi(const char*,A...){}
static void darwin_watch_memory_pressure(){}
static int tested_mutex_init(pthread_mutex_t *m,const pthread_mutexattr_t *a){
 int n=mutex_fail_countdown.load();if(n>0 && --mutex_fail_countdown==0)return 12;return pthread_mutex_init(m,a);
}
#define pthread_mutex_init tested_mutex_init
#define p_atomic_read(p) __atomic_load_n(p,__ATOMIC_RELAXED)
struct dw_connection {
   io_connect_t conn;
   bool open;
   pthread_mutex_t lock;
   /* Fences of the submissions still possibly in flight, oldest first (the kernel keeps at most eight jobs). */
   uint32_t inflight[8];
   unsigned inflight_count;
   /* Transport. On the tablet a sandboxed process (the Metal plugin inside SpringBoard) reaches the driver through the AGX accelerator
    * service with open type 0x4147, and the sandbox profile decides which external-method selector numbers are admitted, so every ABI
    * method travels as {magic, method, payload} through one admitted selector (the same tunnel the plugin's own test uses). Direct
    * selectors (selector = ABI method) stay available for an unrestricted process that opens NativeAdrenoGpuPublished. */
   bool tunnel;
   uint32_t selector;
   uint32_t last_fence;
   NativeAdrenoAbiDeviceInfo info;
   NativeAdrenoAbiParams params;
};
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
};
struct darwin_bo_cache_stats {
   uint64_t lookups, hits, inserts, evictions, flushes, bytes_peak;
};
struct darwin_va_hole {
   struct rb_node by_address, by_size;
   uint64_t start, size;
};
struct darwin_va {
   struct rb_tree address, size;
   bool seeded;
   uint64_t holes, allocs, frees, merges;
   uint64_t used;   /* bytes handed out */
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
   uint64_t cache_bytes, cache_stamp, cache_cleaned_s;
   uint32_t pressure_seen;
   struct darwin_bo_cache_stats cache_stats;
   struct darwin_va va;
};
static void darwin_cache_log(darwin_retire_state*){}
static volatile uint32_t darwin_memory_pressure;   /* bumped by the memory-pressure source; each device flushes once per change */
static NativeAdrenoAbiPriority dw_priority;
static bool dw_priority_known;
#define DW_TUNNEL_MAGIC 0x4e544e4cu
static pthread_mutex_t dw_registry_lock=PTHREAD_MUTEX_INITIALIZER;
static darwin_retire_state *darwin_states;
static struct dw_connection &
dw_device(struct tu_device *dev)
{
   assert(dev->darwin_retire);
   return ((struct darwin_retire_state *)dev->darwin_retire)->connection;
}
extern "C" __attribute__((visibility("default"))) void
tu_darwin_bo_cache_stats(uint64_t out[8])
{
   memset(out, 0, 8 * sizeof(uint64_t));
   pthread_mutex_lock(&dw_registry_lock);
   for (struct darwin_retire_state *retire = darwin_states; retire; retire = retire->next) {
      pthread_mutex_lock(&retire->lock);
      out[0] += retire->cache_stats.lookups;
      out[1] += retire->cache_stats.hits;
      out[2] += retire->cache_stats.inserts;
      out[3] += retire->cache_stats.evictions;
      out[4] += retire->cache_stats.flushes;
      out[5] += retire->cache_bytes;
      out[6] += retire->cache_stats.bytes_peak;
      out[7] += __atomic_load_n(&retire->va_holes_sample, __ATOMIC_RELAXED);
      pthread_mutex_unlock(&retire->lock);
   }
   pthread_mutex_unlock(&dw_registry_lock);
}
static int
darwin_va_cmp_address(const struct rb_node *a, const struct rb_node *b)
{
   const struct darwin_va_hole *x = rb_node_data(struct darwin_va_hole, a, by_address);
   const struct darwin_va_hole *y = rb_node_data(struct darwin_va_hole, b, by_address);
   return x->start > y->start ? -1 : x->start < y->start ? 1 : 0;
}
static int
darwin_va_cmp_size(const struct rb_node *a, const struct rb_node *b)
{
   const struct darwin_va_hole *x = rb_node_data(struct darwin_va_hole, a, by_size);
   const struct darwin_va_hole *y = rb_node_data(struct darwin_va_hole, b, by_size);
   if (x->size != y->size)
      return x->size > y->size ? -1 : 1;
   return x->start > y->start ? -1 : x->start < y->start ? 1 : 0;
}
static struct darwin_va_hole *
darwin_va_smallest_fit(struct darwin_va *va, uint64_t size)
{
   struct rb_node *best = NULL;
   for (struct rb_node *x = va->size.root; x;) {
      if (rb_node_data(struct darwin_va_hole, x, by_size)->size >= size) {
         best = x;
         x = x->left;
      } else {
         x = x->right;
      }
   }
   return best ? rb_node_data(struct darwin_va_hole, best, by_size) : NULL;
}
static struct darwin_va_hole *
darwin_va_hole_at_or_below(struct darwin_va *va, uint64_t address)
{
   struct rb_node *best = NULL;
   for (struct rb_node *x = va->address.root; x;) {
      if (rb_node_data(struct darwin_va_hole, x, by_address)->start <= address) {
         best = x;
         x = x->right;
      } else {
         x = x->left;
      }
   }
   return best ? rb_node_data(struct darwin_va_hole, best, by_address) : NULL;
}
static bool
darwin_va_add(struct darwin_va *va, uint64_t start, uint64_t size)
{
   if (!size)
      return true;
   struct darwin_va_hole *hole = (struct darwin_va_hole *)calloc(1, sizeof(*hole));
   if (!hole)
      return false;   /* the range is lost, not reused: never handed out twice */
   hole->start = start;
   hole->size = size;
   rb_tree_insert(&va->address, &hole->by_address, darwin_va_cmp_address);
   rb_tree_insert(&va->size, &hole->by_size, darwin_va_cmp_size);
   ++va->holes;
   return true;
}
static void
darwin_va_del(struct darwin_va *va, struct darwin_va_hole *hole)
{
   rb_tree_remove(&va->address, &hole->by_address);
   rb_tree_remove(&va->size, &hole->by_size);
   --va->holes;
   free(hole);
}
static void
darwin_va_finish(struct darwin_va *va)
{
   /* rb_tree_foreach_safe caches only the successor. That successor can still
    * follow parent pointers through the node just freed. Unlink both indices
    * before releasing any hole, using the same removal as carve and merge. */
   while (struct rb_node *node = rb_tree_first(&va->address))
      darwin_va_del(va, rb_node_data(struct darwin_va_hole, node, by_address));
}
static void
darwin_va_carve(struct darwin_va *va, struct darwin_va_hole *hole, uint64_t at, uint64_t size)
{
   const uint64_t begin = hole->start, end = hole->start + hole->size;
   darwin_va_del(va, hole);
   darwin_va_add(va, begin, at - begin);
   darwin_va_add(va, at + size, end - (at + size));
   ++va->allocs;
   va->used += size;
}
static uint64_t
darwin_va_alloc(struct darwin_va *va, uint64_t size, uint64_t alignment, bool high)
{
   if (!size || !alignment || (alignment & (alignment - 1)))
      return 0;
   struct darwin_va_hole *hole = darwin_va_smallest_fit(va, size);
   while (hole) {
      const uint64_t end = hole->start + hole->size;
      uint64_t at;
      bool fits;
      if (high) {
         at = (end - size) & ~(alignment - 1);
         fits = at >= hole->start;
      } else {
         at = (hole->start + alignment - 1) & ~(alignment - 1);
         fits = at >= hole->start && at <= end && end - at >= size;
      }
      if (fits) {
         darwin_va_carve(va, hole, at, size);
         return at;
      }
      struct rb_node *next = rb_node_next(&hole->by_size);
      hole = next ? rb_node_data(struct darwin_va_hole, next, by_size) : NULL;
   }
   return 0;
}
static bool
darwin_va_alloc_at(struct darwin_va *va, uint64_t address, uint64_t size)
{
   struct darwin_va_hole *hole = darwin_va_hole_at_or_below(va, address);
   if (!size || !hole || address - hole->start >= hole->size || hole->start + hole->size - address < size)
      return false;
   darwin_va_carve(va, hole, address, size);
   return true;
}
static void
darwin_va_free(struct darwin_va *va, uint64_t address, uint64_t size)
{
   if (!size)
      return;
   struct darwin_va_hole *below = darwin_va_hole_at_or_below(va, address);
   struct rb_node *after = below ? rb_node_next(&below->by_address) : rb_tree_first(&va->address);
   struct darwin_va_hole *above = after ? rb_node_data(struct darwin_va_hole, after, by_address) : NULL;
   if ((below && below->start + below->size > address) || (above && above->start < address + size)) {
      mesa_loge("darwin: GPU range %#llx+%#llx freed over a free hole", (unsigned long long)address,
                (unsigned long long)size);
      return;
   }
   uint64_t begin = address, end = address + size;
   if (below && below->start + below->size == address) {
      begin = below->start;
      darwin_va_del(va, below);
      ++va->merges;
   }
   if (above && above->start == end) {
      end = above->start + above->size;
      darwin_va_del(va, above);
      ++va->merges;
   }
   darwin_va_add(va, begin, end - begin);
   ++va->frees;
   va->used -= size;
}
static kern_return_t
dw_call(struct dw_connection &dw, uint32_t method, const void *in, size_t in_size, void *out,
        size_t out_size)
{
   size_t out_bytes = out_size;
   if (!dw.tunnel)
      return IOConnectCallStructMethod(dw.conn, method, in, in_size, out_size ? out : NULL,
                                       out_size ? &out_bytes : NULL);
   uint8_t buffer[8 + sizeof(NativeAdrenoAbiSubmit)];
   if (in_size > sizeof(NativeAdrenoAbiSubmit))
      return kIOReturnBadArgument;
   const uint32_t header[2] = {DW_TUNNEL_MAGIC, method};
   memcpy(buffer, header, sizeof(header));
   if (in_size)
      memcpy(buffer + sizeof(header), in, in_size);
   return IOConnectCallStructMethod(dw.conn, dw.selector, buffer, sizeof(header) + in_size, out_size ? out : NULL,
                                    out_size ? &out_bytes : NULL);
}
static bool
dw_probe_selector(struct dw_connection &dw)
{
   const uint32_t header[2] = {DW_TUNNEL_MAGIC, 0xffffffffu};
   for (uint32_t s = 0; s < 256; s++) {
      if (IOConnectCallStructMethod(dw.conn, s, header, sizeof(header), NULL, NULL) == KERN_SUCCESS) {
         dw.selector = s;
         return true;
      }
   }
   return false;
}
static VkResult
dw_open_connection(struct dw_connection &dw, struct tu_instance *instance)
{
   if (dw.open || dw.conn)
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED, "connection already owned");
   /* TU_DARWIN_SERVICE names the IOKit service (default: the AGX accelerator the original Metal discovery finds), TU_DARWIN_OPEN_TYPE its open
    * type (default 0x4147, the type the driver serves to the plugin). NativeAdrenoGpuPublished with type 0 is the unsandboxed direct form. */
   const char *service_name = getenv("TU_DARWIN_SERVICE");
   if (!service_name || !*service_name)
      service_name = "IOAcceleratorES";
   const char *type_text = getenv("TU_DARWIN_OPEN_TYPE");
   uint32_t open_type = type_text && *type_text ? (uint32_t)strtoul(type_text, NULL, 0) : 0x4147u;
   const bool direct = !strcmp(service_name, "NativeAdrenoGpuPublished");
   if (direct && !(type_text && *type_text))
      open_type = 0;
   io_service_t service = IOServiceGetMatchingService(
      kIOMainPortDefault, IOServiceMatching(service_name));
   if (!service)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   kern_return_t kr = IOServiceOpen(service, mach_task_self(), open_type, &dw.conn);
   IOObjectRelease(service);
   if (kr != KERN_SUCCESS) {
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                               "IOServiceOpen(%s, 0x%x) failed: 0x%x", service_name, open_type, kr);
   }
   dw.tunnel = !direct;
   if (dw.tunnel && !dw_probe_selector(dw)) {
      IOServiceClose(dw.conn);
      dw.conn = 0;
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED, "no external-method selector is admitted for this process");
   }

   kr = dw_call(dw, NativeAdrenoMethodQuery, NULL, 0, &dw.params, sizeof(dw.params));
   if (kr == KERN_SUCCESS && dw.params.abi_version != NativeAdrenoAbiVersion)
      kr = kIOReturnUnsupported;
   if (kr == KERN_SUCCESS)
      kr = dw_call(dw, NativeAdrenoMethodGetDeviceInfo, NULL, 0, &dw.info, sizeof(dw.info));
   if (kr != KERN_SUCCESS) {
      IOServiceClose(dw.conn);
      dw.conn = 0;
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                               "NativeAdreno query failed: 0x%x", kr);
   }
   dw.open = true;
   dw.inflight_count = 0;
   dw.last_fence = 0;

   /* Normal until Metal queues publish their current requests. An explicit environment override is also accepted. */
   {
      const char *want = getenv("METAL2VULKAN_QUEUE_PRIORITY");
      NativeAdrenoAbiPriority priority = {.foreground = 1, .background = 3};
      if (want && !strcmp(want, "high")) {
         priority.foreground = 0;
         priority.background = 2;
      } else if (want && !strcmp(want, "low")) {
         priority.foreground = 2;
         priority.background = 2;
      }
      pthread_mutex_lock(&dw_registry_lock);
      if (dw_priority_known)
         priority = dw_priority;
      else {
         dw_priority = priority;
         dw_priority_known = true;
      }
      pthread_mutex_unlock(&dw_registry_lock);
      (void)dw_call(dw, NativeAdrenoMethodSetPriority, &priority, sizeof(priority), NULL, 0);
   }
   return VK_SUCCESS;
}
extern "C" __attribute__((visibility("default"))) bool
tu_darwin_set_priority(uint32_t foreground, uint32_t background)
{
   if ((foreground != UINT32_MAX && (foreground > 5 || foreground == 3)) ||
       (background != UINT32_MAX && background > 5))
      return false;
   pthread_mutex_lock(&dw_registry_lock);
   bool accepted = dw_priority_known && darwin_states;
   if (dw_priority_known) {
      NativeAdrenoAbiPriority priority = dw_priority;
      if (foreground != UINT32_MAX) priority.foreground = foreground;
      if (background != UINT32_MAX) priority.background = background;
      /* The frontend aggregates current live queues; lowering releases an old boost. */
      for (struct darwin_retire_state *state = darwin_states; state; state = state->next) {
         const bool updated = dw_call(state->connection, NativeAdrenoMethodSetPriority,
                                      &priority, sizeof(priority), NULL, 0) == KERN_SUCCESS;
         accepted = accepted && updated;
      }
      dw_priority = priority;
   }
   pthread_mutex_unlock(&dw_registry_lock);
   return accepted;
}
extern "C" __attribute__((visibility("default"))) bool
tu_darwin_note(uint32_t kind, const uint32_t *words, uint32_t count)
{
   if (count > NativeAdrenoNoteWords || (count && !words))
      return false;
   NativeAdrenoAbiNote note = {};
   note.kind = kind;
   note.count = count;
   if (count)
      memcpy(note.words, words, count * sizeof(uint32_t));
   pthread_mutex_lock(&dw_registry_lock);
   bool result = darwin_states && dw_call(darwin_states->connection, NativeAdrenoMethodNote,
                                         &note, sizeof(note), NULL, 0) == KERN_SUCCESS;
   pthread_mutex_unlock(&dw_registry_lock);
   return result;
}
extern "C" __attribute__((visibility("default"))) bool
tu_darwin_frame_diagnostic(uint32_t method,const void *data,uint32_t bytes)
{
   if(!data || method!=0x7102 ||
      bytes!=120)return false;
   pthread_mutex_lock(&dw_registry_lock);
   bool result = darwin_states && dw_call(darwin_states->connection,method,data,bytes,NULL,0)==KERN_SUCCESS;
   pthread_mutex_unlock(&dw_registry_lock);
   return result;
}
static void
dw_close_connection(struct dw_connection &dw)
{
   if (dw.open) {
      IOServiceClose(dw.conn);
      dw.conn = 0;
      dw.open = false;
   }
}
static VkResult
darwin_device_init(struct tu_device *dev)
{
   dev->fd = -1;
   struct darwin_retire_state *retire = (struct darwin_retire_state *)
      vk_zalloc(&dev->vk.alloc, sizeof(*retire), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!retire)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   if (pthread_mutex_init(&retire->lock, NULL)) {
      vk_free(&dev->vk.alloc, retire);
      return vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
   }
   if (pthread_mutex_init(&retire->connection.lock, NULL)) {
      pthread_mutex_destroy(&retire->lock);
      vk_free(&dev->vk.alloc, retire);
      return vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
   }
   VkResult result = dw_open_connection(retire->connection, dev->physical_device->instance);
   if (result == VK_SUCCESS &&
       (retire->connection.info.chip_id != dev->physical_device->dev_id.chip_id ||
        retire->connection.info.va_start != dev->physical_device->va_start ||
        retire->connection.info.va_size != dev->physical_device->va_size))
      result = vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                        "device connection differs from enumerated GPU/VA contract");
   if (result != VK_SUCCESS) {
      dw_close_connection(retire->connection);
      pthread_mutex_destroy(&retire->connection.lock);
      pthread_mutex_destroy(&retire->lock);
      vk_free(&dev->vk.alloc, retire);
      return result;
   }
   dev->darwin_retire = retire;
   pthread_mutex_lock(&dw_registry_lock);
   struct darwin_retire_state **tail = &darwin_states;
   while (*tail)
      tail = &(*tail)->next;
   *tail = retire;
   /* A process priority request may have arrived while the port was opening. */
   if (dw_priority_known)
      (void)dw_call(retire->connection, NativeAdrenoMethodSetPriority,
                    &dw_priority, sizeof(dw_priority), NULL, 0);
   pthread_mutex_unlock(&dw_registry_lock);
   darwin_watch_memory_pressure();
   retire->pressure_seen = p_atomic_read(&darwin_memory_pressure);
   return VK_SUCCESS;
}
static void
darwin_device_finish(struct tu_device *dev)
{
   struct darwin_retire_state *retire = (struct darwin_retire_state *)dev->darwin_retire;
   if (retire) {
      /* First withdraw from callbacks; no callback can keep a dangling port or
       * stats pointer. Only this VkDevice's kernel client is then drained. */
      pthread_mutex_lock(&dw_registry_lock);
      struct darwin_retire_state **link = &darwin_states;
      while (*link && *link != retire)
         link = &(*link)->next;
      assert(*link == retire);
      if (*link)
         *link = retire->next;
      pthread_mutex_unlock(&dw_registry_lock);
      dw_close_connection(retire->connection);
      /* Some CreateDevice failure paths already destroyed the VMA heap/mutex.
       * Never return ranges here: they are either gone or about to be destroyed. */
      if (debug_get_bool_option("TU_DARWIN_RETIRE_TRACE", false))
         mesa_logi("darwin BO retirement: enqueued=%llu reaped=%llu pending-at-close=%u pressure=%llu",
                   (unsigned long long)retire->enqueued, (unsigned long long)retire->reaped,
                   retire->count, (unsigned long long)retire->pressure_waits);
      if (retire->cache_stats.lookups)
         darwin_cache_log(retire);
      darwin_va_finish(&retire->va);
      pthread_mutex_destroy(&retire->connection.lock);
      pthread_mutex_destroy(&retire->lock);
      free(retire->entries);
      free(retire->cache);
      vk_free(&dev->vk.alloc, retire);
      dev->darwin_retire = NULL;
   }
}
static void
darwin_note_fence(struct dw_connection &dw, uint32_t fence)
{
   if (dw.inflight_count == 8) {
      memmove(dw.inflight, dw.inflight + 1, 7 * sizeof(dw.inflight[0]));
      dw.inflight_count = 7;
   }
   dw.inflight[dw.inflight_count++] = fence;
   dw.last_fence = fence;
}

static tu_instance instance;
static tu_physical_device physical{&instance,{0x44050a31},0x20000000,1ull<<30};
static tu_device make_device(){tu_device d{};d.physical_device=&physical;return d;}
static size_t live(){std::lock_guard<std::mutex> lock(mock_lock);return ports.size();}

static void query(tu_device &d){NativeAdrenoAbiParams p{};assert(dw_call(dw_device(&d),NativeAdrenoMethodQuery,nullptr,0,&p,sizeof p)==0);}
static void map_same_va(tu_device &d){
 NativeAdrenoAbiBoAlloc a{4096,12,0};NativeAdrenoAbiBoAllocReply r{};
 assert(dw_call(dw_device(&d),NativeAdrenoMethodBoAlloc,&a,sizeof a,&r,sizeof r)==0);
 NativeAdrenoAbiBoMap m{};m.handle=r.handle;m.iova=0x20000000;m.permissions=3;NativeAdrenoAbiBoMapReply mr{};
 assert(dw_call(dw_device(&d),NativeAdrenoMethodBoMap,&m,sizeof m,&mr,sizeof mr)==0);
 auto *state=(darwin_retire_state*)d.darwin_retire;
 for(unsigned i=0;i<17;i++)assert(darwin_va_add(&state->va,0x21000000ull+i*8192,4096));
 __atomic_store_n(&state->va_holes_sample,state->va.holes,__ATOMIC_RELAXED);
}
int main(){
 auto primary=make_device();assert(darwin_device_init(&primary)==0);auto saved=dw_device(&primary).conn;assert(live()==1);
 dw_connection probe{};assert(dw_open_connection(probe,&instance)==0);assert(probe.conn!=saved && live()==2);
 dw_close_connection(probe);assert(live()==1 && dw_device(&primary).conn==saved);query(primary);
 auto secondary=make_device();assert(darwin_device_init(&secondary)==0);assert(dw_device(&secondary).conn!=saved);
 map_same_va(primary);map_same_va(secondary);
 darwin_note_fence(dw_device(&primary),111);darwin_note_fence(dw_device(&secondary),222);
 assert(dw_device(&primary).last_fence==111 && dw_device(&secondary).last_fence==222);
 assert(tu_darwin_set_priority(0,2));
 {std::lock_guard<std::mutex> lock(mock_lock);for(const auto &p:ports)assert(p.second.priority.foreground==0 && p.second.priority.background==2);}
 darwin_device_finish(&secondary);assert(live()==1 && dw_device(&primary).last_fence==111);query(primary);
 for(int kind=1;kind<=7;++kind){fail_kind=kind;auto bad=make_device();assert(darwin_device_init(&bad)!=0);assert(!bad.darwin_retire && live()==1);fail_kind=0;query(primary);}
 for(int n:{1,2}){mutex_fail_countdown=n;auto bad=make_device();assert(darwin_device_init(&bad)!=0);assert(!bad.darwin_retire && live()==1);mutex_fail_countdown=0;query(primary);}
 std::atomic<bool> stop{false};
 std::thread telemetry([&]{uint64_t stats[8];uint32_t word=7;char row[120]{};
  while(!stop){tu_darwin_bo_cache_stats(stats);tu_darwin_note(1,&word,1);tu_darwin_frame_diagnostic(0x7102,row,120);tu_darwin_set_priority(1,2);}
 });
 std::vector<std::thread> workers;
 for(unsigned w=0;w<3;++w)workers.emplace_back([&]{for(unsigned i=0;i<300;++i){
  auto d=make_device();assert(darwin_device_init(&d)==0);query(d);map_same_va(d);
  auto *r=(darwin_retire_state*)d.darwin_retire;pthread_mutex_lock(&r->lock);r->cache_stats.lookups=1;pthread_mutex_unlock(&r->lock);
  __atomic_store_n(&r->va_holes_sample,1ull,__ATOMIC_RELAXED);darwin_device_finish(&d);
 }});
 for(auto &thread:workers)thread.join();stop=true;telemetry.join();
 query(primary);assert(live()==1);darwin_device_finish(&primary);assert(live()==0 && allocations==0 && !darwin_states);
 assert(!tu_darwin_note(1,nullptr,0));uint64_t stats[8];tu_darwin_bo_cache_stats(stats);for(auto n:stats)assert(n==0);
 puts("PASS: per-device ports/VA/fences,probe isolation,secondary teardown,9 init failures,900 concurrent lifecycles with telemetry/priority,no leaked ports or allocations");
}
