/* SPDX-License-Identifier: MIT */
#include "a840_frame_weights.h"

#include <string.h>

#define NS_PER_SECOND UINT64_C(1000000000)

static A840FwStatus ticks_to_ns(uint64_t ticks, uint32_t hz, uint64_t *ns)
{
    if (hz == 0)
        return A840_FW_INVALID_ARGUMENT;
    const uint64_t seconds = ticks / hz;
    const uint64_t ns_fraction = (ticks % hz) * NS_PER_SECOND / hz;
    if (seconds > (UINT64_MAX - ns_fraction) / NS_PER_SECOND)
        return A840_FW_OVERFLOW;
    *ns = seconds * NS_PER_SECOND + ns_fraction;
    return A840_FW_OK;
}

uint64_t a840_fw_ticks_to_ns(uint64_t ticks, uint32_t hz)
{
    uint64_t ns;
    return ticks_to_ns(ticks, hz, &ns) == A840_FW_OK ? ns : UINT64_MAX;
}

A840FwStatus a840_fw_interval_to_ns(uint64_t start_ticks, uint64_t end_ticks,
                                  uint64_t *ns)
{
    if (!ns)
        return A840_FW_INVALID_ARGUMENT;
    if (end_ticks < start_ticks)
        return A840_FW_TICK_REVERSED;
    return ticks_to_ns(end_ticks - start_ticks, A840_FW_ALWAYS_ON_HZ, ns);
}

static int valid_scene(uint32_t scene_id)
{
    return scene_id <= A840_FW_SCENE_DENSE_LIQUID_GLASS_SWIPE;
}

static int valid_frame(const A840FwFrameRecord *frame, uint32_t capture_scene)
{
    return valid_scene(frame->scene_id) && frame->ring_id <= 3 &&
           (capture_scene == A840_FW_SCENE_CUSTOM || capture_scene == frame->scene_id);
}

static A840FwStatus frame_margin(const A840FwFrameRecord *frame, int64_t *margin)
{
    const uint64_t stages[] = {frame->pre_create_ns, frame->encode_ns, frame->submit_ns,
                               frame->gpu_exec_ns, frame->wait_sync_ns};
    const uint64_t limit = A840_FW_FRAME_BUDGET_NS + (uint64_t)INT64_MAX + 1;
    uint64_t total = 0;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        if (stages[i] > limit - total)
            return A840_FW_OVERFLOW;
        total += stages[i];
    }
    if (total <= A840_FW_FRAME_BUDGET_NS)
        *margin = (int64_t)(A840_FW_FRAME_BUDGET_NS - total);
    else
        *margin = -1 - (int64_t)(total - A840_FW_FRAME_BUDGET_NS - 1);
    return A840_FW_OK;
}

A840FwStatus a840_fw_recorder_init(A840FwRecorder *recorder, uint32_t scene_id,
                                  A840FwFrameRecord *frames, size_t frame_capacity,
                                  A840FwDrawRecord *draws, size_t draw_capacity)
{
    if (!recorder || !valid_scene(scene_id) ||
        (frame_capacity && !frames) || (draw_capacity && !draws))
        return A840_FW_INVALID_ARGUMENT;
    if (frame_capacity > UINT32_MAX || frame_capacity > SIZE_MAX / sizeof(*frames) ||
        draw_capacity > UINT32_MAX || draw_capacity > SIZE_MAX / sizeof(*draws))
        return A840_FW_OVERFLOW;
    *recorder = (A840FwRecorder){
        .frames = frames, .draws = draws, .frame_capacity = frame_capacity,
        .draw_capacity = draw_capacity, .scene_id = scene_id
    };
    return A840_FW_OK;
}

