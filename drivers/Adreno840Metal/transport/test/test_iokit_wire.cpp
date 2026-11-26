/* SPDX-License-Identifier: MIT */
#include "a840_iokit_wire.h"
#include "a840_hwtest_hook.h"
#include "a840_runtime_cmd.h"
#include "a840_context_init.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <thread>
#include <vector>

static unsigned cases, checks;
static const char *case_name = "";
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d [%s] %s\n", __FILE__, __LINE__, case_name, #condition); \
        std::exit(1); \
    } \
} while (0)

static void scenario(const char *name)
{
    ++cases;
    case_name = name;
}

struct Sizes { size_t in, out; };
static constexpr Sizes envelopes[] = {
    {0,0}, {0,24}, {16,8}, {32,8}, {8,0}, {3104,8}, {16,0}, {0,8}, {0,16},
    {80,0}, {80,80}, {8,8}, {8,0}, {8,16}, {16,0}, {0,64}, {16,8}, {8,0}, {128,0},
    {3104,16}, {528,16}, {0,16}, {0,0}, {16,32}, {160,16}, {48,48},
};
static_assert(offsetof(A840WireSubmit, buffers) == 32);
static_assert(offsetof(A840WireSubmitBatch, buffers) == 32);
static_assert(offsetof(A840WireDeviceInfo, va_start) == 24);
static_assert(offsetof(A840WireBoCpuMapReply, bytes) == 8);
static_assert(offsetof(NativeAdrenoAbiWaitAnyV3, fences) == 272);
static_assert(offsetof(NativeAdrenoAbiSurfaceMetaV3, meta_offset) == 32);

template <typename T>
static T read(const void *ptr)
{
    T t;
    std::memcpy(&t, ptr, sizeof(t));
    return t;
}

template <typename T>
static A840TransportStatus answer(void *out, const T &t)
{
    std::memcpy(out, &t, sizeof(t));
    return A840_TRANSPORT_OK;
}

struct Mock {
    struct Bo {
        std::vector<uint32_t> words;
        uint64_t va = 0;
        uint32_t permissions = 0, token = 0;
        bool leased = false, mapped = false, imported = false;
    };
    struct Call { uint32_t method, action, offset, count; };
    std::vector<Call> calls;
    std::map<uint64_t, Bo> bos;
    std::set<uint64_t> queues;
    std::vector<A840WireSubmitBuffer> staged, last_ibs;
    std::vector<uint32_t> captured;
    A840WireParams params{2, 48, 48, NativeAdrenoParamsV2SubmitBatch |
                         NativeAdrenoParamsV2OwnedCoherent | NativeAdrenoParamsV2HostImport,
                         UINT64_C(64) * 1024 * 1024};
    static constexpr uint64_t window = UINT64_C(0x100000000);
    static constexpr uint64_t window_bytes = UINT64_C(512) * 1024 * 1024;
    NativeAdrenoAbiZeroPageReplyV3 zero{window, 65536};
    NativeAdrenoAbiSurfaceMetaV3 meta{};
    uint64_t next_handle = 1, next_queue = 1, next_fence = 1, complete_fence = 0, batch = 0;
    uint64_t batch_queue = 0, next_batch = 1;
    uint32_t total = 0, priority = A840_QUEUE_PRIORITY_NORMAL;
    uint32_t fail_method = 0, fail_action = 0, fail_offset = 0;
    A840TransportStatus fail_status = A840_TRANSPORT_STATE_ERROR;
    unsigned map_calls = 0, unmap_calls = 0, release_calls = 0, publishes = 0;
    bool auto_complete = true, fail_map = false, short_view = false, unknown_device = false;
    bool malformed_submit = false, malformed_wait = false, bad_iova = false;

    static A840TransportStatus call(void *state, uint32_t method, const void *input,
        size_t in_bytes, void *output, size_t out_bytes)
    {
        return static_cast<Mock *>(state)->dispatch(method, input, in_bytes, output, out_bytes);
    }

    Bo *for_token(uint32_t token)
    {
        for (auto &pair : bos)
            if (pair.second.token == token) return &pair.second;
        return nullptr;
    }

    static A840TransportStatus map(void *state, uint32_t token, uint64_t bytes,
        bool coherent, void **out)
    {
        auto &mock = *static_cast<Mock *>(state);
        ++mock.map_calls;
        auto *bo = mock.for_token(token);
        CHECK(bo && bo->leased && !bo->mapped);
        CHECK(bytes == bo->words.size() * sizeof(uint32_t));
        CHECK(coherent == bool(mock.params.flags & NativeAdrenoParamsV2OwnedCoherent));
        if (mock.fail_map) return A840_TRANSPORT_OOM;
        if (mock.short_view) {
            *out = nullptr;
            return A840_TRANSPORT_OK;
        }
        bo->mapped = true;
        *out = bo->words.data();
        return A840_TRANSPORT_OK;
    }

    static A840TransportStatus unmap(void *state, uint32_t token)
    {
        auto &mock = *static_cast<Mock *>(state);
        ++mock.unmap_calls;
        auto *bo = mock.for_token(token);
        CHECK(bo && bo->mapped && bo->leased);
        bo->mapped = false;
        return A840_TRANSPORT_OK;
    }

    A840TransportStatus publish(const A840WireSubmitBuffer *ibs, size_t count, uint64_t &fence)
    {
        std::vector<uint32_t> frame;
        for (size_t i = 0; i < count; ++i) {
            const auto &ib = ibs[i];
            auto it = bos.find(ib.handle);
            if (it == bos.end()) return A840_TRANSPORT_DANGLING_BO;
            const auto &bo = it->second;
            const auto bytes = bo.words.size() * sizeof(uint32_t);
            if (!bo.va || ib.iova < bo.va || ib.iova - bo.va > bytes ||
                ib.bytes > bytes - (ib.iova - bo.va)) return A840_TRANSPORT_OUT_OF_BOUNDS;
            const size_t first = static_cast<size_t>((ib.iova - bo.va) / 4);
            frame.insert(frame.end(), bo.words.begin() + first, bo.words.begin() + first + ib.bytes / 4);
        }
        ++publishes;
        last_ibs.assign(ibs, ibs + count);
        captured = std::move(frame);
        fence = malformed_submit ? 0 : next_fence++;
        if (auto_complete) complete_fence = fence;
        return A840_TRANSPORT_OK;
    }

    size_t count(uint32_t method, uint32_t action = 0) const
    {
        return static_cast<size_t>(std::count_if(calls.begin(), calls.end(), [&](Call item) {
            return item.method == method && (!action || item.action == action);
        }));
    }

