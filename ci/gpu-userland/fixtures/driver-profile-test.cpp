#include "tu_cpu_profile.cc"
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>

static std::mutex output_lock;
static std::vector<std::vector<TuCpuProfileRecord>> groups;
static void capture(const void *raw, uint32_t count, uint32_t size) {
   assert(size == sizeof(TuCpuProfileRecord));
   assert(count >= 1 && count <= TU_CPU_PHASE_COUNT);
   const auto *records = static_cast<const TuCpuProfileRecord *>(raw);
   assert(records[0].phase == TU_CPU_SUMMARY && records[0].count + 1 == count);
   for (uint32_t i = 0; i < count; ++i) {
      assert(records[i].group == records[0].group);
      assert(records[i].command == records[0].command);
      assert(records[i].thread == records[0].thread);
      assert(records[i].version == 1);
   }
   std::lock_guard<std::mutex> held(output_lock);
   groups.emplace_back(records, records + count);
}
static void sleep_ms(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static void consume_cpu(unsigned ms) {
   uint64_t begin = 0, now = 0;
   assert(cpu_ns(begin));
   do { assert(cpu_ns(now)); } while (now - begin < uint64_t(ms) * 1000000);
}
static const TuCpuProfileRecord &phase(const std::vector<TuCpuProfileRecord> &rows, uint32_t id) {
   for (const auto &row : rows) if (row.phase == id) return row;
   std::abort();
}

using VkResult=int;using kern_return_t=int;using mach_vm_address_t=uint64_t;using mach_vm_size_t=uint64_t;
constexpr int KERN_SUCCESS=0,VK_SUCCESS=0,VK_ERROR_MEMORY_MAP_FAILED=-5;
constexpr unsigned NativeAdrenoMethodBoCpuMap=10,NativeAdrenoMethodBoCpuUnmap=11;
constexpr unsigned kIOMapAnywhere=1,kIOMapCopybackCache=2,kIOMapWriteCombineCache=4,NativeAdrenoParamsOwnedCoherent=1;
struct dw_connection {unsigned conn;struct {unsigned flags;} params;};
struct tu_device {dw_connection recording{41,{1}},waiting{42,{1}};};
struct tu_bo {uint64_t darwin_handle=17,size=4096;void *map=nullptr;uint32_t darwin_token=0;bool never_unmap=false;};
struct NativeAdrenoAbiBoCpuMap {uint64_t handle;};struct NativeAdrenoAbiBoCpuMapReply {uint32_t token;};
struct NativeAdrenoAbiBoCpuUnmap {uint64_t handle;uint32_t token;};
static unsigned map_calls=0,undo_calls=0;static int map_failure=0;
static dw_connection &dw_device(tu_device *d){return d->recording;}
static unsigned mach_task_self(){return 1;}
static int dw_call(dw_connection &d,unsigned method,const void *input,size_t,void *out,size_t){
 assert(d.conn==41);
 if(method==NativeAdrenoMethodBoCpuMap){assert(((const NativeAdrenoAbiBoCpuMap*)input)->handle==17);((NativeAdrenoAbiBoCpuMapReply*)out)->token=71;return 0;}
 assert(method==NativeAdrenoMethodBoCpuUnmap);++undo_calls;return 0;
}
static int IOConnectMapMemory64(unsigned conn,unsigned token,unsigned,mach_vm_address_t *address,mach_vm_size_t *size,unsigned options){
 assert(conn==41 && token==71 && options==(kIOMapAnywhere|kIOMapCopybackCache));++map_calls;
 std::this_thread::sleep_for(std::chrono::milliseconds(12));
 if(map_failure)return map_failure;
 *address=0x100000;*size=4096;return 0;
}
template<typename... A>static int vk_errorf(tu_device *,int code,const char *,A...){return code;}
#define TU_RMV(...) ((void)0)
static VkResult
darwin_bo_map(struct tu_device *dev, struct tu_bo *bo, void *placed_addr)
{
   /* Descriptor updates and other unprofiled entry points can also grow a BO.
    * A surrounding profiled draw/pass keeps ownership of its existing group.
    * Zero means no Vulkan command identity at this lower-level entry point. */
   TuCpuCall cpu_map_call(TU_CPU_MAP_BUFFER, 0);
   struct dw_connection &dw = dw_device(dev);
   NativeAdrenoAbiBoCpuMap req = {.handle = bo->darwin_handle};
   NativeAdrenoAbiBoCpuMapReply reply = {};
   kern_return_t kr = dw_call(dw, NativeAdrenoMethodBoCpuMap, &req, sizeof(req), &reply, sizeof(reply));
   if (kr != KERN_SUCCESS)
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED, "BoCpuMap failed: 0x%x", kr);

   mach_vm_address_t address = (mach_vm_address_t)(uintptr_t)placed_addr;
   mach_vm_size_t length = 0;
   /* IOUserClient::mapClientMemory64 takes the cache mode from THESE flags, not from the descriptor the driver returns, so a plain
    * kIOMapAnywhere would give a cached mapping of memory the GPU reads non-cacheable. Write combine is Normal non-cacheable: the
    * GPU sees CPU writes after a barrier, and memcpy/unaligned stores stay legal (device-memory mappings fault on those). */
   /* With IO-coherent BOs (the kernel reports NativeAdrenoParamsOwnedCoherent) the GPU maps the pages write-back inner-shareable, so the CPU
    * view is a write-back one too: a write-combined view of a page the GPU snoops and the kernel's cacheable alias would disagree. */
   uint32_t options = (placed_addr ? 0 : kIOMapAnywhere) |
                      ((dw.params.flags & NativeAdrenoParamsOwnedCoherent) ? kIOMapCopybackCache : kIOMapWriteCombineCache);
   {
      TU_CPU_PHASE(TU_CPU_CPU_MAP);
      kr = IOConnectMapMemory64(dw.conn, reply.token, mach_task_self(), &address, &length, options);
   }
   if (kr != KERN_SUCCESS || !address || length < bo->size) {
      NativeAdrenoAbiBoCpuUnmap undo = {.handle = bo->darwin_handle, .token = reply.token};
      dw_call(dw, NativeAdrenoMethodBoCpuUnmap, &undo, sizeof(undo), NULL, 0);
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED, "IOConnectMapMemory64 failed: 0x%x", kr);
   }

   bo->map = (void *)(uintptr_t)address;
   bo->darwin_token = reply.token;
   /* The mapping belongs to the kernel descriptor; tu_bo_unmap() must not munmap() it. It goes away in bo_finish. */
   bo->never_unmap = true;
   TU_RMV(bo_map, dev, bo);
   return VK_SUCCESS;
}

