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
// One marker per in-flight Mach call, shared by its send/receive events.
// Only an admitted render message creates a marker. A new Mach call or any
// trace loss invalidates it, so an omitted end cannot qualify a later call.
enum { NEZHA_CHAIN_PENDING_MAX = 32 };
static uintptr_t nezha_chain_pending_tid[NEZHA_CHAIN_PENDING_MAX];
static unsigned nezha_chain_pending_count, nezha_chain_pending_drops;

static unsigned nezha_chain_pending_slot(uintptr_t tid) {
    for (unsigned i = 0; i < NEZHA_CHAIN_PENDING_MAX; ++i)
        if (__atomic_load_n(&nezha_chain_pending_tid[i], __ATOMIC_RELAXED) == tid)
            return i;
    return NEZHA_CHAIN_PENDING_MAX;
}

void nezha_input_chain_trace(uint32_t code, uintptr_t a, uintptr_t b,
                            uintptr_t c, uintptr_t d, uintptr_t tid) {
    // Reuse CoreAnimation's existing events. Transaction::commit is an outer
    // boundary; the per-context encoding and command-stream deferral events
    // distinguish it from actual context work. No callback or request is added.
    const uint32_t ca_code=code&~3u;
    // Stock CA command-stream messages use msgh_id 40002..40005. The kernel
    // copyin/copyout events put that id in argument 3 and retain the kmsg token.
    const unsigned render_message=code==0x0120000c || code==0x01200010 ||
        code==0x01200014;
    if (render_message && (c<40002 || c>40005)) return;
    // All Mach call starts/ends retire an old marker, but only the three
    // message-call ends are emitted. Starts are maintenance, not new records.
    const unsigned mach_boundary=(code>>16)==0x010c &&
        ((code&3u)==1 || (code&3u)==2);
    const unsigned render_end=code==0x010c007e || code==0x010c0082 ||
        code==0x010c00be;
    if (mach_boundary && (!tid ||
        !__atomic_load_n(&nezha_chain_pending_count, __ATOMIC_RELAXED) ||
        nezha_chain_pending_slot(tid)==NEZHA_CHAIN_PENDING_MAX)) return;
    // Original UIKit scheduled-begin, main-section and scheduled-done sites.
    // These delimit actual UI callbacks; no update or timer is generated here.
    if (!render_message && !mach_boundary &&
        (code >> 16) != 0x2b8a && (code >> 16) != 0x2b28 &&
        (code >> 16) != 0x0523 && ca_code!=0x31ca0028 &&
        ca_code!=0x31ca0004 && ca_code!=0x31ca0210 &&
        code!=0x31ca00bd && code!=0x31ca00be &&
        code!=0x31ca000d && code!=0x31ca000e && code!=0x31ca023a &&
        ca_code!=0x31ca01dc && ca_code!=0x31ca0214 &&
        ca_code!=0x31ca0218 && ca_code!=0x31ca0260 &&
        ca_code!=0x31ca0264 && ca_code!=0x31ca0270 &&
        ca_code!=0x31ca0224 && ca_code!=0x14ca001c &&
        ca_code!=0x2b8701e4 && ca_code!=0x2b8701e8 &&
        ca_code!=0x2b870210) return;
    // Per-Context encoding crowded out actual input and rendering boundaries.
    if ((ca_code==0x31ca01dc || ca_code==0x14ca001c) &&
        !__atomic_load_n(&nezha_chain_ca_details, __ATOMIC_RELAXED)) return;
    if (__atomic_exchange_n(&nezha_chain_lock, 1, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&nezha_chain_drops, 1, __ATOMIC_RELAXED);
        return;
    }
    // Snapshot losses before admission. A competing writer may increment the
    // counter while this lock is held; that also invalidates this generation.
    const unsigned drops=__atomic_load_n(&nezha_chain_drops, __ATOMIC_RELAXED);
    if (nezha_chain_pending_drops!=drops) {
        for (unsigned i = 0; i < NEZHA_CHAIN_PENDING_MAX; ++i)
            __atomic_store_n(&nezha_chain_pending_tid[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&nezha_chain_pending_count, 0, __ATOMIC_RELAXED);
        nezha_chain_pending_drops=drops;
    }
    unsigned pending=NEZHA_CHAIN_PENDING_MAX;
    if (mach_boundary) {
        pending=nezha_chain_pending_slot(tid);
        if (pending!=NEZHA_CHAIN_PENDING_MAX) {
            __atomic_store_n(&nezha_chain_pending_tid[pending], 0, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&nezha_chain_pending_count, 1, __ATOMIC_RELAXED);
        }
        if (pending==NEZHA_CHAIN_PENDING_MAX || !render_end) {
            __atomic_store_n(&nezha_chain_lock, 0, __ATOMIC_RELEASE);
            return;
        }
    } else if (render_message && tid) {
        pending=nezha_chain_pending_slot(tid);
        if (pending==NEZHA_CHAIN_PENDING_MAX)
            pending=nezha_chain_pending_slot(0);
        if (pending==NEZHA_CHAIN_PENDING_MAX) {
            // No association slot: drop this message rather than leave an
            // admitted message with an untracked or evicted return marker.
            __atomic_fetch_add(&nezha_chain_drops, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&nezha_chain_lock, 0, __ATOMIC_RELEASE);
            return;
        }
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
        if (render_message && tid &&
            !__atomic_load_n(&nezha_chain_pending_tid[pending], __ATOMIC_RELAXED)) {
            __atomic_store_n(&nezha_chain_pending_tid[pending], tid, __ATOMIC_RELAXED);
            __atomic_fetch_add(&nezha_chain_pending_count, 1, __ATOMIC_RELAXED);
        }
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