    A840TransportStatus dispatch(uint32_t method, const void *input, size_t in_bytes,
        void *output, size_t out_bytes)
    {
        CHECK(method > 0 && method < std::size(envelopes) && method != NativeAdrenoMethodMapSparseV3);
        CHECK(in_bytes == envelopes[method].in && out_bytes == envelopes[method].out);
        CHECK((!in_bytes || input) && (!out_bytes || output));
        const auto request = method == A840_WIRE_SUBMIT_BATCH
            ? read<A840WireSubmitBatch>(input) : A840WireSubmitBatch{};
        calls.push_back({method, request.action, request.offset, request.buffer_count});
        if (method == fail_method && request.action == fail_action && request.offset == fail_offset)
            return fail_status;
        switch (method) {
        case A840_WIRE_QUERY:
            return answer(output, params);
        case A840_WIRE_GET_DEVICE_INFO:
            return answer(output, A840WireDeviceInfo{unknown_device ? 0u : 0x44050a31u,
                18u * 1024u * 1024u, 0x4000000, UINT64_C(0x1fffffffff000), window, window_bytes,
                16, 6, 1, 6, A840_TRANSPORT_DEV_CLIENT_IOVA, 0});
        case NativeAdrenoMethodGetZeroPageV3:
            return answer(output, zero);
        case A840_WIRE_BO_ALLOC:
        case A840_WIRE_BO_IMPORT: {
            const uint64_t bytes = method == A840_WIRE_BO_ALLOC ?
                read<A840WireBoAlloc>(input).bytes : read<A840WireBoImport>(input).bytes;
            CHECK(bytes && bytes % 4096 == 0);
            const uint64_t id = next_handle++;
            auto &bo = bos[id];
            bo.words.resize(static_cast<size_t>(bytes / 4));
            bo.token = static_cast<uint32_t>(id + 1000);
            bo.imported = method == A840_WIRE_BO_IMPORT;
            return answer(output, A840WireBoAllocReply{id});
        }
        case A840_WIRE_BO_MAP: {
            const auto req = read<A840WireBoMap>(input);
            auto it = bos.find(req.handle);
            if (it == bos.end()) return A840_TRANSPORT_NOT_FOUND;
            CHECK(req.reserved == 0 && req.iova % 4096 == 0);
            CHECK(req.shareability == A840_BO_SHARE_INNER || req.shareability == A840_BO_SHARE_OUTER);
            CHECK((params.flags & NativeAdrenoParamsV2OwnedCoherent) || req.writeback == 0);
            it->second.va = req.iova;
            it->second.permissions = req.permissions;
            return answer(output, A840WireBoMapReply{req.iova + (bad_iova ? 4096u : 0u)});
        }
        case A840_WIRE_BO_CPU_MAP: {
            auto &bo = bos.at(read<A840WireBoCpuMap>(input).handle);
            CHECK(!bo.leased && !bo.imported);
            bo.leased = true;
            return answer(output, A840WireBoCpuMapReply{bo.token, 0, bo.words.size() * sizeof(uint32_t)});
        }
        case A840_WIRE_BO_CPU_UNMAP: {
            const auto req = read<A840WireBoCpuUnmap>(input);
            auto &bo = bos.at(req.handle);
            CHECK(bo.leased && !bo.mapped && req.token == bo.token && req.reserved == 0);
            bo.leased = false;
            ++release_calls;
            return A840_TRANSPORT_OK;
        }
        case A840_WIRE_BO_FREE: {
            auto it = bos.find(read<A840WireBoFree>(input).handle);
            if (it == bos.end()) return A840_TRANSPORT_NOT_FOUND;
            CHECK(!it->second.mapped && !it->second.leased);
            bos.erase(it);
            return A840_TRANSPORT_OK;
        }
        case A840_WIRE_CREATE_QUEUE: {
            CHECK(read<A840WireQueue>(input).queue == 0);
            const auto handle = next_queue++;
            queues.insert(handle);
            return answer(output, A840WireQueueReply{handle});
        }
        case A840_WIRE_CLOSE_QUEUE:
            return queues.erase(read<A840WireQueue>(input).queue) ? A840_TRANSPORT_OK : A840_TRANSPORT_NOT_FOUND;
        case A840_WIRE_SET_PRIORITY:
            priority = read<A840WirePriority>(input).foreground;
            CHECK(read<A840WirePriority>(input).background == A840_QUEUE_PRIORITY_DISABLED);
            return A840_TRANSPORT_OK;
        case A840_WIRE_SUBMIT: {
            const auto req = read<A840WireSubmit>(input);
            CHECK(req.buffer_count && req.buffer_count <= 128 && req.reserved == 0);
            CHECK(!req.queue || queues.count(req.queue));
            for (uint32_t i = req.buffer_count; i < 128; ++i) CHECK(req.buffers[i].handle == 0);
            if (req.wait_fence > complete_fence) return A840_TRANSPORT_TIMEOUT;
            A840WireSubmitReply reply{};
            const auto result = publish(req.buffers, req.buffer_count, reply.fence);
            return result == A840_TRANSPORT_OK ? answer(output, reply) : result;
        }
        case A840_WIRE_SUBMIT_BATCH:
            return batch_dispatch(request, output);
        case A840_WIRE_WAIT_FENCE:
            return read<A840WireWaitFence>(input).fence <= complete_fence
                ? A840_TRANSPORT_OK : A840_TRANSPORT_TIMEOUT;
        case A840_WIRE_GET_TIMESTAMP:
            return answer(output, A840WireTimestamp{777});
        case A840_WIRE_GET_STATUS:
            return answer(output, A840WireStatus{0, 0});
        case A840_WIRE_SET_METADATA:
            CHECK(read<A840WireMetadata>(input).reserved == 0);
            return A840_TRANSPORT_OK;
        case A840_WIRE_GET_METADATA:
            return answer(output, read<A840WireMetadata>(input));
        case A840_WIRE_NOTE:
            CHECK(read<A840WireNote>(input).count <= A840_WIRE_NOTE_WORDS);
            return A840_TRANSPORT_OK;
        case NativeAdrenoMethodWaitAnyV3: {
            const auto req = read<NativeAdrenoAbiWaitAnyV3>(input);
            uint32_t mask = 0;
            for (uint32_t i = 0; i < req.count; ++i)
                if (req.fences[i] <= complete_fence) mask |= UINT32_C(1) << i;
            const uint32_t full = req.count == 32 ? UINT32_MAX : (UINT32_C(1) << req.count) - 1;
            const bool done = req.flags ? mask == full : mask != 0;
            return answer(output, NativeAdrenoAbiWaitAnyReplyV3{
                malformed_wait ? UINT32_MAX : mask, done ? 0u : 1u, 777});
        }
        case NativeAdrenoMethodMapSharedPackV3:
            return answer(output, NativeAdrenoAbiSharedPackReplyV3{window * 2, 0, 65536,
                read<NativeAdrenoAbiSharedPackReqV3>(input).expected_stack_key});
        case NativeAdrenoMethodWriterPromiseV3:
            return answer(output, NativeAdrenoAbiWriterPromiseReplyV3{5, 1, 0});
        case NativeAdrenoMethodSurfaceMetaV3: {
            const auto req = read<NativeAdrenoAbiSurfaceMetaV3>(input);
            if (req.op) meta = req;
            if (meta.bo_handle != req.bo_handle) return A840_TRANSPORT_NOT_FOUND;
            auto reply = meta;
            reply.op = req.op;
            return answer(output, reply);
        }
        default:
            return A840_TRANSPORT_UNSUPPORTED;
        }
    }

