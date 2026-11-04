
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <set>
#include <atomic>
#include <thread>
constexpr uint32_t kWidth=3048,kHeight=64;
#define TURNIP_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdFillBuffer) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateFence) X(vkDestroyFence) X(vkQueueSubmit) X(vkWaitForFences) X(vkDeviceWaitIdle) X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdPipelineBarrier) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdDraw) X(vkCmdCopyImageToBuffer)

struct Device {
    VkDevice device = nullptr;
    VkQueue queue = nullptr;
    VkCommandPool pool = nullptr;
    VkCommandBuffer cmd = nullptr;
    VkPhysicalDeviceMemoryProperties memory = {};
#define TURNIP_DECLARE(name) PFN_##name name = nullptr;
    TURNIP_DEVICE_FNS(TURNIP_DECLARE)
#undef TURNIP_DECLARE

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        return UINT32_MAX;
    }
    // Records into the one command buffer, submits it with a fresh fence and waits up to five seconds.
    VkResult submitAndWait() {
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = nullptr;
        VkResult r = vkCreateFence(device, &fci, nullptr, &fence);
        if (r != VK_SUCCESS) return r;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        r = vkQueueSubmit(queue, 1, &si, fence);
        if (r == VK_SUCCESS) r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
        vkDestroyFence(device, fence, nullptr);
        return r;
    }
    VkResult begin() {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(cmd, &bi);
    }
};
struct Fns {
    PFN_vkGetImageSubresourceLayout vkGetImageSubresourceLayout = nullptr;
    PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
    PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
    PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT = nullptr;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
};
struct Target {
    VkImage image = nullptr;
    VkDeviceMemory memory = nullptr;
    VkImageView view = nullptr;
    uint8_t *bytes = nullptr;   // the image's memory as the CPU sees it
    void *host = nullptr;       // the imported allocation (host kind)
    VkDeviceSize pitch = 0;
    bool mapped = false;
};
static void destroy(Device &d, Target &t) {
    if (t.view) d.vkDestroyImageView(d.device, t.view, nullptr);
    if (t.image) d.vkDestroyImage(d.device, t.image, nullptr);
    if (t.memory) {
        if (t.mapped) d.vkUnmapMemory(d.device, t.memory);
        d.vkFreeMemory(d.device, t.memory, nullptr);
    }
    free(t.host);
    t = Target();
}
static int makeTarget(Device &d, Fns &f, bool hostKind, Target &t) {
    VkExternalMemoryImageCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.pNext = hostKind ? &external : nullptr;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_B8G8R8A8_UNORM;
    ic.extent = {kWidth, kHeight, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_LINEAR;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.vkCreateImage(d.device, &ic, nullptr, &t.image) != VK_SUCCESS) return 1;
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout = {};
    f.vkGetImageSubresourceLayout(d.device, t.image, &sub, &layout);
    t.pitch = layout.rowPitch;
    VkMemoryRequirements req = {};
    d.vkGetImageMemoryRequirements(d.device, t.image, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImportMemoryHostPointerInfoEXT import = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    if (hostKind) {
        if (!f.vkGetMemoryHostPointerPropertiesEXT) return 2;
        const size_t size = (req.size + 16383) & ~size_t(16383);
        if (posix_memalign(&t.host, 16384, size) || !t.host) return 2;
        VkMemoryHostPointerPropertiesEXT props = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (f.vkGetMemoryHostPointerPropertiesEXT(d.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, t.host, &props) !=
            VK_SUCCESS) return 3;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = t.host;
        mai.pNext = &import;
        mai.allocationSize = size;
        mai.memoryTypeIndex = d.memoryType(req.memoryTypeBits & props.memoryTypeBits, 0);
        t.bytes = static_cast<uint8_t *>(t.host);
    } else {
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = d.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (mai.memoryTypeIndex == UINT32_MAX) return 4;
    if (d.vkAllocateMemory(d.device, &mai, nullptr, &t.memory) != VK_SUCCESS) return 5;
    if (d.vkBindImageMemory(d.device, t.image, t.memory, 0) != VK_SUCCESS) return 6;
    if (!hostKind) {
        void *mapped = nullptr;
        if (d.vkMapMemory(d.device, t.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) return 7;
        t.mapped = true;
        t.bytes = static_cast<uint8_t *>(mapped) + layout.offset;
    } else {
        t.bytes += layout.offset;
    }
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t.image;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vc.format = ic.format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return d.vkCreateImageView(d.device, &vc, nullptr, &t.view) == VK_SUCCESS ? 0 : 8;
}
static VkResult submitAndDrain(Device &d, bool &quiescent) {
    VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = nullptr;
    VkResult result = d.vkCreateFence(d.device, &info, nullptr, &fence);
    if (result != VK_SUCCESS) return result;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &d.cmd;
    result = d.vkQueueSubmit(d.queue, 1, &submit, fence);
    if (result == VK_SUCCESS) {
        result = d.vkWaitForFences(d.device, 1, &fence, VK_TRUE, 5000000000ull);
        if (result != VK_SUCCESS) {
            const VkResult idle = d.vkDeviceWaitIdle(d.device);
            quiescent = idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST;
        }
    }
    if (quiescent) d.vkDestroyFence(d.device, fence, nullptr);
    return result;
}

static void initial_transition(Device &d,Target &t){
        VkImageMemoryBarrier initial = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        initial.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        initial.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        initial.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        initial.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        initial.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        initial.image = t.image;
        initial.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        d.vkCmdPipelineBarrier(d.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &initial);
}

static int fail=0,images=0,memories=0,views=0,mappings=0,fences=0;
static bool pending=false,transitioned=false;
static VkResult submitResult=VK_SUCCESS,waitResult=VK_SUCCESS,idleResult=VK_SUCCESS;
static std::vector<uint8_t> bytes(kWidth*kHeight*4+16384);
static void configure(Device &d,Fns &f) {
 d.memory.memoryTypeCount=1;d.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 d.vkCreateImage=[](VkDevice,const VkImageCreateInfo *i,const VkAllocationCallbacks*,VkImage *o)->VkResult{
  assert(i->usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT);assert(i->initialLayout==VK_IMAGE_LAYOUT_UNDEFINED);
  if(fail==1)return VK_ERROR_OUT_OF_HOST_MEMORY;*o=(VkImage)1;++images;return VK_SUCCESS;};
 d.vkDestroyImage=[](VkDevice,VkImage,const VkAllocationCallbacks*){assert(images--==1);};
 d.vkGetImageMemoryRequirements=[](VkDevice,VkImage,VkMemoryRequirements *r){r->size=kWidth*kHeight*4;r->alignment=16384;r->memoryTypeBits=fail==4?0:1;};
 d.vkAllocateMemory=[](VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory *o)->VkResult{
  if(fail==5)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*o=(VkDeviceMemory)1;++memories;return VK_SUCCESS;};
 d.vkFreeMemory=[](VkDevice,VkDeviceMemory,const VkAllocationCallbacks*){assert(memories--==1 && !mappings);};
 d.vkBindImageMemory=[](VkDevice,VkImage,VkDeviceMemory,VkDeviceSize)->VkResult{return fail==6?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;};
 d.vkMapMemory=[](VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void **o)->VkResult{
  if(fail==7)return VK_ERROR_MEMORY_MAP_FAILED;*o=bytes.data();++mappings;return VK_SUCCESS;};
 d.vkUnmapMemory=[](VkDevice,VkDeviceMemory){assert(mappings--==1);};
 d.vkCreateImageView=[](VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView *o)->VkResult{
  if(fail==8)return VK_ERROR_OUT_OF_HOST_MEMORY;*o=(VkImageView)1;++views;return VK_SUCCESS;};
 d.vkDestroyImageView=[](VkDevice,VkImageView,const VkAllocationCallbacks*){assert(views--==1);};
 f.vkGetImageSubresourceLayout=[](VkDevice,VkImage,const VkImageSubresource*,VkSubresourceLayout *l){l->rowPitch=kWidth*4;l->offset=0;};
 f.vkGetMemoryHostPointerPropertiesEXT=[](VkDevice,VkExternalMemoryHandleTypeFlagBits,const void*,VkMemoryHostPointerPropertiesEXT *p)->VkResult{
  p->memoryTypeBits=1;return fail==3?VK_ERROR_INVALID_EXTERNAL_HANDLE:VK_SUCCESS;};
 d.vkCreateFence=[](VkDevice,const VkFenceCreateInfo*,const VkAllocationCallbacks*,VkFence *o)->VkResult{
  if(fail==9)return VK_ERROR_OUT_OF_HOST_MEMORY;*o=(VkFence)1;++fences;return VK_SUCCESS;};
 d.vkDestroyFence=[](VkDevice,VkFence,const VkAllocationCallbacks*){assert(!pending && fences--==1);};
 d.vkQueueSubmit=[](VkQueue,uint32_t,const VkSubmitInfo*,VkFence)->VkResult{pending=submitResult==VK_SUCCESS;return submitResult;};
 d.vkWaitForFences=[](VkDevice,uint32_t,const VkFence*,VkBool32,uint64_t)->VkResult{if(waitResult==VK_SUCCESS)pending=false;return waitResult;};
 d.vkDeviceWaitIdle=[](VkDevice)->VkResult{if(idleResult==VK_SUCCESS || idleResult==VK_ERROR_DEVICE_LOST)pending=false;return idleResult;};
 d.vkCmdPipelineBarrier=[](VkCommandBuffer,VkPipelineStageFlags from,VkPipelineStageFlags to,VkDependencyFlags,
     uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t n,const VkImageMemoryBarrier *b){
  assert(from==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT && to==VK_PIPELINE_STAGE_TRANSFER_BIT && n==1);
  assert(b->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && b->newLayout==VK_IMAGE_LAYOUT_GENERAL && b->dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT);
  assert(b->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && b->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);transitioned=true;};
}
int main() {
 for(int kind=0;kind<2;kind++)for(int stage=0;stage<=8;stage++){
  Device d;Fns f;Target t;configure(d,f);fail=stage;
  if(stage==2 && kind)f.vkGetMemoryHostPointerPropertiesEXT=nullptr;
  int code=makeTarget(d,f,kind,t);
  if(!stage)assert(!code);destroy(d,t);assert(!images && !memories && !views && !mappings);
 }
 Device d;Fns f;configure(d,f);fail=0;
 {Target t;t.image=(VkImage)1;initial_transition(d,t);assert(transitioned);}
 for(VkResult waited:{VK_SUCCESS,VK_TIMEOUT,VK_ERROR_DEVICE_LOST})for(VkResult idle:{VK_SUCCESS,VK_ERROR_DEVICE_LOST}){
  bool quiet=true;waitResult=waited;idleResult=idle;assert(submitAndDrain(d,quiet)==waited);assert(quiet && !pending && !fences);
 }
 {bool quiet=true;submitResult=VK_ERROR_DEVICE_LOST;assert(submitAndDrain(d,quiet)==submitResult);assert(!pending && !fences);submitResult=VK_SUCCESS;}
 {bool quiet=true;waitResult=VK_TIMEOUT;idleResult=VK_ERROR_OUT_OF_HOST_MEMORY;assert(submitAndDrain(d,quiet)==VK_TIMEOUT);
  assert(!quiet && pending && fences==1);pending=false;d.vkDestroyFence(d.device,(VkFence)1,nullptr);}
 {bool quiet=true;fail=9;assert(submitAndDrain(d,quiet)==VK_ERROR_OUT_OF_HOST_MEMORY);assert(quiet && !fences);}
 puts("PASS: real swap target/teardown API,partial failures,legal transfer/layout setup,timeout drain before fence destruction,unknown work retained");
}
