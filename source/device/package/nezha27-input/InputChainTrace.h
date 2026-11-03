// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded tracepoint bridge. Writers never allocate, wait, or print.
struct nezha_chain_record {
    uint64_t tick, tid, argument[4];
    uint32_t code;
};
static struct nezha_chain_record nezha_chain_records[4096];
static unsigned nezha_chain_lock, nezha_chain_head, nezha_chain_tail;
static unsigned nezha_chain_drops;

void nezha_input_chain_trace(uint32_t code, uintptr_t a, uintptr_t b,
                            uintptr_t c, uintptr_t d, uintptr_t tid) {
    if ((code >> 24) != 0x2b &&
        (code >> 16) != 0x050c) return;
    if (__atomic_exchange_n(&nezha_chain_lock, 1, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&nezha_chain_drops, 1, __ATOMIC_RELAXED);
        return;
    }
    if (nezha_chain_head - nezha_chain_tail < 4096) {
        struct nezha_chain_record *row = &nezha_chain_records[nezha_chain_head % 4096];
        row->tick = mach_absolute_time();
        row->tid = tid;
        row->code = code;
        row->argument[0] = a;
        row->argument[1] = b;
        row->argument[2] = c;
        row->argument[3] = d;
        ++nezha_chain_head;
    } else {
        __atomic_fetch_add(&nezha_chain_drops, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&nezha_chain_lock, 0, __ATOMIC_RELEASE);
}

static void nezha_input_chain_flush(void) {
    for (unsigned i = 0; i < 1024; ++i) {
        if (__atomic_exchange_n(&nezha_chain_lock, 1, __ATOMIC_ACQUIRE)) break;
        if (nezha_chain_head == nezha_chain_tail) {
            __atomic_store_n(&nezha_chain_lock, 0, __ATOMIC_RELEASE);
            break;
        }
        struct nezha_chain_record row = nezha_chain_records[nezha_chain_tail++ % 4096];
        __atomic_store_n(&nezha_chain_lock, 0, __ATOMIC_RELEASE);
        printf("NEZHA INPUT FLOW: code=%08x tid=%llu ticks=%llu a=%llx b=%llx c=%llx d=%llx drop=%u\n",
            row.code, (unsigned long long)row.tid, (unsigned long long)row.tick,
            (unsigned long long)row.argument[0], (unsigned long long)row.argument[1],
            (unsigned long long)row.argument[2], (unsigned long long)row.argument[3],
            __atomic_load_n(&nezha_chain_drops, __ATOMIC_RELAXED));
    }
}