    A840TransportStatus batch_dispatch(const A840WireSubmitBatch &req, void *output)
    {
        for (uint32_t i = req.buffer_count; i < 128; ++i) CHECK(req.buffers[i].handle == 0);
        if (req.action == A840_WIRE_BATCH_BEGIN) {
            CHECK(!req.batch && !req.offset && !req.buffer_count && req.total_buffers <= 1024);
            if (batch) return A840_TRANSPORT_BUSY;
            batch = next_batch++;
            batch_queue = req.queue;
            total = req.total_buffers;
            staged.clear();
            return answer(output, A840WireSubmitBatchReply{batch, 0});
        }
        CHECK(batch && req.batch == batch && req.offset == staged.size() &&
              req.queue == batch_queue && req.total_buffers == total);
        if (req.action == A840_WIRE_BATCH_APPEND) {
            CHECK(req.buffer_count && req.buffer_count <= 128 && req.offset + req.buffer_count <= total);
            staged.insert(staged.end(), req.buffers, req.buffers + req.buffer_count);
            return answer(output, A840WireSubmitBatchReply{batch, 0});
        }
        CHECK(!req.buffer_count);
        A840WireSubmitBatchReply reply{batch, 0};
        auto result = A840_TRANSPORT_OK;
        if (req.action == A840_WIRE_BATCH_COMMIT) {
            CHECK(staged.size() == total);
            result = publish(staged.data(), staged.size(), reply.fence);
        } else {
            CHECK(req.action == A840_WIRE_BATCH_ABORT);
        }
        batch = 0;
        staged.clear();
        return result == A840_TRANSPORT_OK ? answer(output, reply) : result;
    }
};

struct Fixture {
    Mock mock;
    A840IokitWire *wire = nullptr;
    A840BackendTransport backend{};
    A840RtDevice *device = nullptr;

    void open(bool runtime = false, bool prefix = false)
    {
        CHECK(a840_wire_create(Mock::call, Mock::map, Mock::unmap, &mock, &wire) == A840_TRANSPORT_OK);
        backend = a840_wire_interface(wire);
        if (runtime) {
            CHECK(a840_rt_device_create(&backend, A840_ROUTE_ALL, &device) == A840_RT_OK);
            if (prefix) CHECK(a840_wire_attach_context(wire, device) == A840_TRANSPORT_OK);
        }
    }

    ~Fixture()
    {
        a840_wire_detach_context(wire);
        a840_rt_device_destroy(device);
        a840_wire_destroy(wire);
        CHECK(mock.bos.empty() && mock.queues.empty() && mock.batch == 0);
    }
};