A840FwStatus a840_fw_record_frame(A840FwRecorder *recorder,
                                 const A840FwFrameRecord *frame,
                                 uint64_t gpu_start_ticks, uint64_t gpu_end_ticks)
{
    if (!recorder || !frame)
        return A840_FW_INVALID_ARGUMENT;
    if (!valid_frame(frame, recorder->scene_id))
        return A840_FW_BAD_RECORD;
    if (recorder->frame_count == recorder->frame_capacity)
        return A840_FW_CAPACITY;
    if (recorder->frame_count) {
        const A840FwFrameRecord *prev = &recorder->frames[recorder->frame_count - 1];
        if (frame->frame_seq <= prev->frame_seq || frame->t_vsync_prev_ns < prev->t_vsync_prev_ns)
            return A840_FW_BAD_RECORD;
    }

    A840FwFrameRecord captured = *frame;
    A840FwStatus status = a840_fw_interval_to_ns(gpu_start_ticks, gpu_end_ticks,
                                               &captured.gpu_exec_ns);
    if (status != A840_FW_OK)
        return status;
    status = frame_margin(&captured, &captured.scanout_margin_ns);
    if (status != A840_FW_OK)
        return status;
    recorder->frames[recorder->frame_count++] = captured;
    return A840_FW_OK;
}

A840FwStatus a840_fw_record_draw(A840FwRecorder *recorder, const A840FwDrawRecord *draw)
{
    if (!recorder || !draw)
        return A840_FW_INVALID_ARGUMENT;
    if (draw->gmem_mode > A840_FW_BLIT)
        return A840_FW_BAD_RECORD;
    if (recorder->draw_count == recorder->draw_capacity)
        return A840_FW_CAPACITY;

    A840FwDrawRecord captured = *draw;
    const A840FwStatus status = a840_fw_interval_to_ns(draw->start_ticks_19m2,
                                                      draw->end_ticks_19m2,
                                                      &captured.duration_ns);
    if (status != A840_FW_OK)
        return status;
    recorder->draws[recorder->draw_count++] = captured;
    return A840_FW_OK;
}

static void put_u32(uint8_t **cursor, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i)
        *(*cursor)++ = (uint8_t)(value >> (8 * i));
}

static void put_u64(uint8_t **cursor, uint64_t value)
{
    put_u32(cursor, (uint32_t)value);
    put_u32(cursor, (uint32_t)(value >> 32));
}

static uint32_t get_u32(const uint8_t **cursor)
{
    uint32_t value = 0;
    for (unsigned int i = 0; i < 4; ++i)
        value |= (uint32_t)*(*cursor)++ << (8 * i);
    return value;
}

static uint64_t get_u64(const uint8_t **cursor)
{
    const uint64_t low = get_u32(cursor);
    return low | ((uint64_t)get_u32(cursor) << 32);
}

static void put_frame(uint8_t **p, const A840FwFrameRecord *frame)
{
    put_u64(p, frame->frame_seq);
    put_u32(p, frame->scene_id);
    put_u32(p, frame->ring_id);
    put_u32(p, frame->preempted_count);
    put_u32(p, frame->wfi_count);
    put_u32(p, frame->cache_flush_count);
    put_u32(p, frame->heap_allocs);
    put_u64(p, frame->t_vsync_prev_ns);
    put_u64(p, frame->pre_create_ns);
    put_u64(p, frame->encode_ns);
    put_u64(p, frame->submit_ns);
    put_u64(p, frame->gpu_exec_ns);
    put_u64(p, frame->wait_sync_ns);
    put_u64(p, (uint64_t)frame->scanout_margin_ns);
    put_u64(p, frame->physical_frame_interval_ns);
}

static A840FwFrameRecord get_frame(const uint8_t *p)
{
    A840FwFrameRecord frame;
    frame.frame_seq = get_u64(&p);
    frame.scene_id = get_u32(&p);
    frame.ring_id = get_u32(&p);
    frame.preempted_count = get_u32(&p);
    frame.wfi_count = get_u32(&p);
    frame.cache_flush_count = get_u32(&p);
    frame.heap_allocs = get_u32(&p);
    frame.t_vsync_prev_ns = get_u64(&p);
    frame.pre_create_ns = get_u64(&p);
    frame.encode_ns = get_u64(&p);
    frame.submit_ns = get_u64(&p);
    frame.gpu_exec_ns = get_u64(&p);
    frame.wait_sync_ns = get_u64(&p);
    const uint64_t bits = get_u64(&p);
    frame.scanout_margin_ns = bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)(UINT64_MAX - bits);
    frame.physical_frame_interval_ns = get_u64(&p);
    return frame;
}

