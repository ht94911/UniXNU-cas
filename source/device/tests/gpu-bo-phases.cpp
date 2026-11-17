/* SPDX-License-Identifier: MIT */
/* Exercise the production hook adapter and original trace store together. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../package/gpu/NezhaGpuBoPhases.h"
#include "NativeGpuTraceStore.h"

static NativeGpuTraceSlot slots[16];
static NativeGpuTraceStore records;
static bool enabled;
extern "C" int native_gpu_trace_enabled(void) { return enabled; }
extern "C" void native_gpu_trace_record(uint32_t event, uint32_t pid,
                                        const uint64_t *value, uint32_t count)
{
    assert(event == GpuTraceProblem && count == 12);
    NativeGpuTraceRecord record{};
    record.event = event;
    record.pid = pid;
    memcpy(record.value, value, sizeof record.value);
    records.push(record);
}
static const char *text(unsigned sequence, NativeGpuTraceRecord &row)
{
    assert(records.read(sequence, &row));
    assert(row.event == GpuTraceProblem && row.value[0] == 0);
    return reinterpret_cast<const char *>(&row.value[1]);
}
int main()
{
    records.slots = slots;
    records.capacity = 16;
    NativeAdrenoBoTrace trace{};
    trace.entered = UINT64_C(10197094534);
    trace.kib = 65536;
    trace.flags = 14;
    trace.create_us = 150000;
    trace.wire_us = 10000;
    trace.clean_us = 5000;
    const NativeAdrenoBoTrace original = trace;
    nezhaGpuRecordBoPhases(&trace, 64, 793);
    assert(records.issued == 0);
    enabled = true;
    trace.kib = 255;
    nezhaGpuRecordBoPhases(&trace, 64, 793);
    assert(records.issued == 0);
    trace = original;
    trace.create_us = 9999;
    trace.wire_us = trace.clean_us = 0;
    nezhaGpuRecordBoPhases(&trace, 64, 793);
    assert(records.issued == 0);
    trace.create_us = 10000;
    trace.kib = 256;
    nezhaGpuRecordBoPhases(&trace, 64, 793);
    assert(records.issued == 3);
    NativeGpuTraceRecord row{};
    assert(!strcmp(text(1, row),
        "bo0 i=10197094534 t=793 k=256 f=e e=0"));
    assert(row.pid == 64);
    assert(!strcmp(text(2, row),
        "bo1 i=10197094534 l=0 c=10000 w=0 z=0"));
    assert(!strcmp(text(3, row),
        "bo2 i=10197094534 q=0 d=0 r=0 s=0"));
    /* Full-width counters must survive every record without truncated text,
     * wrapped duration filtering or a write past the 88-byte wire payload. */
    trace.entered = UINT64_MAX;
    trace.kib = trace.flags = trace.spans = UINT32_MAX;
    trace.limit_us = trace.create_us = trace.wire_us = trace.clean_us = UINT32_MAX;
    trace.lock_us = trace.dispatch_us = trace.discard_us = UINT32_MAX;
    trace.status = -1;
    NativeAdrenoBoTrace unchanged = trace;
    nezhaGpuRecordBoPhases(&trace, 64, UINT64_MAX);
    assert(records.issued == 6 && !memcmp(&trace, &unchanged, sizeof trace));
    for (unsigned i = 4; i <= 6; ++i) {
        const char *line = text(i, row);
        assert(memchr(line, 0, 88));
        assert(strstr(line, "i=18446744073709551615"));
        if (i == 4) assert(strstr(line, " e=ffffffff"));
        if (i == 5) assert(strstr(line, " z=4294967295"));
        if (i == 6) assert(strstr(line, " s=4294967295"));
        puts(line);
    }
    puts("PASS: disabled/short/small suppression, boundary, integer maxima, original ring and immutable phases");
}