static A840RtBuffer *buffer(A840RtDevice *dev, uint32_t size = 4096)
{
    const A840RtBufferDesc desc{size, 4096, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE |
        A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
    A840RtBuffer *buf = nullptr;
    CHECK(a840_rt_buffer_create(dev, &desc, &buf) == A840_RT_OK);
    return buf;
}

static void test_envelopes_and_open()
{
    for (uint32_t method = 1; method < std::size(envelopes); ++method) {
        scenario("independent method envelope");
        size_t in = 1, out = 1;
        if (method == NativeAdrenoMethodMapSparseV3) {
            CHECK(a840_wire_method_sizes(method, &in, &out) == A840_TRANSPORT_UNSUPPORTED);
            CHECK(in == 1 && out == 1);
        } else {
            CHECK(a840_wire_method_sizes(method, &in, &out) == A840_TRANSPORT_OK);
            CHECK(in == envelopes[method].in && out == envelopes[method].out);
        }
    }
    scenario("invalid open inputs");
    Mock mock;
    A840IokitWire *wire = nullptr;
    CHECK(a840_wire_create(nullptr, Mock::map, Mock::unmap, &mock, &wire) == A840_TRANSPORT_INVALID_ARG);
    CHECK(a840_wire_create(Mock::call, Mock::map, nullptr, &mock, &wire) == A840_TRANSPORT_INVALID_ARG);
    CHECK(wire == nullptr && mock.calls.empty());

    scenario("query returns failure");
    mock.fail_method = A840_WIRE_QUERY;
    CHECK(a840_wire_create(Mock::call, Mock::map, Mock::unmap, &mock, &wire) == A840_TRANSPORT_STATE_ERROR);
    CHECK(wire == nullptr && mock.count(A840_WIRE_GET_DEVICE_INFO) == 0);

    scenario("unknown ABI is refused");
    mock.fail_method = 0;
    mock.params.abi_version = 1;
    CHECK(a840_wire_create(Mock::call, Mock::map, Mock::unmap, &mock, &wire) == A840_TRANSPORT_UNSUPPORTED);
    CHECK(wire == nullptr);

    scenario("malformed device info is refused");
    mock.params.abi_version = 2;
    mock.unknown_device = true;
    CHECK(a840_wire_create(Mock::call, Mock::map, Mock::unmap, &mock, &wire) == A840_TRANSPORT_STATE_ERROR);
    CHECK(wire == nullptr);

    scenario("raw v2 calls use exact bytes");
    Fixture f;
    f.open();
    A840WireStatus status{};
    CHECK(a840_wire_call(f.wire, A840_WIRE_GET_STATUS, nullptr, 0, &status, sizeof(status)) == A840_TRANSPORT_OK);
    CHECK(!status.faults && !status.suspends);
    const A840WireNote note{1, 1, {73}};
    CHECK(a840_wire_call(f.wire, A840_WIRE_NOTE, &note, sizeof(note), nullptr, 0) == A840_TRANSPORT_OK);
    const A840WireMetadata text{2, 2, 0, {'H', 'i'}};
    A840WireMetadata reply{};
    CHECK(a840_wire_call(f.wire, A840_WIRE_SET_METADATA, &text, sizeof(text), nullptr, 0) == A840_TRANSPORT_OK);
    CHECK(a840_wire_call(f.wire, A840_WIRE_GET_METADATA, &text, sizeof(text), &reply, sizeof(reply)) == A840_TRANSPORT_OK);
    CHECK(reply.text[0] == 'H' && reply.bytes == 2);
    CHECK(a840_wire_call(f.wire, A840_WIRE_GET_STATUS, nullptr, 0, &status, sizeof(status)-1) == A840_TRANSPORT_INVALID_ARG);
    CHECK(a840_wire_call(f.wire, A840_WIRE_GET_STATUS, nullptr, 0, nullptr, sizeof(status)) == A840_TRANSPORT_INVALID_ARG);
    CHECK(a840_wire_call(f.wire, 26, nullptr, 0, nullptr, 0) == A840_TRANSPORT_UNSUPPORTED);
    CHECK(f.mock.count(A840_WIRE_GET_STATUS) == 1);
}

static void test_bo_and_queue()
{
    Fixture f;
    f.open();
    const auto *ops = f.backend.ops;
    scenario("BO and CPU map lease lifecycle");
    A840BoAllocDesc alloc{4096, 12, 0};
    uint64_t handle = 0;
    CHECK(ops->bo_alloc(f.wire, &alloc, &handle) == A840_TRANSPORT_OK && handle);
    A840BoMapDesc map{handle, Mock::window + 4096, A840_BO_MAP_CPU | A840_BO_MAP_GPU,
        A840_BO_PERM_READ | A840_BO_PERM_WRITE, 1, A840_BO_SHARE_INNER};
    A840BoMapReply reply{};
    CHECK(ops->bo_map(f.wire, &map, &reply) == A840_TRANSPORT_OK);
    CHECK(reply.cpu_ptr && reply.gpu_va == map.gpu_va && reply.bytes == 4096);
    CHECK(f.mock.bos.at(handle).leased && f.mock.bos.at(handle).mapped);
    const A840BoUnmapDesc release{handle, A840_BO_UNMAP_CPU | A840_BO_UNMAP_GPU, reply.cpu_token};
    CHECK(ops->bo_unmap(f.wire, &release) == A840_TRANSPORT_OK);
    CHECK(f.mock.map_calls == 1 && f.mock.unmap_calls == 1 && f.mock.release_calls == 1);

    scenario("map callback failure releases lease");
    reply.gpu_va = 123;
    f.mock.fail_map = true;
    CHECK(ops->bo_map(f.wire, &map, &reply) == A840_TRANSPORT_OOM && reply.gpu_va == 123);
    CHECK(!f.mock.bos.at(handle).leased);
    f.mock.fail_map = false;

    scenario("missing mapped pointer releases lease");
    f.mock.short_view = true;
    CHECK(ops->bo_map(f.wire, &map, &reply) == A840_TRANSPORT_STATE_ERROR);
    CHECK(!f.mock.bos.at(handle).leased && !f.mock.bos.at(handle).mapped);
    f.mock.short_view = false;

    scenario("GPU mapping failure releases CPU view");
    f.mock.fail_method = A840_WIRE_BO_MAP;
    CHECK(ops->bo_map(f.wire, &map, &reply) == A840_TRANSPORT_STATE_ERROR);
    CHECK(!f.mock.bos.at(handle).leased && !f.mock.bos.at(handle).mapped);
    f.mock.fail_method = 0;

    scenario("GPU mapping must echo requested address");
    f.mock.bad_iova = true;
    CHECK(ops->bo_map(f.wire, &map, &reply) == A840_TRANSPORT_STATE_ERROR);
    CHECK(reply.gpu_va == 123 && !f.mock.bos.at(handle).leased);
    f.mock.bad_iova = false;
    CHECK(ops->bo_free(f.wire, handle) == A840_TRANSPORT_OK);

    scenario("invalid BO sizes and alignment");
    alloc = {4095, 12, 0};
    CHECK(ops->bo_alloc(f.wire, &alloc, &handle) == A840_TRANSPORT_INVALID_ARG);
    alloc = {4096, 25, 0};
    CHECK(ops->bo_alloc(f.wire, &alloc, &handle) == A840_TRANSPORT_INVALID_ARG);

    scenario("page-aligned BO import");
    alignas(4096) std::array<unsigned char, 4096> imported{};
    const A840BoImportDesc import{imported.data(), imported.size()};
    CHECK(ops->bo_import(f.wire, &import, &handle) == A840_TRANSPORT_OK);
    CHECK(f.mock.bos.at(handle).imported);
    CHECK(ops->bo_free(f.wire, handle) == A840_TRANSPORT_OK);

    scenario("unadvertised BO import skipped");
    Fixture old;
    old.mock.params.flags = 0;
    old.open();
    CHECK(old.backend.ops->bo_import(old.wire, &import, &handle) == A840_TRANSPORT_UNSUPPORTED);
    CHECK(old.mock.count(A840_WIRE_BO_IMPORT) == 0);

    scenario("connection priority aggregates live queues");
    CHECK(ops->queue_create(f.wire, A840_QUEUE_PRIORITY_DISABLED, &handle) == A840_TRANSPORT_INVALID_ARG);
    uint64_t high = 0, low = 0;
    CHECK(ops->queue_create(f.wire, A840_QUEUE_PRIORITY_LOW, &low) == A840_TRANSPORT_OK);
    CHECK(f.mock.priority == A840_QUEUE_PRIORITY_LOW);
    CHECK(ops->queue_create(f.wire, A840_QUEUE_PRIORITY_HIGH, &high) == A840_TRANSPORT_OK);
    CHECK(f.mock.priority == A840_QUEUE_PRIORITY_HIGH);
    CHECK(ops->queue_destroy(f.wire, high) == A840_TRANSPORT_OK);
    CHECK(f.mock.priority == A840_QUEUE_PRIORITY_LOW);
    CHECK(ops->queue_destroy(f.wire, low) == A840_TRANSPORT_OK);
    CHECK(f.mock.priority == A840_QUEUE_PRIORITY_NORMAL);

    scenario("priority RPC failure closes just-created queue");
    f.mock.fail_method = A840_WIRE_SET_PRIORITY;
    CHECK(ops->queue_create(f.wire, A840_QUEUE_PRIORITY_HIGH, &high) == A840_TRANSPORT_STATE_ERROR);
    CHECK(f.mock.queues.empty());
}

static void test_v3()
{
    scenario("v3 feature pack and zero-page exclusion");
    Fixture f;
    f.mock.params.abi_version = 3;
    f.mock.params.flags |= NativeAdrenoParamsV3TimelineWaitAny | NativeAdrenoParamsV3SharedZeroPage |
        NativeAdrenoParamsV3SharedTier0Pack | NativeAdrenoParamsV3WriterPromise | NativeAdrenoParamsV3UbwcSurfaceMeta |
        NativeAdrenoParamsV3SparseVmBind;
    f.open(true, true);
    const auto *ops = f.backend.ops;
    NativeAdrenoAbiZeroPageReplyV3 zero{};
    CHECK(ops->get_zero_page_v3(f.wire, &zero) == A840_TRANSPORT_OK);
    CHECK(zero.gpu_va == Mock::window && f.mock.count(NativeAdrenoMethodGetZeroPageV3) == 1);
    CHECK(!f.device->zero_page_owned && f.device->zero_page_va == zero.gpu_va);
    CHECK(f.device->transport_caps.va_start == zero.gpu_va + zero.bytes);
    CHECK(f.device->border_color_va != zero.gpu_va);

    const NativeAdrenoAbiSharedPackReqV3 pack{91, 0, 0};
    NativeAdrenoAbiSharedPackReplyV3 pack_reply{};
    CHECK(a840_wire_call(f.wire, NativeAdrenoMethodMapSharedPackV3, &pack, sizeof(pack),
                         &pack_reply, sizeof(pack_reply)) == A840_TRANSPORT_OK);
    CHECK(pack_reply.actual_stack_key == 91);
    NativeAdrenoAbiWriterPromiseV3 promise{};
    promise.op = NativeAdrenoPromiseMake;
    promise.surface_count = 1;
    promise.surface_ids[0] = 3;
    NativeAdrenoAbiWriterPromiseReplyV3 promise_reply{};
    CHECK(a840_wire_call(f.wire, NativeAdrenoMethodWriterPromiseV3, &promise, sizeof(promise),
                         &promise_reply, sizeof(promise_reply)) == A840_TRANSPORT_OK);
    CHECK(promise_reply.accepted == 1 && promise_reply.promise_token != 0);
    CHECK(a840_wire_call(f.wire, NativeAdrenoMethodMapSparseV3, nullptr, 0, nullptr, 0) == A840_TRANSPORT_UNSUPPORTED);

    scenario("v3 SurfaceMeta set and get");
    NativeAdrenoAbiSurfaceMetaV3 meta{97, 1, 1, 6, 16, 6, 1, 4096, 64, 1};
    CHECK(ops->surface_meta_v3(f.wire, &meta) == A840_TRANSPORT_OK);
    meta = {97, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(ops->surface_meta_v3(f.wire, &meta) == A840_TRANSPORT_OK);
    CHECK(meta.meta_offset == 4096 && meta.highest_bank_bit == 16 && meta.op == 0);

    scenario("v3 WAIT_ANY mask and ticks");
    f.mock.complete_fence = 5;
    NativeAdrenoAbiWaitAnyV3 wait{};
    wait.count = 3;
    wait.fences[0] = 2; wait.fences[1] = 7; wait.fences[2] = 5;
    NativeAdrenoAbiWaitAnyReplyV3 result{};
    CHECK(ops->wait_any_v3(f.wire, &wait, &result) == A840_TRANSPORT_OK);
    CHECK(result.satisfied_mask == 5 && result.observed_ticks == 777 && result.timed_out == 0);
    CHECK(f.mock.count(A840_WIRE_WAIT_FENCE) == 0);

    scenario("v3 WAIT_ALL timeout and partial mask");
    wait.flags = 1;
    CHECK(ops->wait_any_v3(f.wire, &wait, &result) == A840_TRANSPORT_TIMEOUT);
    CHECK(result.satisfied_mask == 5 && result.timed_out == 1);

    scenario("malformed WAIT_ANY reply rejected");
    f.mock.malformed_wait = true;
    CHECK(ops->wait_any_v3(f.wire, &wait, &result) == A840_TRANSPORT_STATE_ERROR);
    CHECK(result.satisfied_mask == 5);
    f.mock.malformed_wait = false;

    scenario("unsupported WAIT_ANY falls back to real v2 polls");
    f.mock.fail_method = NativeAdrenoMethodWaitAnyV3;
    f.mock.fail_status = A840_TRANSPORT_UNSUPPORTED;
    CHECK(ops->wait_any_v3(f.wire, &wait, &result) == A840_TRANSPORT_TIMEOUT);
    CHECK(result.satisfied_mask == 5 && f.mock.count(A840_WIRE_WAIT_FENCE) == 3);

    scenario("v2 capability absence avoids v3 calls");
    Fixture old;
    old.mock.params.flags = 0;
    old.open(true);
    CHECK(old.device->zero_page_owned && old.device->zero_page_va);
    CHECK(old.backend.ops->get_zero_page_v3(old.wire, &zero) == A840_TRANSPORT_UNSUPPORTED);
    CHECK(old.backend.ops->surface_meta_v3(old.wire, &meta) == A840_TRANSPORT_UNSUPPORTED);
    CHECK(old.mock.count(NativeAdrenoMethodGetZeroPageV3) == 0 &&
          old.mock.count(NativeAdrenoMethodSurfaceMetaV3) == 0);
    wait.count = 32; wait.flags = 1; wait.timeout_ns = 0;
    for (auto &fence : wait.fences) fence = 0;
    CHECK(old.backend.ops->wait_any_v3(old.wire, &wait, &result) == A840_TRANSPORT_OK);
    CHECK(result.satisfied_mask == UINT32_MAX && old.mock.count(NativeAdrenoMethodWaitAnyV3) == 0);

    scenario("v2 finite deadline timeout");
    wait.count = 1; wait.fences[0] = 9; wait.timeout_ns = 1000000;
    CHECK(old.backend.ops->wait_any_v3(old.wire, &wait, &result) == A840_TRANSPORT_TIMEOUT);
    CHECK(result.timed_out == 1 && result.satisfied_mask == 0 && old.mock.count(A840_WIRE_WAIT_FENCE) >= 1);

    scenario("stale zero-page flag falls back to v2 allocation");
    Fixture stale;
    stale.mock.params.abi_version = 3;
    stale.mock.params.flags |= NativeAdrenoParamsV3SharedZeroPage;
    stale.mock.fail_method = NativeAdrenoMethodGetZeroPageV3;
    stale.mock.fail_status = A840_TRANSPORT_UNSUPPORTED;
    stale.open(true);
    CHECK(stale.device->zero_page_owned);
    CHECK(!(stale.device->transport_caps.abi_flags & NativeAdrenoParamsV3SharedZeroPage));
}

static void test_submit()
{
    Fixture f;
    f.open(true, true);
    const auto *ops = f.backend.ops;
    A840RtBuffer *code = buffer(f.device);
    const uint32_t marker[] = {0x70100000};
    CHECK(a840_rt_buffer_write(code, 0, marker, sizeof(marker)) == A840_RT_OK);
    std::array<A840SubmitIb, 1024> ibs;
    ibs.fill({code->bo_handle, code->gpu_va, sizeof(marker)});
    A840SubmitDesc submit{};
    submit.ibs = ibs.data();
    submit.ib_count = 1;
    A840SubmitReply reply{999, 999};

    scenario("first submit error keeps the context pending");
    f.mock.fail_method = A840_WIRE_SUBMIT;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_STATE_ERROR);
    CHECK(reply.fence == 999 && f.mock.publishes == 0);
    f.mock.fail_method = 0;

    scenario("every submit prepends context IB while context is attached");
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_OK && reply.fence != 0);
    CHECK(f.mock.last_ibs.size() == 2 && f.mock.last_ibs[0].bytes == A840_CTX_INIT_MAX_DWORDS * 4);
    CHECK(f.mock.last_ibs[1].handle == code->bo_handle);
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_OK);
    CHECK(f.mock.last_ibs.size() == 2 && f.mock.last_ibs[0].bytes == A840_CTX_INIT_MAX_DWORDS * 4);
    CHECK(f.mock.last_ibs[1].handle == code->bo_handle);
    CHECK(a840_wire_attach_context(f.wire, f.device) == A840_TRANSPORT_STATE_ERROR);

    scenario("127 body IBs plus context IB (128 total) stay in Method 5");
    submit.ib_count = 127;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_OK);
    CHECK(f.mock.count(A840_WIRE_SUBMIT_BATCH) == 0 && f.mock.last_ibs.size() == 128);

    scenario("149 body IBs plus context IB (150 total) in 128 plus 22 are one published job");
    const auto start = f.mock.calls.size();
    const unsigned before = f.mock.publishes;
    submit.ib_count = 149;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_OK);
    CHECK(f.mock.publishes == before + 1 && f.mock.last_ibs.size() == 150);
    CHECK(f.mock.calls.size() == start + 4);
    CHECK(f.mock.calls[start].action == A840_WIRE_BATCH_BEGIN);
    CHECK(f.mock.calls[start + 1].offset == 0 && f.mock.calls[start + 1].count == 128);
    CHECK(f.mock.calls[start + 2].offset == 128 && f.mock.calls[start + 2].count == 22);
    CHECK(f.mock.calls[start + 3].action == A840_WIRE_BATCH_COMMIT && f.mock.calls[start + 3].offset == 150);

    scenario("batch dependency is waited before staging");
    submit.wait_fence = reply.fence;
    submit.wait_timeout_ns = 1000000;
    const size_t next = f.mock.calls.size();
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_OK);
    CHECK(f.mock.calls[next].method == A840_WIRE_WAIT_FENCE);
    CHECK(f.mock.calls[next + 1].action == A840_WIRE_BATCH_BEGIN);

    scenario("batch dependency timeout emits no BEGIN");
    submit.wait_fence = reply.fence + 1;
    const size_t begins = f.mock.count(A840_WIRE_SUBMIT_BATCH, A840_WIRE_BATCH_BEGIN);
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_TIMEOUT);
    CHECK(f.mock.count(A840_WIRE_SUBMIT_BATCH, A840_WIRE_BATCH_BEGIN) == begins);
    submit.wait_fence = 0;

    scenario("second APPEND failure aborts at committed offset");
    f.mock.fail_method = A840_WIRE_SUBMIT_BATCH;
    f.mock.fail_action = A840_WIRE_BATCH_APPEND;
    f.mock.fail_offset = 128;
    const unsigned published = f.mock.publishes;
    const uint64_t last_fence = reply.fence;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_STATE_ERROR);
    CHECK(f.mock.calls.back().action == A840_WIRE_BATCH_ABORT && f.mock.calls.back().offset == 128);
    CHECK(!f.mock.batch && f.mock.publishes == published && reply.fence == last_fence);

    scenario("uncertain COMMIT never replayed");
    f.mock.fail_action = A840_WIRE_BATCH_COMMIT;
    f.mock.fail_offset = 150;
    f.mock.fail_status = A840_TRANSPORT_BUSY;
    const size_t commits = f.mock.count(A840_WIRE_SUBMIT_BATCH, A840_WIRE_BATCH_COMMIT);
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_BUSY);
    CHECK(f.mock.calls.back().action == A840_WIRE_BATCH_ABORT);
    CHECK(!f.mock.batch && f.mock.publishes == published &&
          f.mock.count(A840_WIRE_SUBMIT_BATCH, A840_WIRE_BATCH_COMMIT) == commits + 1);
    f.mock.fail_method = 0;

    scenario("1023 body IBs plus context IB (1024 max) map to eight APPEND records");
    submit.ib_count = 1023;
    const size_t appends = f.mock.count(A840_WIRE_SUBMIT_BATCH, A840_WIRE_BATCH_APPEND);
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_OK);
    CHECK(f.mock.last_ibs.size() == 1024);
    CHECK(f.mock.count(A840_WIRE_SUBMIT_BATCH, A840_WIRE_BATCH_APPEND) == appends + 8);
    submit.ib_count = 1024;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_OUT_OF_BOUNDS);

    scenario("invalid lengths and reloc input rejected without a call");
    const size_t count = f.mock.calls.size();
    submit.ib_count = 1025;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_INVALID_ARG);
    submit.ib_count = 1;
    submit.reloc_count = 1;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_INVALID_ARG);
    submit.reloc_count = 0;
    ibs[0].bytes = 5;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_INVALID_ARG);
    CHECK(f.mock.calls.size() == count);
    ibs[0].bytes = 4;

    scenario("zero-fence answer cannot become a successful submit");
    f.mock.malformed_submit = true;
    CHECK(ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_STATE_ERROR);
    a840_rt_buffer_release(code);

    scenario("missing batch capability reports unsupported without splitting the job");
    Fixture old;
    old.mock.params.flags = 0;
    old.open(true);
    A840RtBuffer *old_code = buffer(old.device);
    ibs.fill({old_code->bo_handle, old_code->gpu_va, 4});
    submit.ib_count = 150;
    CHECK(old.backend.ops->queue_submit(old.wire, &submit, &reply) == A840_TRANSPORT_UNSUPPORTED);
    CHECK(!old.mock.publishes && old.mock.count(A840_WIRE_SUBMIT_BATCH) == 0);
    a840_rt_buffer_release(old_code);
}

