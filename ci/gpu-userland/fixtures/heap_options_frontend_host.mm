// SPDX-License-Identifier: MIT
// Real frontend heaps and their reported option fields over the host Vulkan device.
#import "../NativeBridgeMetalFrontend.mm"
#include <cassert>

@interface NBHeapOptionsDescriptor : NSObject
@property(nonatomic) NSUInteger size;
@property(nonatomic) MTLHeapType type;
@property(nonatomic) MTLResourceOptions resourceOptions;
@end
@implementation NBHeapOptionsDescriptor
@end

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        assert(device);
        unsigned checks = 0;
        for (uint64_t raw : {0x200ull, 0x200200ull, 0x400200ull, 0x600200ull, 0x600220ull}) {
            auto descriptor = [NBHeapOptionsDescriptor new];
            descriptor.size = 65536;
            descriptor.resourceOptions = (MTLResourceOptions)raw;
            id<MTLHeap> heap = [device newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor];
            assert(heap && heap.size >= descriptor.size);
            assert((uint64_t)[(NBHeap *)heap resourceOptions] == (raw & ~0x600000ull));
            assert((uint64_t)heap.cpuCacheMode == (raw & 15));
            assert((uint64_t)heap.storageMode == ((raw >> 4) & 15));
            assert((uint64_t)heap.hazardTrackingMode == ((raw >> 8) & 3));
            assert((uint64_t)descriptor.resourceOptions == raw);
            ++checks;
        }
        auto invalid = [NBHeapOptionsDescriptor new];
        invalid.size = 65536;
        invalid.resourceOptions = (MTLResourceOptions)0x600240;
        assert(![device newHeapWithDescriptor:(MTLHeapDescriptor *)invalid]);
        ++checks;
        assert(![(NBDeviceMethods *)device newHeapWithDescriptor:nil]);
        ++checks;
        printf("PASS heap frontend: %u production calls; real heaps and reported fields\n", checks);
    }
    return 0;
}
