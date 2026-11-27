/* SPDX-License-Identifier: MIT */
#ifndef A840_FRAME_WEIGHTS_H
#define A840_FRAME_WEIGHTS_H

#include <stddef.h>
#include <stdint.h>

#define A840_FW_MAGIC "A840FW01"
#define A840_FW_FRAME_BUDGET_NS UINT64_C(8333333)

enum {
    A840_FW_VERSION = 1,
    A840_FW_ALWAYS_ON_HZ = 19200000,
    A840_FW_TARGET_REFRESH_HZ = 120,
    A840_FW_HEADER_BYTES = 44,
    A840_FW_FRAME_BYTES = 96,
    A840_FW_DRAW_BYTES = 64
};

typedef enum A840FwScene {
    A840_FW_SCENE_CUSTOM = 0,
    A840_FW_SCENE_APP_OPEN_CLOSE = 1,
    A840_FW_SCENE_SETTINGS_SCROLL = 2,
    A840_FW_SCENE_APP_LIBRARY_SCROLL = 3,
    A840_FW_SCENE_DENSE_LIQUID_GLASS_SWIPE = 4
} A840FwScene;

typedef enum A840FwGmemMode {
    A840_FW_GMEM = 0,
    A840_FW_SYSMEM = 1,
    A840_FW_COMPUTE = 2,
    A840_FW_BLIT = 3
} A840FwGmemMode;

typedef enum A840FwStatus {
    A840_FW_OK = 0,
    A840_FW_INVALID_ARGUMENT,
    A840_FW_CAPACITY,
    A840_FW_OVERFLOW,
    A840_FW_BAD_MAGIC,
    A840_FW_BAD_HEADER,
    A840_FW_BAD_LENGTH,
    A840_FW_BAD_CRC,
    A840_FW_BAD_RECORD,
    A840_FW_TICK_REVERSED
} A840FwStatus;

/*
 * Wire order is header, frame array, draw array. Fields are consecutive in
 * the order below, with no padding. Integers use little-endian encoding
 * (signed margin uses two's complement). IEEE CRC32 covers both arrays.
 * CUSTOM in the header permits any valid per-frame scene, including CUSTOM.
 * A named header scene requires every frame to carry that same scene.
 * These structs are host values. Use the codec for wire data.
 */
typedef struct A840FwHeader {
    uint8_t magic[8];
    uint32_t version;
    uint32_t always_on_hz;
    uint32_t target_refresh_hz;
    uint64_t frame_budget_ns;
    uint32_t scene_id;
    uint32_t frame_count;
    uint32_t draw_count;
    uint32_t payload_crc32;
} A840FwHeader;

typedef struct A840FwFrameRecord {
    uint64_t frame_seq;
    uint32_t scene_id;
    uint32_t ring_id;
    uint32_t preempted_count;
    uint32_t wfi_count;
    uint32_t cache_flush_count;
    uint32_t heap_allocs;
    uint64_t t_vsync_prev_ns;
    uint64_t pre_create_ns;
    uint64_t encode_ns;
    uint64_t submit_ns;
    uint64_t gpu_exec_ns;
    uint64_t wait_sync_ns;
    int64_t scanout_margin_ns; /* 120 Hz budget minus the five timed stages */
    uint64_t physical_frame_interval_ns; /* zero when unavailable */
} A840FwFrameRecord;

typedef struct A840FwDrawRecord {
    uint64_t frame_seq;
    uint32_t pass_id;
    uint32_t draw_id;
    uint64_t pipeline_hash;
    uint32_t gmem_mode;
    uint32_t tile_count;
    uint32_t draw_verts_or_threads;
    uint64_t start_ticks_19m2;
    uint64_t end_ticks_19m2;
    uint64_t duration_ns;
    uint32_t sync_flags;
} A840FwDrawRecord;

typedef struct A840FwRecorder {
    A840FwFrameRecord *frames;
    A840FwDrawRecord *draws;
    size_t frame_capacity;
    size_t draw_capacity;
    size_t frame_count;
    size_t draw_count;
    uint32_t scene_id;
} A840FwRecorder;

typedef struct A840FwCapture {
    A840FwHeader header;
    const uint8_t *payload;
} A840FwCapture;

#ifdef __cplusplus
extern "C" {
#endif

/* Floor(ticks * 10^9 / hz), saturating to UINT64_MAX on overflow or hz == 0. */
uint64_t a840_fw_ticks_to_ns(uint64_t ticks, uint32_t hz);

/* Convert a difference of two completed Always-On samples. On error *ns is intact. */
A840FwStatus a840_fw_interval_to_ns(uint64_t start_ticks, uint64_t end_ticks,
                                  uint64_t *ns);

/*
 * Single-writer recorder with caller-owned storage. Arrays may be NULL at
 * zero capacity. Storage, metadata, and outputs must be disjoint. Keep
 * recorded entries unchanged. Reinitializing the recorder starts a new capture.
 */
A840FwStatus a840_fw_recorder_init(A840FwRecorder *recorder, uint32_t scene_id,
                                  A840FwFrameRecord *frames, size_t frame_capacity,
                                  A840FwDrawRecord *draws, size_t draw_capacity);

/*
 * Append finalized stages and completed GPU-clock samples. The recorder
 * computes gpu_exec_ns and scanout_margin_ns from the inputs. Frame sequence
 * must increase strictly and vsync time must be nondecreasing in the capture.
 */
A840FwStatus a840_fw_record_frame(A840FwRecorder *recorder,
                                 const A840FwFrameRecord *frame,
                                 uint64_t gpu_start_ticks, uint64_t gpu_end_ticks);

/*
 * Append completed Draw/Pass metadata. The recorder computes duration_ns.
 * Draws may be appended before their frame is finalized. Export requires
 * every draw to reference a recorded frame.
 */
A840FwStatus a840_fw_record_draw(A840FwRecorder *recorder,
                                const A840FwDrawRecord *draw);

/* Return the required output bytes, or zero if the size cannot be represented. */
size_t a840_fw_encoded_size(const A840FwRecorder *recorder);
A840FwStatus a840_fw_encode(const A840FwRecorder *recorder, void *bytes, size_t capacity,
                            size_t *written);

/*
 * Validate the entire capture before publishing a read-only view. Keep the
 * caller's wire bytes alive and immutable while using the view. The indexed
 * readers copy one decoded record from a successfully decoded view.
 */
A840FwStatus a840_fw_decode(const void *bytes, size_t length, A840FwCapture *capture);
A840FwStatus a840_fw_frame_at(const A840FwCapture *capture, size_t index,
                             A840FwFrameRecord *frame);
A840FwStatus a840_fw_draw_at(const A840FwCapture *capture, size_t index,
                            A840FwDrawRecord *draw);

#ifdef __cplusplus
}
#endif

#endif /* A840_FRAME_WEIGHTS_H */