static void test_first_batch()
{
    Fixture f;
    f.open(true, true);
    auto *body = buffer(f.device);
    std::array<A840SubmitIb, 128> ibs;
    ibs.fill({body->bo_handle, body->gpu_va, 4});
    A840SubmitDesc submit{};
    submit.ib_count = static_cast<uint32_t>(ibs.size());
    submit.ibs = ibs.data();
    A840SubmitReply reply{19, 99};

    scenario("first batch failure leaves the prefix pending");
    f.mock.fail_method = A840_WIRE_SUBMIT_BATCH;
    f.mock.fail_action = A840_WIRE_BATCH_APPEND;
    f.mock.fail_offset = 128;
    CHECK(f.backend.ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_STATE_ERROR);
    CHECK(f.mock.publishes == 0 && f.mock.batch == 0 && reply.fence == 19);

    scenario("first 128 body IBs plus prefix use one 129-IB job");
    f.mock.fail_method = 0;
    CHECK(f.backend.ops->queue_submit(f.wire, &submit, &reply) == A840_TRANSPORT_OK);
    CHECK(f.mock.publishes == 1 && f.mock.last_ibs.size() == 129);
    CHECK(f.mock.last_ibs[0].bytes == A840_CTX_INIT_MAX_DWORDS * 4 && f.mock.last_ibs[1].handle == body->bo_handle);
    CHECK(f.mock.count(A840_WIRE_SUBMIT) == 0);
    a840_rt_buffer_release(body);
}

