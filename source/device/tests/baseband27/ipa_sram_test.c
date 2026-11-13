/* SPDX-License-Identifier: BSD-2-Clause */
/* Execute the actual probe-time SRAM writer with host MMIO storage. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/ipa/nb_ipa55_config.c"

vm_offset_t ml_io_map(vm_offset_t address, vm_size_t bytes)
{
    (void)address;
    (void)bytes;
    return 0;
}

void delay(int microseconds) { (void)microseconds; }

static void check_case(uint32_t offset, uint32_t reported_size, int valid)
{
    uint32_t registers[(CIP_REG_BYTES + 64u) / 4u];
    uint32_t shared[(CIP_SHARED_BYTES + 64u) / 4u];
    uint32_t expected[(CIP_SHARED_BYTES + 64u) / 4u];
    uint8_t zero[CIP_MEM_MAX + 64u];
    memset(registers, 0, sizeof registers);
    memset(shared, 0x6b, sizeof shared);
    memcpy(expected, shared, sizeof expected);
    memset(zero, 0x93, sizeof zero);
    registers[CIPA_SHARED_MEM_SIZE / 4u] =
        cip_encode(CIPA_SHARED_MEM_SIZE_MEM_BADDR, offset / 8u) |
        cip_encode(CIPA_SHARED_MEM_SIZE_MEM_SIZE, reported_size / 8u);
    struct coachz_ipa ipa = {
        .reg = (vm_offset_t)registers,
        .shared = (vm_offset_t)shared,
        .zero_virt = zero,
    };
    assert(mem_config(&ipa) == (valid ? 0 : -EINVAL));
    assert(ipa.mem_offset == offset);
    if (valid) {
        const struct cipa_region *descriptor = &cipa_mem[CIPA_MEM_UC_DESC_RAM];
        const struct cipa_region *command = &cipa_mem[CIPA_MEM_UC_SHARED];
        assert(descriptor->offset == 0 && descriptor->size == 0x300u);
        assert(descriptor->offset + descriptor->size <= command->offset);
        /* Original _ipa_init_sram_v3 publishes the marker after its canaries. */
        for (unsigned i = 0; i < CI_MEM_COUNT; i++)
            for (unsigned k = 0; k < ci_nezha_mem[i].canaries; k++)
                expected[cip_canary_offset(offset, i, k) / 4u] = CIP_CANARY;
        expected[(offset + descriptor->offset) / 4u] = 0xabcdef01u;
        for (unsigned i = 0; i < CIP_MEM_MAX; i++) assert(zero[i] == 0);
        for (unsigned i = CIP_MEM_MAX; i < sizeof zero; i++) assert(zero[i] == 0x93);
        for (unsigned i = offset + command->offset;
             i < offset + command->offset + command->size; i++)
            assert(((uint8_t *)shared)[i] == 0x6b);
    } else {
        for (unsigned i = 0; i < sizeof zero; i++) assert(zero[i] == 0x93);
    }
    /* Covers descriptor tail, uC command/info, modem tables and both bounds. */
    assert(!memcmp(shared, expected, sizeof shared));
}

int main(void)
{
    check_case(0, 0x5000u, 1);       /* Actual 011 SHARED_MEM_SIZE=0xa00. */
    check_case(0, 0x8000u, 1);       /* A larger report keeps the mapped limit. */
    check_case(0x80u, 0x4800u, 1);   /* Nonzero restricted SRAM base. */
    check_case(0, 0x1000u, 0);       /* Invalid table has no SRAM/DMA writes. */
    puts("PASS actual IPA SRAM writer: v5.5 descriptor marker, canaries, uC shared/tail and invalid-size preservation");
    return 0;
}