int main() {
   { TuCpuCall call(TU_CPU_BEGIN_PASS, 1); TuCpuPhase p(TU_CPU_ATTACHMENTS); sleep_ms(10); }
   assert(groups.empty());
   tu_cpu_profile_set_hook(capture);
   {
      TuCpuCall call(TU_CPU_BEGIN_PASS, 0x123456789abcdef0ull);
      TuCpuPhase parent(TU_CPU_SUBPASS_EMIT);
      sleep_ms(10);
      { TuCpuPhase child(TU_CPU_CS_RESERVE); sleep_ms(12); }
   }
   assert(groups.size() == 1);
   const auto &sleep = groups[0];
   assert(sleep[0].command == 0x123456789abcdef0ull && sleep[0].wall_ns >= 22000000);
   assert(sleep[0].flags & 1);
   const auto &parent = phase(sleep, TU_CPU_SUBPASS_EMIT);
   const auto &child = phase(sleep, TU_CPU_CS_RESERVE);
   assert(parent.wall_ns >= child.wall_ns && parent.count == child.count);
   assert(parent.self_ns + child.self_ns == parent.wall_ns);
   assert(sleep[0].self_ns + parent.self_ns + child.self_ns == sleep[0].wall_ns);
   {
      TuCpuCall call(TU_CPU_DRAW, 2);
      TuCpuPhase p(TU_CPU_DRAW_STATE);
      consume_cpu(16);
   }
   assert(groups.size() == 2 && groups[1][0].cpu_ns >= 16000000);
   assert(groups[1][0].flags & 1);
   {
      TuCpuCall outer(TU_CPU_END_PASS, 3);
      TuCpuPhase p(TU_CPU_RENDER);
      { TuCpuCall nested(TU_CPU_BIND_PIPELINE, 4); sleep_ms(10); }
   }
   assert(groups.size() == 3 && groups[2][0].command == 3);
   std::thread other([] { TuCpuCall c(TU_CPU_UPDATE_BUFFER, 5); TuCpuPhase p(TU_CPU_BO_INIT); sleep_ms(10); });
   other.join();
   assert(groups.size() == 4 && groups[3][0].thread != groups[0][0].thread);

   tu_device map_device;tu_bo mapped;
   assert(darwin_bo_map(&map_device,&mapped,nullptr)==VK_SUCCESS && mapped.map && mapped.never_unmap && mapped.darwin_token==71);
   assert(groups.size()==5 && groups.back()[0].api==TU_CPU_MAP_BUFFER && groups.back()[0].command==0);
   assert(phase(groups.back(),TU_CPU_CPU_MAP).wall_ns>=12000000);
   {
      TuCpuCall parent(TU_CPU_END_PASS,19);
      assert(darwin_bo_map(&map_device,&mapped,nullptr)==VK_SUCCESS);
   }
   assert(groups.size()==6 && groups.back()[0].api==TU_CPU_END_PASS && groups.back()[0].command==19);
   assert(phase(groups.back(),TU_CPU_CPU_MAP).count==1);
   map_failure=7;tu_bo failed;
   assert(darwin_bo_map(&map_device,&failed,nullptr)==VK_ERROR_MEMORY_MAP_FAILED && !failed.map && undo_calls==1 && map_calls==3);
   assert(groups.size()==7 && groups.back()[0].api==TU_CPU_MAP_BUFFER);

   profile_written.store(max_groups - 1);
   { TuCpuCall call(TU_CPU_BIND_PIPELINE, 7); sleep_ms(10); }
   assert(groups.size() == 8 && (groups.back()[0].flags & 8));
   { TuCpuCall call(TU_CPU_BEGIN_PASS, 8); sleep_ms(10); }
   assert(groups.size() == 8);
   tu_cpu_profile_set_hook(nullptr);
   { TuCpuCall call(TU_CPU_DRAW, 6); sleep_ms(10); }
   assert(groups.size() == 8);
   std::puts("PASS: real profiler disabled-hook, nested exclusive time, sleep/CPU distinction, nested API, thread identity and callback layout");
   for (const auto &rows : groups)
      std::printf("api=%u wall_ns=%llu cpu_ns=%llu phases=%u flags=%u\n", rows[0].api,
         (unsigned long long)rows[0].wall_ns, (unsigned long long)rows[0].cpu_ns,
         rows[0].count, rows[0].flags);
}