static void test_hook()
{
    scenario("missing evidence and invalid predicate");
    int unknown = 0;
    CHECK(!a840_hwtest_path_for_command_buffer(nullptr));
    CHECK(!a840_hwtest_path_for_command_buffer(&unknown));
    CHECK(!a840_hwtest_record_command_buffer(&unknown, static_cast<A840FallbackPredicate>(31)));

    scenario("raw and fallback use the runner's exact strings");
    int raw = 0, fallback = 0;
    CHECK(a840_hwtest_record_command_buffer(&raw, A840_PRED_NONE));
    CHECK(std::strcmp(a840_hwtest_path_for_command_buffer(&raw), "raw") == 0);
    CHECK(a840_hwtest_record_command_buffer(&fallback, A840_PRED_RENDER_GMEM_REQUIRED));
    CHECK(std::strcmp(a840_hwtest_path_for_command_buffer(&fallback), "fallback:raw:render.gmem_required") == 0);
    CHECK(a840_hwtest_record_command_buffer(&fallback, A840_PRED_NONE));
    CHECK(std::strcmp(a840_hwtest_path_for_command_buffer(&fallback), "raw") == 0);
    a840_hwtest_forget_command_buffer(&fallback);
    CHECK(!a840_hwtest_path_for_command_buffer(&fallback));

    scenario("ring overwrite evicts stale identities");
    std::array<int, A840_HWTEST_RECENT_COMMANDS> cbs{};
    for (auto &cb : cbs) CHECK(a840_hwtest_record_command_buffer(&cb, A840_PRED_NONE));
    CHECK(!a840_hwtest_path_for_command_buffer(&raw));
    CHECK(std::strcmp(a840_hwtest_path_for_command_buffer(&cbs.back()), "raw") == 0);
    for (auto &cb : cbs) a840_hwtest_forget_command_buffer(&cb);

    scenario("parallel writers and thread-local readers");
    std::array<int, 8> keys{};
    std::array<std::thread, 8> workers;
    std::array<bool, 8> passed{};
    for (size_t i = 0; i < keys.size(); ++i) {
        workers[i] = std::thread([&, i] {
            bool all = true;
            for (unsigned round = 0; round < 100; ++round) {
                all &= a840_hwtest_record_command_buffer(&keys[i], A840_PRED_RENDER_GMEM_REQUIRED);
                const char *text = a840_hwtest_path_for_command_buffer(&keys[i]);
                if (text && std::strcmp(text, "fallback:raw:render.gmem_required") != 0)
                    all = false;
                /* A busy ring may have evicted this key before the read. */
            }
            a840_hwtest_forget_command_buffer(&keys[i]);
            passed[i] = all;
        });
    }
    for (auto &worker : workers) worker.join();
    for (bool pass : passed) CHECK(pass);
}

