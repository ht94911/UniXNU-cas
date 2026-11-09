
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using VkResult = int;using kern_return_t = int;
constexpr int VK_SUCCESS=0,VK_TIMEOUT=2,VK_ERROR_DEVICE_LOST=-4;
constexpr int KERN_SUCCESS=0,kIOReturnTimeout=7,NativeAdrenoMethodWaitFence=6;
struct dw_connection {unsigned identity;};
struct darwin_retire_state {dw_connection connection{41},wait_connection{42};};
struct tu_device {darwin_retire_state *darwin_retire;};
struct NativeAdrenoAbiWaitFence {uint32_t fence;uint64_t timeout_ns;};
static darwin_retire_state state;
static uint64_t now_ns,complete_ns;static int failure;
static std::vector<uint64_t> requests;
static uint64_t os_time_get_nano(){return now_ns;}
#define MIN2(a,b) std::min((a),(b))
static kern_return_t dw_call(dw_connection &dw,uint32_t method,const void *in,size_t size,void *,size_t out_size){
 assert(&dw==&state.wait_connection && dw.identity==42);
 assert(method==NativeAdrenoMethodWaitFence && size==sizeof(NativeAdrenoAbiWaitFence) && !out_size);
 const auto &request=*static_cast<const NativeAdrenoAbiWaitFence *>(in);
 assert(request.fence==77 && request.timeout_ns<=100000000);
 requests.push_back(request.timeout_ns);assert(requests.size()<100);
 if(failure)return failure;
 if(complete_ns<=now_ns)return KERN_SUCCESS;
 if(request.timeout_ns && complete_ns-now_ns<=request.timeout_ns){now_ns=complete_ns;return KERN_SUCCESS;}
 now_ns+=request.timeout_ns;return kIOReturnTimeout;
}
static VkResult dw_status_to_vk(tu_device *,kern_return_t kr,const char *){
 return kr==KERN_SUCCESS?VK_SUCCESS:kr==kIOReturnTimeout?VK_TIMEOUT:VK_ERROR_DEVICE_LOST;
}
static void reset(uint64_t completion,int error=0){now_ns=10000000;complete_ns=completion;failure=error;requests.clear();}
static struct dw_connection &
dw_wait_device(struct tu_device *dev)
{
   assert(dev->darwin_retire);
   return ((struct darwin_retire_state *)dev->darwin_retire)->wait_connection;
}
static VkResult
dw_wait_fence_raw(struct tu_device *dev, uint32_t fence, uint64_t abs_timeout_ns)
{
   struct dw_connection &dw = dw_wait_device(dev);
   for (;;) {
      uint64_t now = os_time_get_nano();
      /* Wait on the independent client, preserving the absolute deadline.
       * Long requests avoid repeated IPC while this client has no CPU mappings
       * or BO operations for its read lock to delay. The main client keeps its
       * existing map/unmap and close serialization. */
      uint64_t slice = 100000000; /* 100 ms per independent wait request */
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

int main(){
 tu_device device{&state};
 reset(UINT64_MAX);assert(dw_wait_fence_raw(&device,77,0)==VK_TIMEOUT && requests==std::vector<uint64_t>{0});
 reset(UINT64_MAX);assert(dw_wait_fence_raw(&device,77,9999999)==VK_TIMEOUT && requests==std::vector<uint64_t>{0});
 reset(10000000);assert(dw_wait_fence_raw(&device,77,0)==VK_SUCCESS && requests==std::vector<uint64_t>{0});
 reset(UINT64_MAX);assert(dw_wait_fence_raw(&device,77,14250000)==VK_TIMEOUT && now_ns==14250000);
 assert((requests==std::vector<uint64_t>{4250000}));
 reset(UINT64_MAX);assert(dw_wait_fence_raw(&device,77,235000000)==VK_TIMEOUT && now_ns==235000000);
 assert((requests==std::vector<uint64_t>{100000000,100000000,25000000}));
 reset(223500000);assert(dw_wait_fence_raw(&device,77,UINT64_MAX)==VK_SUCCESS && now_ns==223500000 && requests.size()==3);
 reset(10000010);assert(dw_wait_fence_raw(&device,77,10500000)==VK_SUCCESS && now_ns==10000010 && requests.size()==1);
 reset(UINT64_MAX,9);assert(dw_wait_fence_raw(&device,77,UINT64_MAX)==VK_ERROR_DEVICE_LOST && requests.size()==1);
 puts("PASS actual wait: independent port,zero/expired/completed,finite/infinite deadlines,early completion,error,100ms request cap");
}