static void put_draw(uint8_t **p, const A840FwDrawRecord *draw)
{
    put_u64(p, draw->frame_seq);
    put_u32(p, draw->pass_id);
    put_u32(p, draw->draw_id);
    put_u64(p, draw->pipeline_hash);
    put_u32(p, draw->gmem_mode);
    put_u32(p, draw->tile_count);
    put_u32(p, draw->draw_verts_or_threads);
    put_u64(p, draw->start_ticks_19m2);
    put_u64(p, draw->end_ticks_19m2);
    put_u64(p, draw->duration_ns);
    put_u32(p, draw->sync_flags);
}

static A840FwDrawRecord get_draw(const uint8_t *p)
{
    A840FwDrawRecord draw;
    draw.frame_seq = get_u64(&p);
    draw.pass_id = get_u32(&p);
    draw.draw_id = get_u32(&p);
    draw.pipeline_hash = get_u64(&p);
    draw.gmem_mode = get_u32(&p);
    draw.tile_count = get_u32(&p);
    draw.draw_verts_or_threads = get_u32(&p);
    draw.start_ticks_19m2 = get_u64(&p);
    draw.end_ticks_19m2 = get_u64(&p);
    draw.duration_ns = get_u64(&p);
    draw.sync_flags = get_u32(&p);
    return draw;
}

static uint32_t payload_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static size_t wire_size(size_t frames, size_t draws)
{
    if (frames > (SIZE_MAX - A840_FW_HEADER_BYTES) / A840_FW_FRAME_BYTES)
        return 0;
    const size_t prefix = A840_FW_HEADER_BYTES + frames * A840_FW_FRAME_BYTES;
    if (draws > (SIZE_MAX - prefix) / A840_FW_DRAW_BYTES)
        return 0;
    return prefix + draws * A840_FW_DRAW_BYTES;
}

