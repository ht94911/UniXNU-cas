/* SPDX-License-Identifier: MIT */
#include "a840_frame_weights.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
unsigned int cases;
unsigned int checks;
const char *case_name;
thread_local bool track_allocations;
thread_local std::size_t allocations;
}

/* ASan calls this for C malloc/calloc/realloc and C++ allocation. */
extern "C" void __sanitizer_malloc_hook(const volatile void *, std::size_t)
{
    if (track_allocations)
        ++allocations;
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define A840_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define A840_TEST_ASAN 1
#endif

namespace {
void check(bool passed, int line, const char *expression)
{
    ++checks;
    if (!passed) {
        std::fprintf(stderr, "FAIL %s at line %d: %s\n", case_name, line, expression);
        std::exit(1);
    }
}
#define CHECK(expression) check((expression), __LINE__, #expression)

template <class Function>
void run(const char *name, Function test)
{
    case_name = name;
    ++cases;
    test();
}

constexpr uint64_t start_tick = UINT64_C(0xfedcba9876543210);
constexpr uint64_t hash = UINT64_C(0xfedcba0102030405);
constexpr std::size_t frame_offset = A840_FW_HEADER_BYTES;
constexpr std::size_t draw_offset = frame_offset + A840_FW_FRAME_BYTES;
constexpr std::size_t one_frame_wire_bytes = draw_offset + A840_FW_DRAW_BYTES;

A840FwFrameRecord make_frame(uint64_t seq = 1, uint32_t scene = A840_FW_SCENE_APP_OPEN_CLOSE)
{
    A840FwFrameRecord frame{};
    frame.frame_seq = seq;
    frame.scene_id = scene;
    frame.ring_id = 3;
    frame.preempted_count = 17;
    frame.wfi_count = 21;
    frame.cache_flush_count = 37;
    frame.heap_allocs = 41;
    frame.t_vsync_prev_ns = UINT64_C(0x123456000000) + seq * 8333333;
    frame.pre_create_ns = 7000000;
    frame.encode_ns = 1000000;
    frame.submit_ns = 200000;
    frame.gpu_exec_ns = UINT64_MAX;
    frame.wait_sync_ns = 500000;
    frame.scanout_margin_ns = INT64_MAX;
    frame.physical_frame_interval_ns = 16666667;
    return frame;
}

A840FwDrawRecord make_draw(uint64_t seq = 1, uint32_t mode = A840_FW_GMEM)
{
    A840FwDrawRecord draw{};
    draw.frame_seq = seq;
    draw.pass_id = 0x11223344;
    draw.draw_id = 12;
    draw.pipeline_hash = hash;
    draw.gmem_mode = mode;
    draw.tile_count = 126;
    draw.draw_verts_or_threads = 1024;
    draw.start_ticks_19m2 = start_tick;
    draw.end_ticks_19m2 = start_tick + 192;
    draw.duration_ns = UINT64_MAX;
    draw.sync_flags = 0x80000001;
    return draw;
}

template <std::size_t Frames, std::size_t Draws>
struct Fixture {
    std::array<A840FwFrameRecord, Frames> frames{};
    std::array<A840FwDrawRecord, Draws> draws{};
    std::array<uint8_t, A840_FW_HEADER_BYTES + Frames * A840_FW_FRAME_BYTES +
                            Draws * A840_FW_DRAW_BYTES> bytes{};
    A840FwRecorder recorder{};
    std::size_t used{};

    explicit Fixture(uint32_t scene = A840_FW_SCENE_CUSTOM)
    {
        CHECK(a840_fw_recorder_init(&recorder, scene, frames.data(), frames.size(),
                                    draws.data(), draws.size()) == A840_FW_OK);
    }

    void add_frame(const A840FwFrameRecord &frame, uint64_t gpu_ticks = 19200)
    {
        CHECK(a840_fw_record_frame(&recorder, &frame, start_tick,
                                   start_tick + gpu_ticks) == A840_FW_OK);
    }

    void add_draw(const A840FwDrawRecord &draw)
    {
        CHECK(a840_fw_record_draw(&recorder, &draw) == A840_FW_OK);
    }

    void encode()
    {
        CHECK(a840_fw_encode(&recorder, bytes.data(), bytes.size(), &used) == A840_FW_OK);
        CHECK(used == a840_fw_encoded_size(&recorder));
    }

    A840FwCapture decode() const
    {
        A840FwCapture capture{};
        CHECK(a840_fw_decode(bytes.data(), used, &capture) == A840_FW_OK);
        return capture;
    }
};

void store_le(uint8_t *p, uint64_t value, unsigned int size)
{
    for (unsigned int i = 0; i < size; ++i) {
        p[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

void repair_crc(uint8_t *data, std::size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (std::size_t i = frame_offset; i < size; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = crc & 1 ? (crc >> 1) ^ UINT32_C(0xedb88320) : crc >> 1;
    }
    store_le(data + 40, ~crc, 4);
}

void test_conversion()
{
    const struct {
        const char *name;
        uint64_t ticks;
        uint32_t hz;
        uint64_t expected;
    } conversions[] = {
        {"zero ticks", 0, 19200000, 0},
        {"one tick floors to 52 ns", 1, 19200000, 52},
        {"remainder truncation", 3, 19200000, 156},
        {"192 ticks", 192, 19200000, 10000},
        {"one 120 Hz period", 160000, 19200000, 8333333},
        {"whole second", 19200000, 19200000, 1000000000},
        {"seconds with a remainder", 19200001, 19200000, 1000000052},
        {"above exact double domain", UINT64_C(36028797018963968), 19200000,
         UINT64_C(1876499844737706666)},
        {"last representable Adreno tick count", UINT64_C(354177486215223391), 19200000,
         UINT64_C(18446744073709551614)},
        {"Adreno conversion overflow", UINT64_C(354177486215223392), 19200000, UINT64_MAX},
        {"full uint64 ticks overflow", UINT64_MAX, 19200000, UINT64_MAX},
        {"uint32 max frequency", UINT64_MAX, UINT32_MAX, UINT64_C(4294967297000000000)},
        {"exact uint64 max nanoseconds", UINT64_MAX, 1000000000, UINT64_MAX},
        {"invalid zero Hz", 1, 0, UINT64_MAX},
    };
    for (const auto &test : conversions) {
        run(test.name, [&] {
            CHECK(a840_fw_ticks_to_ns(test.ticks, test.hz) == test.expected);
        });
    }
    run("high absolute ticks, one-tick duration", [] {
        uint64_t ns = 0;
        CHECK(a840_fw_interval_to_ns(UINT64_MAX - 1, UINT64_MAX, &ns) == A840_FW_OK);
        CHECK(ns == 52);
    });
    run("equal endpoints", [] {
        uint64_t ns = 1;
        CHECK(a840_fw_interval_to_ns(UINT64_MAX, UINT64_MAX, &ns) == A840_FW_OK);
        CHECK(ns == 0);
    });
    run("reversed interval leaves output", [] {
        uint64_t ns = 9;
        CHECK(a840_fw_interval_to_ns(2, 1, &ns) == A840_FW_TICK_REVERSED);
        CHECK(ns == 9);
    });
    run("oversized interval leaves output", [] {
        uint64_t ns = 9;
        CHECK(a840_fw_interval_to_ns(0, UINT64_MAX, &ns) == A840_FW_OVERFLOW);
        CHECK(ns == 9);
    });
    run("interval requires output", [] {
        CHECK(a840_fw_interval_to_ns(0, 1, nullptr) == A840_FW_INVALID_ARGUMENT);
    });
}

void test_recorder()
{
    run("invalid initialization", [] {
        A840FwRecorder recorder{};
        CHECK(a840_fw_recorder_init(nullptr, 0, nullptr, 0, nullptr, 0) == A840_FW_INVALID_ARGUMENT);
        CHECK(a840_fw_recorder_init(&recorder, 5, nullptr, 0, nullptr, 0) == A840_FW_INVALID_ARGUMENT);
        CHECK(a840_fw_recorder_init(&recorder, 0, nullptr, 1, nullptr, 0) == A840_FW_INVALID_ARGUMENT);
        CHECK(a840_fw_recorder_init(&recorder, 0, nullptr, 0, nullptr, 1) == A840_FW_INVALID_ARGUMENT);
    });
    run("capacity exceeds wire count", [] {
        A840FwRecorder recorder{};
        A840FwFrameRecord frame{};
        A840FwDrawRecord draw{};
        const std::size_t too_big = UINT64_C(1) << 32;
        CHECK(a840_fw_recorder_init(&recorder, 0, &frame, too_big, &draw, 0) == A840_FW_OVERFLOW);
        CHECK(a840_fw_recorder_init(&recorder, 0, &frame, 0, &draw, too_big) == A840_FW_OVERFLOW);
    });
    run("empty capture and empty CRC", [] {
        Fixture<0, 0> f;
        f.encode();
        CHECK(f.used == A840_FW_HEADER_BYTES);
        const auto capture = f.decode();
        CHECK(capture.header.frame_count == 0);
        CHECK(capture.header.draw_count == 0);
        CHECK(capture.header.payload_crc32 == 0);
    });
    run("frame buffer full", [] {
        Fixture<1, 0> f;
        f.add_frame(make_frame());
        const auto frame = make_frame(2);
        CHECK(a840_fw_record_frame(&f.recorder, &frame, 0, 0) == A840_FW_CAPACITY);
        CHECK(f.recorder.frame_count == 1);
        CHECK(f.frames[0].frame_seq == 1);
    });
    run("draw buffer full", [] {
        Fixture<0, 1> f;
        auto draw = make_draw();
        f.add_draw(draw);
        ++draw.draw_id;
        CHECK(a840_fw_record_draw(&f.recorder, &draw) == A840_FW_CAPACITY);
        CHECK(f.recorder.draw_count == 1 && f.draws[0].draw_id == 12);
    });
    run("frame rejects backwards GPU ticks", [] {
        Fixture<1, 0> f;
        const auto frame = make_frame();
        CHECK(a840_fw_record_frame(&f.recorder, &frame, 2, 1) == A840_FW_TICK_REVERSED);
        CHECK(f.recorder.frame_count == 0);
    });
    run("draw rejects backwards GPU ticks", [] {
        Fixture<0, 1> f;
        auto draw = make_draw();
        draw.end_ticks_19m2 = draw.start_ticks_19m2 - 1;
        CHECK(a840_fw_record_draw(&f.recorder, &draw) == A840_FW_TICK_REVERSED);
        CHECK(f.recorder.draw_count == 0);
    });
    run("draw rejects interval overflow", [] {
        Fixture<0, 1> f;
        auto draw = make_draw();
        draw.start_ticks_19m2 = 0;
        draw.end_ticks_19m2 = UINT64_MAX;
        CHECK(a840_fw_record_draw(&f.recorder, &draw) == A840_FW_OVERFLOW);
        CHECK(f.recorder.draw_count == 0);
    });
    run("invalid frame scene, ring, and draw mode", [] {
        Fixture<1, 1> f(A840_FW_SCENE_APP_OPEN_CLOSE);
        auto frame = make_frame(1, A840_FW_SCENE_SETTINGS_SCROLL);
        CHECK(a840_fw_record_frame(&f.recorder, &frame, 0, 0) == A840_FW_BAD_RECORD);
        frame.scene_id = A840_FW_SCENE_APP_OPEN_CLOSE;
        frame.ring_id = 4;
        CHECK(a840_fw_record_frame(&f.recorder, &frame, 0, 0) == A840_FW_BAD_RECORD);
        const auto draw = make_draw(1, 4);
        CHECK(a840_fw_record_draw(&f.recorder, &draw) == A840_FW_BAD_RECORD);
    });
    run("frame ordering and monotonic vsync", [] {
        Fixture<2, 0> f;
        auto frame = make_frame(2);
        f.add_frame(frame);
        CHECK(a840_fw_record_frame(&f.recorder, &frame, 0, 0) == A840_FW_BAD_RECORD);
        ++frame.frame_seq;
        --frame.t_vsync_prev_ns;
        CHECK(a840_fw_record_frame(&f.recorder, &frame, 0, 0) == A840_FW_BAD_RECORD);
        ++frame.t_vsync_prev_ns;
        f.add_frame(frame);
    });
    run("unsigned stage addition and signed margin overflow", [] {
        Fixture<1, 0> f;
        auto frame = make_frame();
        frame.pre_create_ns = UINT64_MAX;
        CHECK(a840_fw_record_frame(&f.recorder, &frame, 0, 1) == A840_FW_OVERFLOW);
        CHECK(f.recorder.frame_count == 0);
        frame = {};
        frame.pre_create_ns = A840_FW_FRAME_BUDGET_NS + (UINT64_C(1) << 63) + 1;
        CHECK(a840_fw_record_frame(&f.recorder, &frame, 0, 0) == A840_FW_OVERFLOW);
    });
    run("INT64_MIN margin round trip", [] {
        Fixture<1, 0> f;
        A840FwFrameRecord frame{};
        frame.pre_create_ns = A840_FW_FRAME_BUDGET_NS + (UINT64_C(1) << 63);
        f.add_frame(frame, 0);
        f.encode();
        const auto capture = f.decode();
        CHECK(a840_fw_frame_at(&capture, 0, &frame) == A840_FW_OK);
        CHECK(frame.scanout_margin_ns == INT64_MIN);
    });
    run("short encode leaves buffer and size intact", [] {
        Fixture<1, 1> f;
        f.add_frame(make_frame());
        f.add_draw(make_draw());
        f.bytes.fill(0xab);
        f.used = 12;
        CHECK(a840_fw_encode(&f.recorder, f.bytes.data(), f.bytes.size() - 1,
                             &f.used) == A840_FW_CAPACITY);
        CHECK(f.used == 12);
        CHECK(std::all_of(f.bytes.begin(), f.bytes.end(), [](uint8_t b) { return b == 0xab; }));
    });
    run("draw may precede frame, unresolved reference fails export", [] {
        Fixture<1, 1> f;
        f.add_draw(make_draw());
        CHECK(a840_fw_encode(&f.recorder, f.bytes.data(), f.bytes.size(), &f.used) == A840_FW_BAD_RECORD);
        f.add_frame(make_frame());
        f.encode();
        CHECK(f.decode().header.draw_count == 1);
    });
    run("capture, encode, and decode allocate nothing", [] {
#ifdef A840_TEST_ASAN
        void *(*volatile allocate)(std::size_t) = std::malloc;
        track_allocations = true;
        void *probe = allocate(24);
        track_allocations = false;
        std::free(probe);
        CHECK(allocations == 1);  // The hook actually observes heap allocation.
#endif
        allocations = 0;
        track_allocations = true;
        Fixture<1, 1> f;
        f.add_frame(make_frame());
        f.add_draw(make_draw());
        f.encode();
        const auto capture = f.decode();
        A840FwFrameRecord frame{};
        A840FwDrawRecord draw{};
        CHECK(a840_fw_frame_at(&capture, 0, &frame) == A840_FW_OK);
        CHECK(a840_fw_draw_at(&capture, 0, &draw) == A840_FW_OK);
        track_allocations = false;
        CHECK(allocations == 0);
    });
    run("four scenes in one stream and out-of-order draw arrival", [] {
        Fixture<4, 4> f;
        for (uint32_t scene = 1; scene <= 4; ++scene) {
            f.add_frame(make_frame(scene, scene));
            f.add_draw(make_draw(5 - scene, scene - 1));
        }
        f.encode();
        const auto capture = f.decode();
        CHECK(capture.header.scene_id == A840_FW_SCENE_CUSTOM);
        CHECK(capture.header.frame_count == 4 && capture.header.draw_count == 4);
        for (uint32_t index = 0; index < 4; ++index) {
            A840FwFrameRecord frame{};
            A840FwDrawRecord draw{};
            CHECK(a840_fw_frame_at(&capture, index, &frame) == A840_FW_OK);
            CHECK(a840_fw_draw_at(&capture, index, &draw) == A840_FW_OK);
            CHECK(frame.scene_id == index + 1);
            CHECK(draw.frame_seq == 4 - index && draw.gmem_mode == index);
        }
    });
}

void test_wire()
{
    const char *scenes[] = {"CUSTOM", "APP_OPEN_CLOSE", "SETTINGS_SCROLL",
                           "APP_LIBRARY_SCROLL", "DENSE_LIQUID_GLASS_SWIPE"};
    for (uint32_t scene = 0; scene <= 4; ++scene) {
        run(scenes[scene], [scene] {
            Fixture<1, 1> f(scene);
            f.add_frame(make_frame(1, scene));
            f.add_draw(make_draw());
            f.encode();
            const auto capture = f.decode();
            const auto &head = capture.header;
            CHECK(std::memcmp(head.magic, "A840FW01", 8) == 0);
            CHECK(head.version == 1 && head.always_on_hz == 19200000 && head.target_refresh_hz == 120);
            CHECK(head.frame_budget_ns == 8333333 && head.scene_id == scene);
            CHECK(head.frame_count == 1 && head.draw_count == 1);
            CHECK(f.used == 204);
            const auto expected_frame = make_frame(1, scene);
            A840FwFrameRecord frame{};
            CHECK(a840_fw_frame_at(&capture, 0, &frame) == A840_FW_OK);
            CHECK(frame.frame_seq == 1 && frame.scene_id == scene && frame.ring_id == 3);
            CHECK(frame.preempted_count == 17 && frame.wfi_count == 21);
            CHECK(frame.cache_flush_count == 37 && frame.heap_allocs == 41);
            CHECK(frame.t_vsync_prev_ns == expected_frame.t_vsync_prev_ns);
            CHECK(frame.pre_create_ns == 7000000 && frame.encode_ns == 1000000);
            CHECK(frame.submit_ns == 200000 && frame.wait_sync_ns == 500000);
            CHECK(frame.gpu_exec_ns == 1000000 && frame.scanout_margin_ns == -1366667);
            CHECK(frame.physical_frame_interval_ns == 16666667);
            A840FwDrawRecord draw{};
            CHECK(a840_fw_draw_at(&capture, 0, &draw) == A840_FW_OK);
            CHECK(draw.frame_seq == 1 && draw.pass_id == 0x11223344 && draw.draw_id == 12);
            CHECK(draw.pipeline_hash == hash && draw.gmem_mode == A840_FW_GMEM);
            CHECK(draw.tile_count == 126 && draw.draw_verts_or_threads == 1024);
            CHECK(draw.start_ticks_19m2 == start_tick && draw.end_ticks_19m2 == start_tick + 192);
            CHECK(draw.duration_ns == 10000 && draw.sync_flags == 0x80000001);
            CHECK(a840_fw_frame_at(&capture, 1, &frame) == A840_FW_INVALID_ARGUMENT);
            CHECK(a840_fw_draw_at(&capture, 1, &draw) == A840_FW_INVALID_ARGUMENT);
        });
    }
    run("unaligned little-endian wire", [] {
        Fixture<1, 1> f;
        f.add_frame(make_frame());
        f.add_draw(make_draw());
        std::array<uint8_t, one_frame_wire_bytes + 1> bytes{};
        uint8_t *p = bytes.data() + 1;
        CHECK(a840_fw_encode(&f.recorder, p, bytes.size() - 1, &f.used) == A840_FW_OK);
        CHECK(p[8] == 1 && p[9] == 0);
        CHECK(p[draw_offset + 8] == 0x44 && p[draw_offset + 11] == 0x11);
        CHECK(p[draw_offset + 36] == 0x10 && p[draw_offset + 43] == 0xfe);
        A840FwCapture capture{};
        CHECK(a840_fw_decode(p, f.used, &capture) == A840_FW_OK);
    });
    run("all truncated prefixes and trailing byte", [] {
        Fixture<1, 1> f;
        f.add_frame(make_frame());
        f.add_draw(make_draw());
        f.encode();
        A840FwCapture capture{};
        for (std::size_t length = 0; length < f.used; ++length)
            CHECK(a840_fw_decode(f.bytes.data(), length, &capture) == A840_FW_BAD_LENGTH);
        std::array<uint8_t, one_frame_wire_bytes + 1> bytes{};
        std::copy(f.bytes.begin(), f.bytes.end(), bytes.begin());
        CHECK(a840_fw_decode(bytes.data(), bytes.size(), &capture) == A840_FW_BAD_LENGTH);
    });

    const struct {
        const char *name;
        std::size_t offset;
        uint64_t value;
        unsigned int size;
        bool refresh_crc;
        A840FwStatus error;
    } damaged[] = {
        {"magic damage", 0, 'F', 1, false, A840_FW_BAD_MAGIC},
        {"unexpected version", 8, 2, 4, false, A840_FW_BAD_HEADER},
        {"unexpected frequency", 12, 1, 4, false, A840_FW_BAD_HEADER},
        {"unexpected refresh", 16, 60, 4, false, A840_FW_BAD_HEADER},
        {"unexpected budget", 20, 0, 8, false, A840_FW_BAD_HEADER},
        {"header scene invalid", 28, 5, 4, false, A840_FW_BAD_HEADER},
        {"header and frame scenes disagree", 28, 2, 4, false, A840_FW_BAD_RECORD},
        {"frame count/length mismatch", 32, UINT32_MAX, 4, false, A840_FW_BAD_LENGTH},
        {"draw count/length mismatch", 36, 0, 4, false, A840_FW_BAD_LENGTH},
        {"payload CRC mismatch", frame_offset + 16, 99, 4, false, A840_FW_BAD_CRC},
        {"frame scene invalid", frame_offset + 8, 5, 4, true, A840_FW_BAD_RECORD},
        {"ring invalid", frame_offset + 12, 4, 4, true, A840_FW_BAD_RECORD},
        {"frame algebra broken", frame_offset + 80, 0, 8, true, A840_FW_BAD_RECORD},
        {"orphan draw", draw_offset, 100, 8, true, A840_FW_BAD_RECORD},
        {"invalid mode", draw_offset + 24, 4, 4, true, A840_FW_BAD_RECORD},
        {"backwards wire tick", draw_offset + 44, 0, 8, true, A840_FW_TICK_REVERSED},
        {"forged duration", draw_offset + 52, 9999, 8, true, A840_FW_BAD_RECORD},
    };
    for (const auto &test : damaged) {
        run(test.name, [&] {
            Fixture<1, 1> f;
            f.add_frame(make_frame());
            f.add_draw(make_draw());
            f.encode();
            store_le(f.bytes.data() + test.offset, test.value, test.size);
            if (test.refresh_crc)
                repair_crc(f.bytes.data(), f.used);
            A840FwCapture capture{};
            capture.header.version = 99;
            CHECK(a840_fw_decode(f.bytes.data(), f.used, &capture) == test.error);
            CHECK(capture.payload == nullptr && capture.header.version == 99);
        });
    }
}

void dump_sample(const char *path)
{
    Fixture<12, 24> sample;
    const uint64_t pre[] = {14000000, 600000, 500000, 110000000};
    const uint64_t gpu[] = {19200, 250000, 19200, 24000};
    const uint64_t intervals[] = {16666667, 20000000, 30000000, 120000000};
    for (uint32_t scene = 1; scene <= 4; ++scene) {
        for (uint32_t index = 0; index < 3; ++index) {
            const uint64_t seq = (scene - 1) * 3 + index + 1;
            auto frame = make_frame(seq, scene);
            frame.ring_id = scene - 1;
            frame.pre_create_ns = index == 1 ? pre[scene - 1] : 300000;
            frame.physical_frame_interval_ns = index == 0 ? 8333333 :
                                               index == 1 ? intervals[scene - 1] : 0;
            frame.wfi_count = scene + index;
            frame.heap_allocs = 0;
            sample.add_frame(frame, index == 1 ? gpu[scene - 1] : 38400);
            auto draw = make_draw(seq, scene - 1);
            draw.pass_id = scene;
            draw.draw_id = 0;
            draw.end_ticks_19m2 = draw.start_ticks_19m2 + scene * (index + 1) * 192;
            sample.add_draw(draw);
            draw.draw_id = 1;
            draw.pipeline_hash = hash - 1;
            draw.gmem_mode = A840_FW_SYSMEM;
            draw.end_ticks_19m2 = draw.start_ticks_19m2 + 192;
            sample.add_draw(draw);
        }
    }
    sample.encode();
    CHECK(sample.decode().header.draw_count == 24);

    namespace fs = std::filesystem;
    const fs::path cache = "/home/hua/nezha-work/ios27/adreno840-native/.cache/w09-l4";
    const fs::path absolute = fs::absolute(path);
    const auto output = fs::canonical(absolute.parent_path()) / absolute.filename();
    const auto relative = output.lexically_relative(cache);
    CHECK(fs::canonical(cache) == cache && !relative.empty() && *relative.begin() != "..");
    CHECK(!fs::is_symlink(fs::symlink_status(output)));
    if (fs::exists(output))
        CHECK(fs::is_regular_file(output) && fs::hard_link_count(output) == 1);
    std::ofstream stream(output, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(sample.bytes.data()), sample.used);
    stream.close();
    CHECK(stream.good());
    std::printf("Exported host fixture: %zu bytes, 12 frames, 24 draws, 4 scenes\n", sample.used);
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc != 1 && (argc != 3 || std::string_view(argv[1]) != "--dump-sample")) {
        std::fprintf(stderr, "Usage: %s [--dump-sample <path>]\n", argv[0]);
        return 2;
    }
    test_conversion();
    test_recorder();
    test_wire();
    std::printf("PASS: %u independent cases, %u checks\n", cases, checks);
    if (argc == 3)
        dump_sample(argv[2]);
}