static A840CompiledStage compiled(uint32_t kind)
{
    A840CompiledStage stage{};
    stage.abi_version = ADRENO_COMPILE_ABI_VERSION;
    stage.stage = kind;
    stage.isa_dwords = 32;
    stage.instruction_groups = 8;
    stage.max_full_reg = 5;
    stage.max_half_reg = -1;
    stage.constlen_vec4 = 128;
    stage.local_size[0] = 8;
    stage.local_size[1] = 4;
    stage.local_size[2] = 1;
    return stage;
}

static void little_endian_dump(const std::vector<uint32_t> &words, const char *path)
{
    if (!path) return;
    FILE *file = std::fopen(path, "wb");
    CHECK(file);
    for (uint32_t word : words) {
        const uint8_t bytes[] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
            static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
        CHECK(std::fwrite(bytes, sizeof(bytes), 1, file) == 1);
    }
    CHECK(std::fclose(file) == 0);
}

static void test_full_frame(const char *path)
{
    scenario("real C core submission with wire Context Init, Blit, Compute and Sysmem VS/FS");
    Fixture f;
    f.open(true, true);
    A840RtQueue *queue = nullptr;
    CHECK(a840_rt_queue_create(f.device, A840_QUEUE_PRIORITY_NORMAL, &queue) == A840_RT_OK);
    A840RtCmdBuffer *cb = nullptr;
    CHECK(a840_rt_cb_create(queue, nullptr, &cb) == A840_RT_OK);
    auto *src = buffer(f.device), *dst = buffer(f.device);
    auto *shader = buffer(f.device), *table = buffer(f.device);

    A840RtTextureDesc texture{};
    texture.mtl_pixel_format = 80;
    texture.tex_type = A840_TEX_2D;
    texture.width = texture.height = 32;
    texture.depth = texture.array_layers = 1;
    texture.tile_mode = A840_TILE_TILED;
    texture.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_RENDER_TARGET;
    A840RtTexture *target = nullptr;
    CHECK(a840_rt_texture_create(f.device, &texture, &target) == A840_RT_OK);
    CHECK(a840_rt_buffer_write(table, 0, target->tex_const, sizeof(target->tex_const)) == A840_RT_OK);

    A840RtBlitEncoder *blit = nullptr;
    CHECK(a840_rt_cb_begin_blit(cb, &blit) == A840_RT_OK);
    CHECK(a840_rt_blit_copy_buffer(blit, src, 0, dst, 16, 128) == A840_RT_OK);
    CHECK(a840_rt_blit_end(blit) == A840_RT_OK);
    a840_rt_blit_destroy(blit);
    CHECK(a840_rt_cb_encode_barrier(cb, A840_CACHE_UCHE_INV, true, true) == A840_RT_OK);

    A840RtComputeEncoder *compute = nullptr;
    CHECK(a840_rt_cb_begin_compute(cb, &compute) == A840_RT_OK);
    auto cs = compiled(A840_STAGE_COMPUTE);
    A840RtComputePipelineDesc pipeline{};
    pipeline.stage = &cs;
    pipeline.code_buffer = shader;
    pipeline.bindless_tex = true;
    pipeline.local_id_regid = 4;
    pipeline.wg_id_regid = 8;
    CHECK(a840_rt_compute_set_pipeline(compute, &pipeline) == A840_RT_OK);
    A840RtBuffer *tables[] = {table};
    CHECK(a840_rt_compute_set_bindless(compute, 1, tables, nullptr) == A840_RT_OK);
    CHECK(a840_rt_compute_dispatch_threads(compute, 19, 11, 1) == A840_RT_OK);
    CHECK(a840_rt_compute_end(compute) == A840_RT_OK);
    a840_rt_compute_destroy(compute);

    A840RtRenderPassDesc pass{};
    pass.color_count = pass.sample_count = 1;
    pass.color_targets[0] = target;
    A840RtRenderEncoder *render = nullptr;
    CHECK(a840_rt_cb_begin_render_pass(cb, &pass, &render) == A840_RT_OK);
    auto vs = compiled(A840_STAGE_VERTEX), fs = compiled(A840_STAGE_FRAGMENT);
    A840RtGraphicsProgramDesc program{};
    program.emit_desc.vs.stage = &vs;
    program.emit_desc.fs.stage = &fs;
    program.vs_code_buffer = program.fs_code_buffer = shader;
    program.fs_code_offset = 128;
    program.emit_desc.position = 0;
    program.emit_desc.point_size = A840_REGID_UNUSED;
    program.emit_desc.vertex_id = 4;
    program.emit_desc.instance_id = program.emit_desc.primitive_id =
        program.emit_desc.view_id = A840_REGID_UNUSED;
    std::memset(&program.emit_desc.fs_inputs, A840_REGID_UNUSED, sizeof(program.emit_desc.fs_inputs));
    program.emit_desc.fs_inputs.frag_coord_mask = 0;
    program.emit_desc.depth_output = program.emit_desc.stencil_output =
        program.emit_desc.sample_mask_output = A840_REGID_UNUSED;
    program.emit_desc.colors[0] = {0, 15, false};
    CHECK(a840_rt_render_set_program(render, &program) == A840_RT_OK);

    A840RtRenderStateDesc state{};
    state.viewport_count = state.scissor_count = 1;
    state.viewports[0] = {0, 0, 32, 32, 0, 1};
    state.scissors[0] = {0, 0, 32, 32};
    state.line_width = 1;
    state.sample_mask = UINT16_MAX;
    state.color_blends[0].write_mask = 15;
    CHECK(a840_rt_render_set_state(render, &state) == A840_RT_OK);
    A840RtDrawDesc draw{};
    draw.primitive = A840_PRIM_TRIANGLES;
    draw.count = 3;
    draw.instance_count = 1;
    CHECK(a840_rt_render_draw(render, &draw) == A840_RT_OK);
    CHECK(a840_rt_render_end_pass(render) == A840_RT_OK);
    a840_rt_render_destroy(render);

    CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);
    CHECK(a840_rt_cb_wait_until_completed(cb, 0) == A840_RT_OK);
    CHECK(a840_rt_cb_last_route(cb).path == A840_ROUTE_PATH_RAW);
    CHECK(a840_hwtest_record_command_buffer(cb, A840_PRED_NONE));
    CHECK(std::strcmp(a840_hwtest_path_for_command_buffer(cb), "raw") == 0);

    CHECK(f.mock.last_ibs.size() == 2 && f.mock.last_ibs[0].bytes == A840_CTX_INIT_MAX_DWORDS * 4);
    CHECK(f.mock.captured.size() == A840_CTX_INIT_MAX_DWORDS + a840_rt_cb_used_dwords(cb));
    const auto &words = f.mock.captured;
    std::vector<A840PM4Packet> packets(words.size());
    size_t count = 0, ambles = 0;
    CHECK(a840_pm4_decode(words.data(), words.size(), packets.data(), packets.size(), &count) == A840_EMIT_OK);
    bool saw_blit = false, saw_compute = false, saw_draw = false;
    for (size_t i = 0; i < count; ++i) {
        const auto &packet = packets[i];
        if (packet.type == 4) {
            for (uint32_t reg = 0; reg < packet.payload_words; ++reg)
                CHECK(!a840_reg_is_umd_blocked_on_a8xx(packet.identifier + reg));
        } else {
            if (packet.identifier == A840_CP_SET_AMBLE) {
                CHECK(packet.header_word < A840_CTX_INIT_MAX_DWORDS);
                const uint64_t addr = words[packet.header_word + 1] |
                    (uint64_t{words[packet.header_word + 2]} << 32);
                if (addr) {
                    CHECK(addr >= f.mock.last_ibs[0].iova + A840_CTX_INIT_MAX_DWORDS * 4);
                    CHECK(addr < f.mock.last_ibs[0].iova + 4096 && addr != f.device->zero_page_va);
                    ++ambles;
                }
            }
            saw_blit |= packet.identifier == A840_CP_MEMCPY;
            saw_compute |= packet.identifier == A840_CP_EXEC_CS;
            saw_draw |= packet.identifier == A840_CP_DRAW_INDX_OFFSET;
        }
    }
    CHECK(ambles == 2 && saw_blit && saw_compute && saw_draw);
    const auto &last = packets[count - 1];
    CHECK(last.identifier == A840_CP_EVENT_WRITE7 &&
          (words[last.header_word + 1] & 0xff) == A840_EVENT_RB_DONE_TS);
    little_endian_dump(words, path);
    std::printf("full frame: %zu dwords, %zu packets, %u context dwords, %zu live BR/BV ambles\n",
        words.size(), count, A840_CTX_INIT_MAX_DWORDS, ambles);

    a840_hwtest_forget_command_buffer(cb);
    a840_rt_cb_release(cb);
    a840_rt_texture_release(target);
    for (auto *bo : {src, dst, shader, table}) a840_rt_buffer_release(bo);
    a840_rt_queue_destroy(queue);
}

int main(int argc, char **argv)
{
    const char *path = nullptr;
    if (argc == 3 && std::strcmp(argv[1], "--dump-full-frame-pm4") == 0) path = argv[2];
    else if (argc != 1) {
        std::fprintf(stderr, "usage: %s [--dump-full-frame-pm4 path]\n", argv[0]);
        return 2;
    }
    test_envelopes_and_open();
    test_bo_and_queue();
    test_v3();
    test_submit();
    test_first_batch();
    test_hook();
    test_full_frame(path);
    std::printf("W04-L3 IOKit wire: %u independent cases, %u checks passed\n", cases, checks);
}
