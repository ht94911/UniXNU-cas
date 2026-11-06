/* SPDX-License-Identifier: GPL-2.0-only */
/* Narrow XNU12377 Mach-memory binding. Unlike the BSD protocol worker, this
 * unit uses the actual OSFMK compile record and vm_kern_xnu.h typed guards.
 * All allocations and partial mappings remain owned until kernel shutdown.
 * Mapping follows XNU arm/io_map.c, but returns allocation/pmap failure instead
 * of requesting KMA_NOFAIL. It does not touch any SMMU or device register. */
#include "NezhaWifiPlatform.h"
#include <mach/vm_param.h>
#include <vm/vm_kern_xnu.h>
#include <vm/pmap.h>
#include <arm/pmap/pmap.h>
#include <arm/caches_internal.h>

uintptr_t nezha_wifi_map_device(uint64_t physical, size_t length)
{
    if (!length || PAGE_SIZE != 4096 || physical > UINT64_MAX - length) return 0;
    uint64_t offset = physical & PAGE_MASK;
    if (length > UINT64_MAX - offset - PAGE_MASK) return 0;
    vm_size_t bytes = round_page(length + offset);
    vm_offset_t address = 0;
    kern_return_t error = kmem_alloc(kernel_map, &address, bytes,
        KMA_PAGEABLE | KMA_IO | KMA_PERMANENT | KMA_NOPAGEWAIT, VM_KERN_MEMORY_IOKIT);
    if (error != KERN_SUCCESS) return 0;
    physical -= offset;
    for (vm_size_t at = 0; at < bytes; at += PAGE_SIZE) {
        error = pmap_enter_options_addr(kernel_pmap, address + at, physical + at,
            VM_PROT_READ | VM_PROT_WRITE, VM_PROT_NONE, VM_WIMG_IO, TRUE,
            PMAP_OPTIONS_NOWAIT, NULL, PMAP_MAPPING_TYPE_DEFAULT);
        if (error != KERN_SUCCESS) return 0; /* retain any partially mapped entry */
    }
    return address + offset;
}

uintptr_t nezha_wifi_allocate_pages(size_t bytes, unsigned contiguous)
{
    if (!bytes || PAGE_SIZE != 4096 || (bytes & PAGE_MASK) || contiguous > 1) return 0;
    vm_offset_t address = 0;
    kma_flags_t flags = KMA_DATA | KMA_ZERO | KMA_PERMANENT;
    kern_return_t error = contiguous ?
        kmem_alloc_contig(kernel_map, &address, bytes, PAGE_MASK, 0, 0, flags, VM_KERN_MEMORY_IOKIT) :
        kmem_alloc(kernel_map, &address, bytes, flags, VM_KERN_MEMORY_IOKIT);
    return error == KERN_SUCCESS ? address : 0;
}

uint64_t nezha_wifi_physical(uintptr_t address)
{
    ppnum_t page = pmap_find_phys(kernel_pmap, address);
    return page ? ptoa_64(page) + (address & PAGE_MASK) : 0;
}

int nezha_wifi_make_uncached(uintptr_t address, size_t bytes)
{
    if (!address || !bytes || ((address | bytes) & PAGE_MASK) || address > UINTPTR_MAX - bytes)
        return KERN_INVALID_ARGUMENT;
    for (size_t at = 0; at < bytes; at += PAGE_SIZE) {
        ppnum_t page = pmap_find_phys(kernel_pmap, address + at);
        if (!page) return KERN_INVALID_ADDRESS;
        pmap_set_cache_attributes(page, VM_WIMG_WCOMB);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return KERN_SUCCESS;
}

void nezha_wifi_clean_tables(uint64_t physical, size_t bytes)
{
    clean_dcache64(physical, bytes, TRUE);
}