static int has_frame(const A840FwFrameRecord *native, const uint8_t *wire,
                     size_t count, uint64_t frame_seq)
{
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const uint8_t *p = wire ? wire + mid * A840_FW_FRAME_BYTES : NULL;
        const uint64_t seq = p ? get_u64(&p) : native[mid].frame_seq;
        if (seq == frame_seq)
            return 1;
        if (seq < frame_seq)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

size_t a840_fw_encoded_size(const A840FwRecorder *recorder)
{
    return recorder ? wire_size(recorder->frame_count, recorder->draw_count) : 0;
}

A840FwStatus a840_fw_encode(const A840FwRecorder *recorder, void *bytes, size_t capacity,
                            size_t *written)
{
    if (!recorder || !bytes || !written)
        return A840_FW_INVALID_ARGUMENT;
    const size_t size = a840_fw_encoded_size(recorder);
    if (!size)
        return A840_FW_OVERFLOW;
    if (capacity < size)
        return A840_FW_CAPACITY;
    for (size_t i = 0; i < recorder->draw_count; ++i) {
        if (!has_frame(recorder->frames, NULL, recorder->frame_count, recorder->draws[i].frame_seq))
            return A840_FW_BAD_RECORD;
    }

    uint8_t *const payload = (uint8_t *)bytes + A840_FW_HEADER_BYTES;
    uint8_t *p = payload;
    for (size_t i = 0; i < recorder->frame_count; ++i)
        put_frame(&p, &recorder->frames[i]);
    for (size_t i = 0; i < recorder->draw_count; ++i)
        put_draw(&p, &recorder->draws[i]);

    memcpy(bytes, A840_FW_MAGIC, 8);
    p = (uint8_t *)bytes + 8;
    put_u32(&p, A840_FW_VERSION);
    put_u32(&p, A840_FW_ALWAYS_ON_HZ);
    put_u32(&p, A840_FW_TARGET_REFRESH_HZ);
    put_u64(&p, A840_FW_FRAME_BUDGET_NS);
    put_u32(&p, recorder->scene_id);
    put_u32(&p, (uint32_t)recorder->frame_count);
    put_u32(&p, (uint32_t)recorder->draw_count);
    put_u32(&p, payload_crc32(payload, size - A840_FW_HEADER_BYTES));
    *written = size;
    return A840_FW_OK;
}

A840FwStatus a840_fw_decode(const void *bytes, size_t length, A840FwCapture *capture)
{
    if (!bytes || !capture)
        return A840_FW_INVALID_ARGUMENT;
    if (length < A840_FW_HEADER_BYTES)
        return A840_FW_BAD_LENGTH;
    if (memcmp(bytes, A840_FW_MAGIC, 8) != 0)
        return A840_FW_BAD_MAGIC;

    A840FwCapture decoded;
    A840FwHeader *const header = &decoded.header;
    memcpy(header->magic, bytes, 8);
    const uint8_t *p = (const uint8_t *)bytes + 8;
    header->version = get_u32(&p);
    header->always_on_hz = get_u32(&p);
    header->target_refresh_hz = get_u32(&p);
    header->frame_budget_ns = get_u64(&p);
    header->scene_id = get_u32(&p);
    header->frame_count = get_u32(&p);
    header->draw_count = get_u32(&p);
    header->payload_crc32 = get_u32(&p);
    if (header->version != A840_FW_VERSION || header->always_on_hz != A840_FW_ALWAYS_ON_HZ ||
        header->target_refresh_hz != A840_FW_TARGET_REFRESH_HZ ||
        header->frame_budget_ns != A840_FW_FRAME_BUDGET_NS || !valid_scene(header->scene_id))
        return A840_FW_BAD_HEADER;
    if (wire_size(header->frame_count, header->draw_count) != length)
        return A840_FW_BAD_LENGTH;
    if (payload_crc32(p, length - A840_FW_HEADER_BYTES) != header->payload_crc32)
        return A840_FW_BAD_CRC;

    decoded.payload = p;
    uint64_t prev_seq = 0, prev_vsync = 0;
    for (size_t i = 0; i < header->frame_count; ++i, p += A840_FW_FRAME_BYTES) {
        const A840FwFrameRecord frame = get_frame(p);
        if (!valid_frame(&frame, header->scene_id) ||
            (i && (frame.frame_seq <= prev_seq || frame.t_vsync_prev_ns < prev_vsync)))
            return A840_FW_BAD_RECORD;
        int64_t margin;
        A840FwStatus status = frame_margin(&frame, &margin);
        if (status != A840_FW_OK)
            return status;
        if (frame.scanout_margin_ns != margin)
            return A840_FW_BAD_RECORD;
        prev_seq = frame.frame_seq;
        prev_vsync = frame.t_vsync_prev_ns;
    }
    for (size_t i = 0; i < header->draw_count; ++i, p += A840_FW_DRAW_BYTES) {
        const A840FwDrawRecord draw = get_draw(p);
        if (draw.gmem_mode > A840_FW_BLIT ||
            !has_frame(NULL, decoded.payload, header->frame_count, draw.frame_seq))
            return A840_FW_BAD_RECORD;
        uint64_t duration;
        A840FwStatus status = a840_fw_interval_to_ns(draw.start_ticks_19m2, draw.end_ticks_19m2,
                                                   &duration);
        if (status != A840_FW_OK)
            return status;
        if (draw.duration_ns != duration)
            return A840_FW_BAD_RECORD;
    }
    *capture = decoded;
    return A840_FW_OK;
}

A840FwStatus a840_fw_frame_at(const A840FwCapture *capture, size_t index,
                             A840FwFrameRecord *frame)
{
    if (!capture || !frame || index >= capture->header.frame_count)
        return A840_FW_INVALID_ARGUMENT;
    *frame = get_frame(capture->payload + index * A840_FW_FRAME_BYTES);
    return A840_FW_OK;
}

A840FwStatus a840_fw_draw_at(const A840FwCapture *capture, size_t index, A840FwDrawRecord *draw)
{
    if (!capture || !draw || index >= capture->header.draw_count)
        return A840_FW_INVALID_ARGUMENT;
    const size_t frames = (size_t)capture->header.frame_count * A840_FW_FRAME_BYTES;
    *draw = get_draw(capture->payload + frames + index * A840_FW_DRAW_BYTES);
    return A840_FW_OK;
}
