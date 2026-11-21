// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded tracepoint bridge. Writers never allocate, wait, or print.
struct nezha_chain_record {
    uint64_t tick, tid, argument[4];
    uint32_t code;
};
static struct nezha_chain_record nezha_chain_records[4096];
static unsigned nezha_chain_lock, nezha_chain_head, nezha_chain_tail;
static unsigned nezha_chain_drops;
// Keep input delivery, transactions, deferrals and timer callbacks in the
// normal profile. Per-object encoding and contents are available on demand:
// their rate crowded input and scheduling events out of the bounded queue.
static unsigned nezha_chain_ca_details;
enum { NEZHA_CHAIN_FLUSH_MAX = 256 };

void nezha_input_chain_trace(uint32_t code, uintptr_t a, uintptr_t b,
                            uintptr_t c, uintptr_t d, uintptr_t tid) {
    // Reuse CoreAnimation's existing events. Transaction::commit is an outer
    // boundary; the per-context encoding and command-stream deferral events
    // distinguish it from actual context work. No callback or request is added.
    const uint32_t ca_code=code&~3u;
    // Original UIKit scheduled-begin, main-section and scheduled-done sites.
    // These delimit actual UI callbacks; no update or timer is generated here.
    if ((code >> 16) != 0x2b8a && (code >> 16) != 0x2b28 &&
        (code >> 16) != 0x0523 && ca_code!=0x31ca0028 &&
        ca_code!=0x31ca0004 && ca_code!=0x31ca0210 &&
        ca_code!=0x31ca01dc && ca_code!=0x31ca0214 &&
        ca_code!=0x31ca0218 && ca_code!=0x31ca0260 &&
        ca_code!=0x31ca0264 && ca_code!=0x14ca001c &&
        ca_code!=0x2b8701e4 && ca_code!=0x2b8701e8 &&
        ca_code!=0x2b870210) return;
    if ((ca_code==0x31ca01dc || ca_code==0x14ca001c) &&
        !__atomic_load_n(&nezha_chain_ca_details, __ATOMIC_RELAXED)) return;
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

// The existing worker drains short batches, with a time budget between records.
// A slow printf or preemption can exceed it once; it then stops this batch.
static unsigned nezha_input_chain_flush(uint64_t deadline) {
    unsigned emitted = 0;
    for (unsigned i = 0; i < NEZHA_CHAIN_FLUSH_MAX; ++i) {
        if (mach_absolute_time() >= deadline) break;
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
        ++emitted;
    }
    return emitted;
}
