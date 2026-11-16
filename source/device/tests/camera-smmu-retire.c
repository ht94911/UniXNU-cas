/* SPDX-License-Identifier: MIT */
/* Actual shared resource code. MMIO models a disabled camera bank retaining
 * the same TTBR image that DisableOwnedContext leaves after TLB sync. */
#include <Sm8850Resources.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t reg[0x100000 / 4];
static Sm8850Resources resources;
static unsigned writes;
static int locked;
static const uint64_t ttbr0 = (UINT64_C(0xfc00) << 48) | 0x12345000;
static const uint64_t ttbr1 = UINT64_C(0xfc00) << 48;
enum {Bank = (128 + 8) * 4096};
static int lock(void *p) {(void)p; assert(!locked); locked = 1; return 0;}
static void unlock(void *p) {(void)p; assert(locked); locked = 0;}
static int rd(void *p, unsigned domain, uint32_t at, uint32_t *v)
{(void)p; assert(domain == Sm8850Smmu && at < sizeof(reg)); *v = reg[at / 4]; return 0;}
static int rd64(void *p, unsigned domain, uint32_t at, uint64_t *v)
{uint32_t lo, hi; rd(p, domain, at, &lo); rd(p, domain, at + 4, &hi); *v = lo | (uint64_t)hi << 32; return 0;}
static int wr(void *p, unsigned domain, uint32_t at, uint32_t v)
{(void)p; assert(domain == Sm8850Smmu && ((at >= Bank + 0x20 && at <= Bank + 0x2c) || at==0x800+4*80)); ++writes; reg[at / 4] = v; return 0;}
static int fence(void *p) {(void)p; return 0;}
static void init(void)
{
    memset(&resources, 0, sizeof(resources)); memset(reg, 0, sizeof(reg)); writes = 0;
    const Sm8850ResourceIo io = {NULL, lock, unlock, rd, wr, rd64, fence};
    assert(sm8850_resources_init(&resources, &io) == Sm8850Okay);
    reg[0x20 / 4] = (1u << 27) | (1u << 30) | (15u << 9) | 128;
    reg[0x24 / 4] = 0x60000051; reg[0x28 / 4] = 0x5221; reg[0] = 0x406;
    for (unsigned i = 0; i < 128; ++i) reg[(0xc00 + 4 * i) / 4] = 0x20000;
    reg[(Bank + 0x20) / 4] = (uint32_t)ttbr0; reg[(Bank + 0x24) / 4] = (uint32_t)(ttbr0 >> 32);
    reg[(Bank + 0x28) / 4] = (uint32_t)ttbr1; reg[(Bank + 0x2c) / 4] = (uint32_t)(ttbr1 >> 32);
}
int main(void)
{
    init();
    const Sm8850SmmuAssignment assignment = {8, 0xfc00, {80}, {0x1c00}, 1};
    assert(sm8850_smmu_check_assignment(&resources, &assignment) == Sm8850Conflict);
    assert(sm8850_smmu_retire_disabled_bank(&resources, 8, ttbr0, ttbr1) == Sm8850Okay && writes == 4);
    assert(sm8850_smmu_check_assignment(&resources, &assignment) == Sm8850Okay);
    assert(reg[(Bank + 0x20) / 4] == 0 && reg[(Bank + 0x24) / 4] == 0 &&
           reg[(Bank + 0x28) / 4] == 0 && reg[(Bank + 0x2c) / 4] == 0);
    init(); reg[Bank / 4] = 1;
    assert(sm8850_smmu_retire_disabled_bank(&resources, 8, ttbr0, ttbr1) == Sm8850Conflict && !writes);
    init();
    assert(sm8850_smmu_retire_disabled_bank(&resources, 8, ttbr0 + 4096, ttbr1) == Sm8850Conflict && !writes);
    init(); reg[(0x800 + 4 * 80) / 4] = 0x80001c00; reg[(0xc00 + 4 * 80) / 4] = 8;
    assert(sm8850_smmu_retire_disabled_bank(&resources, 8, ttbr0, ttbr1) == Sm8850Conflict && !writes);
    init(); // Another inactive bank retaining this ASID is still a conflict.
    reg[(129 * 4096 + 0x24) / 4] = (uint32_t)(ttbr0 >> 32);
    assert(sm8850_smmu_retire_disabled_bank(&resources, 8, ttbr0, ttbr1) == Sm8850Okay);
    assert(sm8850_smmu_check_assignment(&resources, &assignment) == Sm8850Conflict);
    // FaultStreams retains a valid match even though its route is FAULT.
    // Both that match and the stale TTBR must retire before reacquisition.
    init(); reg[(0x800+4*80)/4]=0x80001c00;
    assert(sm8850_smmu_retire_disabled_bank(&resources,8,ttbr0,ttbr1)==Sm8850Okay);
    assert(sm8850_smmu_check_assignment(&resources,&assignment)==Sm8850Conflict);
    assert(sm8850_smmu_retire_faulted_stream(&resources,80,0x1c00)==Sm8850Okay);
    assert(sm8850_smmu_check_assignment(&resources,&assignment)==Sm8850Okay);
    init(); reg[(0x800+4*80)/4]=0x80001c01;
    assert(sm8850_smmu_retire_faulted_stream(&resources,80,0x1c00)==Sm8850Conflict && !writes);
    init(); reg[(0x800+4*80)/4]=0x80001c00;reg[(0xc00+4*80)/4]=8;
    assert(sm8850_smmu_retire_faulted_stream(&resources,80,0x1c00)==Sm8850Conflict && !writes);
    puts("Shared SMMU retires only exact disabled detached camera TTBRs; actual next assignment passes; active/changed/foreign conflicts remain");
}
