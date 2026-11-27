/* SPDX-License-Identifier: MIT */
/* Comprehensive unit, integration, protocol, priority-inheritance, multi-tier
 * cache, client transport/circuit-breaker, and TSan 20x concurrency stress
 * suite for NezhaShaderCompiler (W07-L2).
 */
#include "a840_shader_compiler_client.h"
#include "a840_shader_compiler_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using ByteVec = std::vector<uint8_t>;

constexpr uint64_t kStackKey = UINT64_C(0x8401a2b3c4d5e6f7);
constexpr uint32_t kClientPid = UINT32_C(0x00007a42);
constexpr size_t kMemBudget = 1024u * 1024u;
constexpr auto kWaitLimit = 10s;

std::atomic<uint64_t> g_assertions{0};
unsigned g_cases_passed = 0;
const char *g_current_case = "";

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++g_assertions;                                                        \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__,  \
                g_current_case, #expr);                                        \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

template <typename Fn>
void run_case(const char *name, Fn &&fn)
{
    g_current_case = name;
    fn();
    ++g_cases_passed;
}

A840PipelineKey make_key(uint32_t tag)
{
    A840PipelineKey k{};
    k.sha256[0] = static_cast<uint8_t>(tag & 0xffu);
    k.sha256[1] = static_cast<uint8_t>((tag >> 8) & 0xffu);
    k.sha256[2] = static_cast<uint8_t>((tag >> 16) & 0xffu);
    k.sha256[3] = static_cast<uint8_t>((tag >> 24) & 0xffu);
    k.sha256[31] = 0x84u;
    return k;
}

uint32_t key_tag(const A840PipelineKey &k)
{
    return uint32_t(k.sha256[0]) | (uint32_t(k.sha256[1]) << 8) |
           (uint32_t(k.sha256[2]) << 16) | (uint32_t(k.sha256[3]) << 24);
}

A840ScFrameHeader make_req_hdr(uint32_t tag, uint64_t req_id,
    A840ScPriority prio = A840_SC_PRIO_BG_ASYNC, uint32_t payload_bytes = 4u,
    uint64_t stack_key = kStackKey, uint32_t pid = kClientPid)
{
    A840ScFrameHeader h{};
    h.magic = A840_SC_PROTO_MAGIC;
    h.version = A840_SC_PROTO_VERSION;
    h.msg_type = (prio == A840_SC_PRIO_WARMUP_LOW) ? A840_SC_MSG_PREWARM_HINT
                                                   : A840_SC_MSG_COMPILE_REQUEST;
    h.priority_class = prio;
    h.stack_key = stack_key;
    h.abi_fingerprint = a840_compile_abi_fingerprint();
    h.pipeline_key = make_key(tag);
    h.request_id = req_id;
    h.client_pid = pid;
    h.status = A840_SC_OK;
    h.payload_bytes = payload_bytes;
    return h;
}

A840ScFrameHeader make_ping_hdr(uint64_t req_id, uint32_t pid = kClientPid)
{
    A840ScFrameHeader h{};
    h.magic = A840_SC_PROTO_MAGIC;
    h.version = A840_SC_PROTO_VERSION;
    h.msg_type = A840_SC_MSG_PING_STATUS;
    h.priority_class = A840_SC_PRIO_FG_SYNC;
    h.stack_key = kStackKey;
    h.abi_fingerprint = a840_compile_abi_fingerprint();
    h.request_id = req_id;
    h.client_pid = pid;
    return h;
}

ByteVec encode_ok(const A840ScFrameHeader &h, const ByteVec &body = {})
{
    CHECK(h.payload_bytes == body.size());
    ByteVec out(A840_SC_HEADER_BYTES + body.size(), 0xeeu);
    size_t written = 0u;
    CHECK(a840_sc_encode_frame(&h, body.empty() ? nullptr : body.data(),
              out.data(), out.size(), &written) == A840_SC_OK);
    CHECK(written == out.size());
    return out;
}

A840ScFrameHeader decode_ok(const ByteVec &frame, const uint8_t **out_body = nullptr)
{
    A840ScFrameHeader hdr{};
    const uint8_t *body = nullptr;
    CHECK(a840_sc_decode_frame(frame.data(), frame.size(), kStackKey,
              a840_compile_abi_fingerprint(), &hdr, &body) == A840_SC_OK);
    if (out_body)
        *out_body = body;
    return hdr;
}

void expect_decode_fail(const ByteVec &frame, A840ScStatus expected)
{
    A840ScFrameHeader hdr{};
    hdr.request_id = 0xdeadbeefu;
    const uint8_t *body = reinterpret_cast<const uint8_t *>(0x1);
    A840ScStatus st = a840_sc_decode_frame(frame.data(), frame.size(),
        kStackKey, a840_compile_abi_fingerprint(), &hdr, &body);
    CHECK(st == expected);
    CHECK(hdr.request_id == 0xdeadbeefu);
    CHECK(body == reinterpret_cast<const uint8_t *>(0x1));
}

ByteVec make_valid_record_blob(uint32_t tag, uint32_t marker,
    uint64_t stack_key = kStackKey, uint32_t isa_dwords = 4u)
{
    std::vector<uint32_t> isa(isa_dwords, 0u);
    isa[0] = marker;
    if (isa_dwords > 1u)
        isa[1] = tag;

    A840PipelineRecord rec{};
    rec.abi_fingerprint = a840_compile_abi_fingerprint();
    rec.stack_key = stack_key;
    rec.chip_id = A840_RECORD_CHIP_ID;
    rec.highest_bank_bit = A840_RECORD_HBB;
    rec.stage_count = 1u;
    rec.pipeline_key = make_key(tag);
    rec.stages[0].abi_version = ADRENO_COMPILE_ABI_VERSION;
    rec.stages[0].stage = A840_STAGE_COMPUTE;
    rec.stages[0].isa_words = isa.data();
    rec.stages[0].isa_dwords = isa_dwords;
    rec.stages[0].instruction_groups = isa_dwords / 4u;
    rec.stages[0].max_full_reg = -1;
    rec.stages[0].max_half_reg = -1;

    uint8_t *raw = nullptr;
    size_t raw_bytes = 0u;
    CHECK(a840_record_serialize(&rec, &raw, &raw_bytes) == A840_RECORD_OK);
    ByteVec out(raw, raw + raw_bytes);
    std::free(raw);
    return out;
}

uint32_t verify_record_marker(const ByteVec &blob, uint32_t tag,
    uint64_t stack_key = kStackKey)
{
    A840PipelineRecord rec{};
    A840PipelineKey k = make_key(tag);
    CHECK(a840_record_deserialize(blob.data(), blob.size(), stack_key,
              a840_compile_abi_fingerprint(), &k, &rec) == A840_RECORD_OK);
    CHECK(rec.stage_count == 1u && rec.stages[0].isa_dwords >= 4u);
    uint32_t marker = rec.stages[0].isa_words[0];
    a840_record_free(&rec);
    return marker;
}

/* ---------------- 1. Wire Protocol Tests ---------------- */

void run_protocol_tests()
{
    const ByteVec body = {'a', 'b', 'c'};
    const auto req = make_req_hdr(0x030201u, UINT64_C(0x1122334455667788),
        A840_SC_PRIO_BG_ASYNC, 3u);
    const ByteVec frame = encode_ok(req, body);

    run_case("proto: canonical LE layout and roundtrip", [&] {
        CHECK(frame.size() == A840_SC_HEADER_BYTES + 3u);
        CHECK(frame[0] == '8' && frame[1] == 'S' && frame[2] == 'C' && frame[3] == '1');
        CHECK(frame[4] == 1u && frame[5] == 0u && frame[6] == 0u && frame[7] == 0u);
        CHECK(frame[8] == A840_SC_MSG_COMPILE_REQUEST);
        CHECK(frame[12] == A840_SC_PRIO_BG_ASYNC);
        const uint8_t *payload = nullptr;
        auto dec = decode_ok(frame, &payload);
        CHECK(dec.magic == A840_SC_PROTO_MAGIC);
        CHECK(dec.version == A840_SC_PROTO_VERSION);
        CHECK(dec.stack_key == kStackKey);
        CHECK(dec.abi_fingerprint == a840_compile_abi_fingerprint());
        CHECK(dec.request_id == UINT64_C(0x1122334455667788));
        CHECK(dec.client_pid == kClientPid);
        CHECK(dec.payload_bytes == 3u);
        CHECK(dec.reserved0 == 0u && dec.reserved1 == 0u);
        CHECK(key_tag(dec.pipeline_key) == 0x030201u);
        CHECK(payload == frame.data() + A840_SC_HEADER_BYTES);
        CHECK(std::memcmp(payload, body.data(), 3u) == 0);
    });

    run_case("proto: SHA-256('abc') NIST KAT vector", [&] {
        static const uint8_t kAbcSha[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };
        CHECK(std::memcmp(frame.data() + 96u, kAbcSha, 32u) == 0);
    });

    run_case("proto: PING_STATUS empty SHA-256 KAT", [&] {
        static const uint8_t kEmptySha[32] = {
            0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
            0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
        };
        ByteVec ping = encode_ok(make_ping_hdr(99u));
        const uint8_t *p = nullptr;
        auto dec = decode_ok(ping, &p);
        CHECK(dec.msg_type == A840_SC_MSG_PING_STATUS);
        CHECK(p == nullptr && dec.payload_bytes == 0u);
        CHECK(std::memcmp(ping.data() + 96u, kEmptySha, 32u) == 0);
    });

    run_case("proto: PREWARM_HINT, CANCEL_REQUEST, COMPILE_REPLY roundtrip", [&] {
        auto prewarm = make_req_hdr(7u, 10u, A840_SC_PRIO_WARMUP_LOW, 3u);
        CHECK(decode_ok(encode_ok(prewarm, body)).msg_type == A840_SC_MSG_PREWARM_HINT);

        auto cancel = make_req_hdr(7u, 11u, A840_SC_PRIO_BG_ASYNC, 0u);
        cancel.msg_type = A840_SC_MSG_CANCEL_REQUEST;
        CHECK(decode_ok(encode_ok(cancel)).msg_type == A840_SC_MSG_CANCEL_REQUEST);

        auto err_reply = make_req_hdr(7u, 12u, A840_SC_PRIO_FG_SYNC, 0u);
        err_reply.msg_type = A840_SC_MSG_COMPILE_REPLY;
        err_reply.status = A840_SC_COMPILE_FAILED;
        auto dec = decode_ok(encode_ok(err_reply));
        CHECK(dec.msg_type == A840_SC_MSG_COMPILE_REPLY && dec.status == A840_SC_COMPILE_FAILED);
    });

    run_case("proto: peek_frame_length valid and invalid", [&] {
        size_t total = 0u;
        CHECK(a840_sc_peek_frame_length(frame.data(), A840_SC_HEADER_BYTES, &total) == A840_SC_OK);
        CHECK(total == A840_SC_HEADER_BYTES + 3u);
        CHECK(a840_sc_peek_frame_length(frame.data(), A840_SC_HEADER_BYTES - 1u, &total) == A840_SC_TRUNCATED);
        ByteVec bad = frame;
        bad[0] ^= 0x01u;
        CHECK(a840_sc_peek_frame_length(bad.data(), bad.size(), &total) == A840_SC_PROTO_ERROR);
        bad = frame;
        bad[83] = 0x02u; /* > 16 MiB */
        CHECK(a840_sc_peek_frame_length(bad.data(), bad.size(), &total) == A840_SC_PAYLOAD_TOO_LARGE);
    });

    struct Corrupter {
        const char *name;
        size_t offset;
        A840ScStatus expected;
    };
    const Corrupter mutations[] = {
        {"proto: reject bad magic", 0u, A840_SC_PROTO_ERROR},
        {"proto: reject bad version", 4u, A840_SC_PROTO_ERROR},
        {"proto: reject unknown msg_type", 8u, A840_SC_UNKNOWN_MSG_TYPE},
        {"proto: reject unknown priority", 12u, A840_SC_UNKNOWN_PRIORITY},
        {"proto: reject stack_key mismatch", 16u, A840_SC_STACK_MISMATCH},
        {"proto: reject abi_fingerprint mismatch", 24u, A840_SC_ABI_MISMATCH},
        {"proto: reject request nonzero status", 76u, A840_SC_PROTO_ERROR},
        {"proto: reject payload > 16 MiB", 83u, A840_SC_PAYLOAD_TOO_LARGE},
        {"proto: reject reserved0 nonzero", 84u, A840_SC_RESERVED_NONZERO},
        {"proto: reject reserved1 nonzero", 90u, A840_SC_RESERVED_NONZERO},
        {"proto: reject digest flip", 100u, A840_SC_HASH_MISMATCH},
        {"proto: reject payload byte tamper", A840_SC_HEADER_BYTES + 1u, A840_SC_HASH_MISMATCH},
    };
    for (const auto &m : mutations) {
        run_case(m.name, [&] {
            ByteVec bad = frame;
            bad[m.offset] ^= 0x40u;
            expect_decode_fail(bad, m.expected);
        });
    }

    run_case("proto: reject every truncated length", [&] {
        for (size_t n = 0u; n < frame.size(); ++n) {
            A840ScFrameHeader hdr{};
            hdr.request_id = 0xdeadbeefu;
            const uint8_t *body = reinterpret_cast<const uint8_t *>(0x1);
            CHECK(a840_sc_decode_frame(frame.data(), n, kStackKey,
                      a840_compile_abi_fingerprint(), &hdr, &body) == A840_SC_TRUNCATED);
            CHECK(hdr.request_id == 0xdeadbeefu);
            CHECK(body == reinterpret_cast<const uint8_t *>(0x1));
        }
        A840ScFrameHeader hdr{};
        const uint8_t *body = nullptr;
        CHECK(a840_sc_decode_frame(nullptr, 0u, kStackKey,
                  a840_compile_abi_fingerprint(), &hdr, &body) == A840_SC_INVALID_ARG);
    });

    run_case("proto: reject trailing bytes", [&] {
        ByteVec extra = frame;
        extra.push_back(0x00u);
        expect_decode_fail(extra, A840_SC_PROTO_ERROR);
    });

    run_case("proto: reject zero request_id, zero pid, zero key on compile", [&] {
        ByteVec z = frame;
        std::fill_n(z.begin() + 64, 8, 0u);
        expect_decode_fail(z, A840_SC_PROTO_ERROR);

        z = frame;
        std::fill_n(z.begin() + 72, 4, 0u);
        expect_decode_fail(z, A840_SC_PROTO_ERROR);

        z = frame;
        std::fill_n(z.begin() + 32, 32, 0u);
        expect_decode_fail(z, A840_SC_PROTO_ERROR);
    });

    run_case("proto: reject semantic violations", [&] {
        ByteVec ping = encode_ok(make_ping_hdr(1u));
        ping[32] = 0x01u;
        expect_decode_fail(ping, A840_SC_PROTO_ERROR);

        ByteVec prewarm = encode_ok(make_req_hdr(2u, 2u, A840_SC_PRIO_WARMUP_LOW, 3u), body);
        prewarm[12] = A840_SC_PRIO_BG_ASYNC;
        expect_decode_fail(prewarm, A840_SC_PROTO_ERROR);

        ByteVec bad_cancel = frame;
        bad_cancel[8] = A840_SC_MSG_CANCEL_REQUEST;
        expect_decode_fail(bad_cancel, A840_SC_PROTO_ERROR);

        ByteVec empty_req = encode_ok(make_req_hdr(3u, 3u, A840_SC_PRIO_FG_SYNC, 0u));
        empty_req[8] = A840_SC_MSG_COMPILE_REPLY;
        expect_decode_fail(empty_req, A840_SC_PROTO_ERROR);
    });

    run_case("proto: exact 16 MiB accepted and 16 MiB + 1 rejected", [&] {
        ByteVec big(A840_SC_MAX_PAYLOAD_BYTES, 0x5au);
        auto hdr = make_req_hdr(55u, 77u, A840_SC_PRIO_FG_SYNC, A840_SC_MAX_PAYLOAD_BYTES);
        ByteVec big_frame = encode_ok(hdr, big);
        CHECK(decode_ok(big_frame).payload_bytes == A840_SC_MAX_PAYLOAD_BYTES);
        big_frame.push_back(0u);
        expect_decode_fail(big_frame, A840_SC_PROTO_ERROR);

        hdr.payload_bytes = A840_SC_MAX_PAYLOAD_BYTES + 1u;
        size_t written = 12345u;
        CHECK(a840_sc_encode_frame(&hdr, big.data(), big_frame.data(), big_frame.size(),
                  &written) == A840_SC_PAYLOAD_TOO_LARGE);
        CHECK(written == 12345u);
    });

    run_case("proto: status string includes raw:compiler.daemon_unavailable", [] {
        CHECK(std::strcmp(a840_sc_status_string(A840_SC_DAEMON_UNAVAILABLE),
                  "raw:compiler.daemon_unavailable") == 0);
        CHECK(std::strcmp(a840_sc_status_string(A840_SC_OK), "OK") == 0);
    });
}

/* ---------------- Test Helpers for Service & Client ---------------- */

struct CompletionItem {
    uint64_t request_id;
    A840ScStatus status;
    ByteVec record;
    std::thread::id callback_thread;
};

class CompletionCollector {
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<uint64_t, CompletionItem> items_;

public:
    void record_item(uint64_t req_id, A840ScStatus status, const uint8_t *blob, size_t bytes)
    {
        if (status == A840_SC_OK && bytes > 0u)
            CHECK(blob != nullptr);
        else
            CHECK(bytes == 0u);
        CompletionItem item{req_id, status, {}, std::this_thread::get_id()};
        if (bytes > 0u)
            item.record.assign(blob, blob + bytes);
        std::lock_guard<std::mutex> lk(mu_);
        CHECK(items_.emplace(req_id, std::move(item)).second);
        cv_.notify_all();
    }

    CompletionItem wait_id(uint64_t req_id)
    {
        std::unique_lock<std::mutex> lk(mu_);
        CHECK(cv_.wait_for(lk, kWaitLimit, [&] { return items_.count(req_id) != 0u; }));
        return items_.at(req_id);
    }

    size_t count()
    {
        std::lock_guard<std::mutex> lk(mu_);
        return items_.size();
    }

    static void service_cb(void *ctx, const A840ScFrameHeader *reply,
        const uint8_t *blob, size_t bytes)
    {
        CHECK(reply->msg_type == A840_SC_MSG_COMPILE_REPLY);
        static_cast<CompletionCollector *>(ctx)->record_item(
            reply->request_id, static_cast<A840ScStatus>(reply->status), blob, bytes);
    }

    static void client_cb(void *ctx, uint64_t req_id, A840ScStatus status,
        const uint8_t *blob, size_t bytes)
    {
        static_cast<CompletionCollector *>(ctx)->record_item(req_id, status, blob, bytes);
    }
};

class FakeCompiler {
public:
    struct CompileCall {
        uint32_t tag;
        uint32_t worker_id;
    };
    struct BoostEvent {
        uint32_t worker_id;
        A840ScPriority from;
        A840ScPriority to;
        uint64_t boosts_stat_inside_hook;
    };
    enum class BadOutput { None, WrongKey, WrongStack, CorruptBlob, EmptyOk };

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::set<uint32_t> held_tags_;
    std::map<uint32_t, A840ScStatus> fail_map_;
    std::map<uint32_t, BadOutput> bad_map_;
    std::vector<CompileCall> calls_;
    std::vector<BoostEvent> boosts_;
    bool hold_boost_ = false;
    uint32_t isa_dwords_ = 4u;

public:
    A840ScService *svc = nullptr;

    void hold(uint32_t tag)
    {
        std::lock_guard<std::mutex> lk(mu_);
        held_tags_.insert(tag);
    }

    void release(uint32_t tag)
    {
        std::lock_guard<std::mutex> lk(mu_);
        held_tags_.erase(tag);
        cv_.notify_all();
    }

    void release_all()
    {
        std::lock_guard<std::mutex> lk(mu_);
        held_tags_.clear();
        hold_boost_ = false;
        cv_.notify_all();
    }

    void hold_boost(bool on)
    {
        std::lock_guard<std::mutex> lk(mu_);
        hold_boost_ = on;
        cv_.notify_all();
    }

    void inject_failure(uint32_t tag, A840ScStatus status)
    {
        std::lock_guard<std::mutex> lk(mu_);
        fail_map_[tag] = status;
    }

    void inject_bad_output(uint32_t tag, BadOutput kind)
    {
        std::lock_guard<std::mutex> lk(mu_);
        bad_map_[tag] = kind;
    }

    void wait_call_count(size_t n)
    {
        std::unique_lock<std::mutex> lk(mu_);
        CHECK(cv_.wait_for(lk, kWaitLimit, [&] { return calls_.size() >= n; }));
    }

    BoostEvent wait_first_boost()
    {
        std::unique_lock<std::mutex> lk(mu_);
        CHECK(cv_.wait_for(lk, kWaitLimit, [&] { return !boosts_.empty(); }));
        return boosts_.front();
    }

    std::vector<CompileCall> calls()
    {
        std::lock_guard<std::mutex> lk(mu_);
        return calls_;
    }

    std::vector<uint32_t> call_tags()
    {
        std::vector<uint32_t> out;
        for (const auto &c : calls())
            out.push_back(c.tag);
        return out;
    }

    size_t boost_count()
    {
        std::lock_guard<std::mutex> lk(mu_);
        return boosts_.size();
    }

    static A840ScStatus compile_cb(void *ctx, uint32_t worker_id,
        const A840ScFrameHeader *req, const uint8_t *payload,
        uint8_t **out_blob, size_t *out_bytes)
    {
        auto &self = *static_cast<FakeCompiler *>(ctx);
        uint32_t tag = key_tag(req->pipeline_key);
        A840ScStatus fail_st = A840_SC_OK;
        BadOutput bad = BadOutput::None;
        uint32_t dwords = 4u;
        {
            std::unique_lock<std::mutex> lk(self.mu_);
            self.calls_.push_back({tag, worker_id});
            dwords = self.isa_dwords_;
            self.cv_.notify_all();
            CHECK(self.cv_.wait_for(lk, kWaitLimit,
                [&] { return self.held_tags_.count(tag) == 0u; }));
            if (auto it = self.fail_map_.find(tag); it != self.fail_map_.end()) {
                fail_st = it->second;
                self.fail_map_.erase(it);
            }
            if (auto it = self.bad_map_.find(tag); it != self.bad_map_.end()) {
                bad = it->second;
                self.bad_map_.erase(it);
            }
        }
        if (fail_st != A840_SC_OK) {
            *out_blob = static_cast<uint8_t *>(std::malloc(8));
            CHECK(*out_blob != nullptr);
            *out_bytes = 8u;
            return fail_st;
        }
        if (bad == BadOutput::EmptyOk) {
            *out_blob = nullptr;
            *out_bytes = 0u;
            return A840_SC_OK;
        }

        uint32_t marker = (req->payload_bytes > 0u && payload) ? uint32_t(payload[0]) : 0x42u;
        uint32_t rec_tag = (bad == BadOutput::WrongKey) ? (tag + 1000u) : tag;
        uint64_t rec_stack = (bad == BadOutput::WrongStack) ? (req->stack_key + 1u) : req->stack_key;
        ByteVec blob = make_valid_record_blob(rec_tag, marker, rec_stack, dwords);
        if (bad == BadOutput::CorruptBlob)
            blob.back() ^= 0xffu;

        uint8_t *copy = static_cast<uint8_t *>(std::malloc(blob.size()));
        CHECK(copy != nullptr);
        std::memcpy(copy, blob.data(), blob.size());
        *out_blob = copy;
        *out_bytes = blob.size();
        return A840_SC_OK;
    }

    static void boost_cb(void *ctx, uint32_t worker_id,
        A840ScPriority from, A840ScPriority to)
    {
        auto &self = *static_cast<FakeCompiler *>(ctx);
        A840ScServiceStats st{};
        a840_sc_service_get_stats(self.svc, &st);

        std::unique_lock<std::mutex> lk(self.mu_);
        self.boosts_.push_back({worker_id, from, to, st.priority_boosts});
        self.cv_.notify_all();
        CHECK(self.cv_.wait_for(lk, kWaitLimit, [&] { return !self.hold_boost_; }));
    }
};

struct ServiceHarness {
    FakeCompiler compiler;
    CompletionCollector collector;
    A840ScService *svc = nullptr;
    uint64_t stack_key;

    explicit ServiceHarness(uint32_t workers = 1u, size_t mem_entries = 32u,
        size_t mem_bytes = kMemBudget, const char *disk_dir = nullptr,
        uint64_t sk = kStackKey)
        : stack_key(sk)
    {
        A840ScServiceConfig cfg{};
        cfg.stack_key = sk;
        cfg.abi_fingerprint = a840_compile_abi_fingerprint();
        cfg.worker_count = workers;
        cfg.mem_cache_entries = mem_entries;
        cfg.mem_cache_bytes = mem_bytes;
        cfg.disk_cache_dir = disk_dir;
        cfg.compile_fn = FakeCompiler::compile_cb;
        cfg.on_priority_boost = FakeCompiler::boost_cb;
        cfg.user_ctx = &compiler;
        CHECK(a840_sc_service_create(&cfg, &svc) == A840_SC_OK);
        compiler.svc = svc;
    }

    ~ServiceHarness()
    {
        compiler.release_all();
        a840_sc_service_destroy(svc);
    }

    A840ScStatus submit(uint32_t tag, uint64_t req_id,
        A840ScPriority prio = A840_SC_PRIO_BG_ASYNC, uint8_t marker = 0x33u,
        uint32_t pid = kClientPid)
    {
        auto hdr = make_req_hdr(tag, req_id, prio, 1u, stack_key, pid);
        return a840_sc_service_submit(svc, &hdr, &marker,
            CompletionCollector::service_cb, &collector);
    }

    void submit_ok(uint32_t tag, uint64_t req_id,
        A840ScPriority prio = A840_SC_PRIO_BG_ASYNC, uint8_t marker = 0x33u)
    {
        CHECK(submit(tag, req_id, prio, marker) == A840_SC_OK);
    }

    A840ScStatus cancel(uint32_t tag, uint64_t req_id, uint32_t pid = kClientPid)
    {
        A840PipelineKey k = make_key(tag);
        return a840_sc_service_cancel(svc, &k, pid, req_id);
    }

    A840ScServiceStats stats() const
    {
        A840ScServiceStats s{};
        a840_sc_service_get_stats(svc, &s);
        return s;
    }

    ByteVec wait_ok(uint64_t req_id, uint32_t tag, uint32_t expected_marker = 0x33u)
    {
        auto item = collector.wait_id(req_id);
        CHECK(item.status == A840_SC_OK);
        CHECK(verify_record_marker(item.record, tag, stack_key) == expected_marker);
        return item.record;
    }
};

class SpinBarrier {
    std::mutex mu_;
    std::condition_variable cv_;
    unsigned remaining_;

public:
    explicit SpinBarrier(unsigned n) : remaining_(n) {}
    void wait()
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (--remaining_ == 0u)
            cv_.notify_all();
        else
            CHECK(cv_.wait_for(lk, kWaitLimit, [&] { return remaining_ == 0u; }));
    }
};

struct TempDir {
    std::filesystem::path dir;
    TempDir()
    {
        const char *base = std::getenv("TMPDIR");
        std::filesystem::path root = (base && base[0]) ? std::filesystem::path(base)
                                                       : std::filesystem::temp_directory_path();
        std::string pattern = (root / "w07-l2-cache-XXXXXX").string();
        CHECK(mkdtemp(pattern.data()) != nullptr);
        dir = pattern;
    }
    ~TempDir() { std::filesystem::remove_all(dir); }
};

/* ---------------- 2. Scheduling, Dedup, Boost & Cache Tests ---------------- */

void run_service_tests()
{
    run_case("service: reject invalid worker count and ABI mismatch", [] {
        FakeCompiler fc;
        A840ScServiceConfig cfg{};
        cfg.stack_key = kStackKey;
        cfg.abi_fingerprint = a840_compile_abi_fingerprint();
        cfg.worker_count = 0u;
        cfg.mem_cache_entries = 8u;
        cfg.mem_cache_bytes = kMemBudget;
        cfg.compile_fn = FakeCompiler::compile_cb;
        cfg.user_ctx = &fc;

        A840ScService *svc = nullptr;
        CHECK(a840_sc_service_create(&cfg, &svc) == A840_SC_INVALID_ARG && !svc);
        cfg.worker_count = 9u;
        CHECK(a840_sc_service_create(&cfg, &svc) == A840_SC_INVALID_ARG && !svc);
        cfg.worker_count = 2u;
        cfg.abi_fingerprint += 1u;
        CHECK(a840_sc_service_create(&cfg, &svc) == A840_SC_ABI_MISMATCH && !svc);
    });

    run_case("service: 8 workers run concurrently with distinct IDs", [] {
        ServiceHarness h(8u);
        for (uint32_t i = 1u; i <= 8u; ++i) {
            h.compiler.hold(i);
            h.submit_ok(i, i);
        }
        h.compiler.wait_call_count(8u);
        std::set<uint32_t> workers;
        for (const auto &c : h.compiler.calls())
            CHECK(c.worker_id < 8u && workers.insert(c.worker_id).second);
        CHECK(h.stats().max_inflight_seen == 8u);
        h.compiler.release_all();
        for (uint32_t i = 1u; i <= 8u; ++i)
            h.wait_ok(i, i);
    });

    run_case("service: strict priority order FG_SYNC > BG_ASYNC > WARMUP_LOW", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.submit_ok(1u, 1u, A840_SC_PRIO_BG_ASYNC);
        h.compiler.wait_call_count(1u);

        h.submit_ok(2u, 2u, A840_SC_PRIO_WARMUP_LOW);
        h.submit_ok(3u, 3u, A840_SC_PRIO_BG_ASYNC);
        h.submit_ok(4u, 4u, A840_SC_PRIO_WARMUP_LOW);
        h.submit_ok(5u, 5u, A840_SC_PRIO_FG_SYNC);
        h.submit_ok(6u, 6u, A840_SC_PRIO_FG_SYNC);

        h.compiler.release(1u);
        for (uint32_t i = 1u; i <= 6u; ++i)
            h.wait_ok(i, i);

        CHECK(h.compiler.call_tags() == std::vector<uint32_t>({1u, 5u, 6u, 3u, 2u, 4u}));
        auto st = h.stats();
        CHECK(st.fg_requests == 2u && st.bg_requests == 4u && st.compiled_ok == 6u);
    });

    run_case("service: queued BG_ASYNC boosted atomically to FG_SYNC head", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.submit_ok(1u, 1u, A840_SC_PRIO_BG_ASYNC);
        h.compiler.wait_call_count(1u);

        h.submit_ok(2u, 2u, A840_SC_PRIO_BG_ASYNC);
        h.submit_ok(3u, 3u, A840_SC_PRIO_BG_ASYNC);
        h.submit_ok(4u, 4u, A840_SC_PRIO_FG_SYNC);
        h.submit_ok(3u, 5u, A840_SC_PRIO_FG_SYNC);

        auto st = h.stats();
        CHECK(st.priority_boosts == 1u && st.dedup_hits == 1u);
        h.compiler.release(1u);
        for (uint32_t i = 1u; i <= 4u; ++i)
            h.wait_ok(i, i);
        h.wait_ok(5u, 3u);
        CHECK(h.compiler.call_tags() == std::vector<uint32_t>({1u, 3u, 4u, 2u}));
        CHECK(h.compiler.boost_count() == 0u);
    });

    run_case("service: queued WARMUP_LOW boosted to BG_ASYNC then FG_SYNC", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.submit_ok(1u, 1u, A840_SC_PRIO_BG_ASYNC);
        h.compiler.wait_call_count(1u);

        h.submit_ok(2u, 2u, A840_SC_PRIO_WARMUP_LOW);
        h.submit_ok(3u, 3u, A840_SC_PRIO_BG_ASYNC);
        h.submit_ok(2u, 4u, A840_SC_PRIO_BG_ASYNC);
        h.submit_ok(4u, 5u, A840_SC_PRIO_FG_SYNC);
        h.submit_ok(2u, 6u, A840_SC_PRIO_FG_SYNC);

        CHECK(h.stats().priority_boosts == 2u && h.stats().dedup_hits == 2u);
        h.compiler.release(1u);
        h.wait_ok(1u, 1u);
        h.wait_ok(2u, 2u);
        h.wait_ok(3u, 3u);
        h.wait_ok(4u, 2u);
        h.wait_ok(5u, 4u);
        h.wait_ok(6u, 2u);
        CHECK(h.compiler.call_tags() == std::vector<uint32_t>({1u, 2u, 4u, 3u}));
    });

    run_case("service: running BG_ASYNC triggers on_priority_boost once outside lock", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.submit_ok(1u, 1u, A840_SC_PRIO_BG_ASYNC);
        h.compiler.wait_call_count(1u);

        h.submit_ok(1u, 2u, A840_SC_PRIO_FG_SYNC);
        h.submit_ok(1u, 3u, A840_SC_PRIO_FG_SYNC);
        auto ev = h.compiler.wait_first_boost();
        CHECK(ev.worker_id == h.compiler.calls()[0].worker_id);
        CHECK(ev.from == A840_SC_PRIO_BG_ASYNC && ev.to == A840_SC_PRIO_FG_SYNC);
        CHECK(ev.boosts_stat_inside_hook == 1u);
        CHECK(h.compiler.boost_count() == 1u);

        h.compiler.release(1u);
        h.wait_ok(1u, 1u);
        h.wait_ok(2u, 1u);
        h.wait_ok(3u, 1u);
        CHECK(h.stats().dedup_hits == 2u && h.stats().compiled_ok == 1u);
    });

    run_case("service: worker stays pinned while running boost hook executes", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.compiler.hold_boost(true);
        h.submit_ok(1u, 1u, A840_SC_PRIO_WARMUP_LOW);
        h.compiler.wait_call_count(1u);

        std::thread fg([&] { h.submit_ok(1u, 2u, A840_SC_PRIO_FG_SYNC); });
        auto ev = h.compiler.wait_first_boost();
        CHECK(ev.from == A840_SC_PRIO_WARMUP_LOW && ev.to == A840_SC_PRIO_FG_SYNC);

        h.submit_ok(9u, 9u, A840_SC_PRIO_BG_ASYNC);
        h.compiler.release(1u);
        std::this_thread::sleep_for(25ms);
        CHECK(h.compiler.call_tags() == std::vector<uint32_t>({1u}));

        h.compiler.hold_boost(false);
        fg.join();
        h.wait_ok(1u, 1u);
        h.wait_ok(2u, 1u);
        h.wait_ok(9u, 9u);
        CHECK(h.compiler.call_tags() == std::vector<uint32_t>({1u, 9u}));
    });

    run_case("service: 8-thread concurrent dedup invokes compiler exactly once (dedup_hits == 7)", [] {
        ServiceHarness h(4u);
        h.compiler.hold(42u);
        SpinBarrier bar(8u);
        std::array<std::thread, 8> threads;
        for (size_t i = 0; i < 8u; ++i) {
            threads[i] = std::thread([&, i] {
                bar.wait();
                h.submit_ok(42u, i + 1u, (i % 2u == 0u) ? A840_SC_PRIO_BG_ASYNC
                                                        : A840_SC_PRIO_FG_SYNC);
            });
        }
        for (auto &t : threads)
            t.join();
        CHECK(h.stats().dedup_hits == 7u);
        CHECK(h.stats().max_inflight_seen == 1u);

        h.compiler.release(42u);
        ByteVec ref = h.wait_ok(1u, 42u);
        for (uint64_t i = 2u; i <= 8u; ++i)
            CHECK(h.wait_ok(i, 42u) == ref);
        CHECK(h.compiler.call_tags() == std::vector<uint32_t>({42u}));
        CHECK(h.stats().compiled_ok == 1u);
    });

    run_case("service: duplicate (pid, request_id) rejected on same key", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.submit_ok(1u, 77u);
        CHECK(h.submit(1u, 77u) == A840_SC_DUPLICATE_REQUEST);

        CompletionCollector other_cc;
        auto hdr2 = make_req_hdr(1u, 77u, A840_SC_PRIO_FG_SYNC, 1u, kStackKey, kClientPid + 1u);
        uint8_t marker = 0x33u;
        CHECK(a840_sc_service_submit(h.svc, &hdr2, &marker,
                  CompletionCollector::service_cb, &other_cc) == A840_SC_OK);

        h.compiler.release(1u);
        h.wait_ok(77u, 1u);
        CHECK(other_cc.wait_id(77u).status == A840_SC_OK);
        CHECK(h.stats().compiled_ok == 1u && h.stats().dedup_hits == 1u);
    });

    run_case("service: memory LRU hit and eviction", [] {
        ServiceHarness h(1u, 2u, kMemBudget);
        h.submit_ok(1u, 1u);
        h.wait_ok(1u, 1u);
        h.submit_ok(2u, 2u);
        h.wait_ok(2u, 2u);
        h.submit_ok(1u, 3u, A840_SC_PRIO_FG_SYNC);
        h.wait_ok(3u, 1u);
        CHECK(h.stats().cache_mem_hits == 1u);
        h.submit_ok(3u, 4u);
        h.wait_ok(4u, 3u);
        h.submit_ok(1u, 5u);
        h.wait_ok(5u, 1u);
        h.submit_ok(2u, 6u);
        h.wait_ok(6u, 2u);
        auto st = h.stats();
        CHECK(st.cache_mem_hits == 2u && st.compiled_ok == 4u);
        CHECK(h.compiler.call_tags() == std::vector<uint32_t>({1u, 2u, 3u, 2u}));
    });

    run_case("service: disk A840PR01 persistence across restart (zero recompile)", [] {
        TempDir tmp;
        ByteVec first_blob;
        {
            ServiceHarness s1(2u, 4u, kMemBudget, tmp.dir.c_str());
            s1.submit_ok(101u, 1u, A840_SC_PRIO_FG_SYNC, 0x79u);
            first_blob = s1.wait_ok(1u, 101u, 0x79u);
            CHECK(s1.stats().compiled_ok == 1u);
        }
        {
            ServiceHarness s2(2u, 4u, kMemBudget, tmp.dir.c_str());
            s2.submit_ok(101u, 2u, A840_SC_PRIO_FG_SYNC, 0x11u);
            ByteVec second_blob = s2.wait_ok(2u, 101u, 0x79u);
            CHECK(second_blob == first_blob);
            CHECK(s2.stats().cache_disk_hits == 1u);
            CHECK(s2.stats().compiled_ok == 0u);
            CHECK(s2.compiler.calls().empty());
            s2.submit_ok(101u, 3u, A840_SC_PRIO_FG_SYNC, 0x11u);
            CHECK(s2.wait_ok(3u, 101u, 0x79u) == first_blob);
            CHECK(s2.stats().cache_mem_hits == 1u);
        }
    });

    run_case("service: disk cache isolates foreign stack_key", [] {
        TempDir tmp;
        {
            ServiceHarness s1(1u, 4u, kMemBudget, tmp.dir.c_str(), kStackKey);
            s1.submit_ok(202u, 1u, A840_SC_PRIO_FG_SYNC, 0x51u);
            s1.wait_ok(1u, 202u, 0x51u);
        }
        {
            ServiceHarness s2(1u, 4u, kMemBudget, tmp.dir.c_str(), kStackKey + 1u);
            s2.submit_ok(202u, 2u, A840_SC_PRIO_FG_SYNC, 0x62u);
            s2.wait_ok(2u, 202u, 0x62u);
            CHECK(s2.stats().cache_disk_hits == 0u && s2.stats().compiled_ok == 1u);
        }
    });

    run_case("service: compile failure & bad backend outputs isolated from neighbors", [] {
        ServiceHarness h(2u);
        h.compiler.inject_failure(1u, A840_SC_COMPILE_FAILED);
        h.compiler.inject_failure(2u, A840_SC_TIMED_OUT);
        h.compiler.inject_bad_output(3u, FakeCompiler::BadOutput::WrongKey);
        h.compiler.inject_bad_output(4u, FakeCompiler::BadOutput::WrongStack);
        h.compiler.inject_bad_output(5u, FakeCompiler::BadOutput::CorruptBlob);
        h.compiler.inject_bad_output(6u, FakeCompiler::BadOutput::EmptyOk);

        for (uint32_t i = 1u; i <= 7u; ++i)
            h.submit_ok(i, i);

        CHECK(h.collector.wait_id(1u).status == A840_SC_COMPILE_FAILED);
        CHECK(h.collector.wait_id(2u).status == A840_SC_TIMED_OUT);
        CHECK(h.collector.wait_id(3u).status == A840_SC_COMPILE_FAILED);
        CHECK(h.collector.wait_id(4u).status == A840_SC_COMPILE_FAILED);
        CHECK(h.collector.wait_id(5u).status == A840_SC_COMPILE_FAILED);
        CHECK(h.collector.wait_id(6u).status == A840_SC_COMPILE_FAILED);
        h.wait_ok(7u, 7u);
        CHECK(h.stats().compiled_fail == 6u && h.stats().compiled_ok == 1u);

        h.submit_ok(1u, 10u);
        h.wait_ok(10u, 1u);
        CHECK(h.stats().compiled_ok == 2u);
    });

    run_case("service: cancel queued sole waiter removes job without compiling", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.submit_ok(1u, 1u);
        h.compiler.wait_call_count(1u);

        h.submit_ok(2u, 2u);
        CHECK(h.cancel(2u, 2u) == A840_SC_OK);
        CHECK(h.collector.wait_id(2u).status == A840_SC_CANCELED);
        CHECK(h.cancel(2u, 2u) == A840_SC_NOT_FOUND);

        h.compiler.release(1u);
        h.wait_ok(1u, 1u);
        CHECK(h.compiler.call_tags() == std::vector<uint32_t>({1u}));
        CHECK(h.stats().canceled == 1u);
    });

    run_case("service: cancel one dedup waiter preserves sibling waiter", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.submit_ok(1u, 1u);
        h.compiler.wait_call_count(1u);
        h.submit_ok(1u, 2u, A840_SC_PRIO_FG_SYNC);

        CHECK(h.cancel(1u, 1u) == A840_SC_OK);
        CHECK(h.collector.wait_id(1u).status == A840_SC_CANCELED);

        h.compiler.release(1u);
        h.wait_ok(2u, 1u);
        CHECK(h.stats().canceled == 1u && h.stats().compiled_ok == 1u);
    });

    run_case("service: shutdown cancels queued jobs, drains running, rejects new", [] {
        ServiceHarness h(1u);
        h.compiler.hold(1u);
        h.submit_ok(1u, 1u);
        h.compiler.wait_call_count(1u);
        h.submit_ok(2u, 2u);
        h.submit_ok(3u, 3u, A840_SC_PRIO_FG_SYNC);

        std::thread s1([&] { a840_sc_service_shutdown(h.svc); });
        std::thread s2([&] { a840_sc_service_shutdown(h.svc); });

        CHECK(h.collector.wait_id(2u).status == A840_SC_CANCELED);
        CHECK(h.collector.wait_id(3u).status == A840_SC_CANCELED);
        CHECK(h.submit(4u, 4u) == A840_SC_SHUTDOWN);

        h.compiler.release(1u);
        s1.join();
        s2.join();
        h.wait_ok(1u, 1u);
        CHECK(h.stats().canceled == 2u && h.stats().compiled_ok == 1u);
    });

    run_case("service: handle_frame PING, COMPILE, CANCEL, malformed rejection", [] {
        ServiceHarness h(1u);
        struct ReplySink {
            std::mutex mu;
            std::condition_variable cv;
            std::vector<ByteVec> frames;
        } sink;
        auto reply_cb = [](void *ctx, const uint8_t *frame, size_t bytes) {
            auto *s = static_cast<ReplySink *>(ctx);
            std::lock_guard<std::mutex> lk(s->mu);
            s->frames.emplace_back(frame, frame + bytes);
            s->cv.notify_all();
        };

        ByteVec ping = encode_ok(make_ping_hdr(10u));
        CHECK(a840_sc_service_handle_frame(h.svc, ping.data(), ping.size(), reply_cb, &sink) == A840_SC_OK);
        {
            std::unique_lock<std::mutex> lk(sink.mu);
            CHECK(sink.cv.wait_for(lk, kWaitLimit, [&] { return sink.frames.size() == 1u; }));
            auto hdr = decode_ok(sink.frames[0]);
            CHECK(hdr.msg_type == A840_SC_MSG_COMPILE_REPLY && hdr.status == A840_SC_OK && hdr.request_id == 10u);
        }

        ByteVec bad = ping;
        bad[0] ^= 0xffu;
        CHECK(a840_sc_service_handle_frame(h.svc, bad.data(), bad.size(), reply_cb, &sink) == A840_SC_PROTO_ERROR);
        CHECK(sink.frames.size() == 1u);

        ByteVec body = {0x55u};
        ByteVec comp = encode_ok(make_req_hdr(88u, 20u, A840_SC_PRIO_FG_SYNC, 1u), body);
        CHECK(a840_sc_service_handle_frame(h.svc, comp.data(), comp.size(), reply_cb, &sink) == A840_SC_OK);
        {
            std::unique_lock<std::mutex> lk(sink.mu);
            CHECK(sink.cv.wait_for(lk, kWaitLimit, [&] { return sink.frames.size() == 2u; }));
            const uint8_t *rec_ptr = nullptr;
            auto hdr = decode_ok(sink.frames[1], &rec_ptr);
            CHECK(hdr.status == A840_SC_OK && hdr.request_id == 20u && hdr.payload_bytes > 0u);
            ByteVec blob(rec_ptr, rec_ptr + hdr.payload_bytes);
            CHECK(verify_record_marker(blob, 88u) == 0x55u);
        }
    });
}

/* ---------------- 3. Client Stub & Socketpair Transport Tests ---------------- */

class SocketBridge {
    int fds_[2] = {-1, -1};
    A840ScService *svc_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> drop_transport_{false};
    std::atomic<bool> corrupt_reply_{false};
    std::mutex send_mu_;
    std::thread server_thread_;

    static bool write_all(int fd, const uint8_t *buf, size_t len, uint32_t timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        size_t off = 0u;
        while (off < len) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            int rem = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            struct pollfd pfd{fd, POLLOUT, 0};
            int pr = poll(&pfd, 1, std::max(rem, 1));
            if (pr <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                return false;
            ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
            if (n <= 0)
                return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    static A840ScStatus read_exact(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        size_t off = 0u;
        while (off < len) {
            int rem = 0;
            if (timeout_ms > 0u) {
                auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    return off == 0u ? A840_SC_TIMED_OUT : A840_SC_DAEMON_UNAVAILABLE;
                rem = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            }
            struct pollfd pfd{fd, POLLIN, 0};
            int pr = poll(&pfd, 1, rem);
            if (pr == 0)
                return off == 0u ? A840_SC_TIMED_OUT : A840_SC_DAEMON_UNAVAILABLE;
            if (pr < 0)
                return A840_SC_DAEMON_UNAVAILABLE;
            ssize_t n = recv(fd, buf + off, len - off, 0);
            if (n <= 0)
                return A840_SC_DAEMON_UNAVAILABLE;
            off += static_cast<size_t>(n);
        }
        return A840_SC_OK;
    }

    static A840ScStatus read_frame(int fd, uint8_t **out_buf, size_t *out_len, uint32_t timeout_ms)
    {
        uint8_t hdr[A840_SC_HEADER_BYTES];
        A840ScStatus st = read_exact(fd, hdr, sizeof(hdr), timeout_ms);
        if (st != A840_SC_OK)
            return st;
        size_t total = 0u;
        st = a840_sc_peek_frame_length(hdr, sizeof(hdr), &total);
        if (st != A840_SC_OK)
            return A840_SC_DAEMON_UNAVAILABLE;
        uint8_t *buf = static_cast<uint8_t *>(std::malloc(total));
        if (!buf)
            return A840_SC_OOM;
        std::memcpy(buf, hdr, sizeof(hdr));
        if (total > sizeof(hdr)) {
            st = read_exact(fd, buf + sizeof(hdr), total - sizeof(hdr),
                timeout_ms ? timeout_ms : 200u);
            if (st != A840_SC_OK) {
                std::free(buf);
                return A840_SC_DAEMON_UNAVAILABLE;
            }
        }
        *out_buf = buf;
        *out_len = total;
        return A840_SC_OK;
    }

    void server_loop()
    {
        while (!stop_.load()) {
            uint8_t *wire = nullptr;
            size_t len = 0u;
            A840ScStatus st = read_frame(fds_[1], &wire, &len, 20u);
            if (st == A840_SC_TIMED_OUT)
                continue;
            if (st != A840_SC_OK)
                break;
            (void)a840_sc_service_handle_frame(svc_, wire, len, &SocketBridge::server_reply, this);
            std::free(wire);
        }
    }

    static void server_reply(void *ctx, const uint8_t *frame, size_t len)
    {
        auto *self = static_cast<SocketBridge *>(ctx);
        if (self->stop_.load(std::memory_order_acquire))
            return;
        ByteVec copy(frame, frame + len);
        if (self->corrupt_reply_.exchange(false) && copy.size() > A840_SC_HEADER_BYTES)
            copy.back() ^= 0x01u;
        std::lock_guard<std::mutex> lk(self->send_mu_);
        if (!self->stop_.load(std::memory_order_acquire) && self->fds_[1] >= 0)
            (void)write_all(self->fds_[1], copy.data(), copy.size(), 1000u);
    }

public:
    SocketBridge() = default;
    explicit SocketBridge(A840ScService *svc) { start(svc); }

    void start(A840ScService *svc)
    {
        svc_ = svc;
        stop_.store(false, std::memory_order_release);
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_) == 0);
        server_thread_ = std::thread([this] { server_loop(); });
    }

    ~SocketBridge() { stop(); }

    void set_down(bool down) { drop_transport_.store(down); }
    void corrupt_next_reply() { corrupt_reply_.store(true); }

    void stop()
    {
        if (!stop_.exchange(true, std::memory_order_acq_rel)) {
            if (fds_[0] >= 0)
                shutdown(fds_[0], SHUT_RDWR);
            if (fds_[1] >= 0)
                shutdown(fds_[1], SHUT_RDWR);
            if (server_thread_.joinable())
                server_thread_.join();
            std::lock_guard<std::mutex> lk(send_mu_);
            if (fds_[0] >= 0) {
                close(fds_[0]);
                fds_[0] = -1;
            }
            if (fds_[1] >= 0) {
                close(fds_[1]);
                fds_[1] = -1;
            }
        }
    }

    static A840ScStatus client_send(void *ctx, const uint8_t *frame, size_t len, uint32_t timeout_ms)
    {
        auto *self = static_cast<SocketBridge *>(ctx);
        if (self->drop_transport_.load() || self->fds_[0] < 0)
            return A840_SC_DAEMON_UNAVAILABLE;
        return write_all(self->fds_[0], frame, len, timeout_ms) ? A840_SC_OK
                                                               : A840_SC_DAEMON_UNAVAILABLE;
    }

    static A840ScStatus client_recv(void *ctx, uint8_t **frame, size_t *len, uint32_t timeout_ms)
    {
        auto *self = static_cast<SocketBridge *>(ctx);
        if (self->drop_transport_.load() || self->fds_[0] < 0)
            return A840_SC_DAEMON_UNAVAILABLE;
        return read_frame(self->fds_[0], frame, len, timeout_ms);
    }
};

A840ScClient *make_client(SocketBridge &bridge, uint32_t init_bo = 40u, uint32_t max_bo = 160u)
{
    A840ScClientConfig cfg{};
    cfg.stack_key = kStackKey;
    cfg.abi_fingerprint = a840_compile_abi_fingerprint();
    cfg.client_pid = kClientPid;
    cfg.initial_backoff_ms = init_bo;
    cfg.max_backoff_ms = max_bo;
    cfg.send_fn = SocketBridge::client_send;
    cfg.recv_fn = SocketBridge::client_recv;
    cfg.user_ctx = &bridge;
    A840ScClient *client = nullptr;
    CHECK(a840_sc_client_create(&cfg, &client) == A840_SC_OK);
    return client;
}

struct ClientServerHarness {
    SocketBridge bridge;
    ServiceHarness h;
    A840ScClient *client = nullptr;

    explicit ClientServerHarness(uint32_t workers = 1u, uint32_t init_bo = 40u,
        uint32_t max_bo = 160u, const char *disk_dir = nullptr)
        : bridge(), h(workers, 32u, kMemBudget, disk_dir)
    {
        bridge.start(h.svc);
        client = make_client(bridge, init_bo, max_bo);
    }

    ~ClientServerHarness()
    {
        destroy_client();
        bridge.stop();
        h.compiler.release_all();
        a840_sc_service_shutdown(h.svc);
    }

    void destroy_client()
    {
        if (client) {
            a840_sc_client_destroy(client);
            client = nullptr;
        }
    }
};

void run_client_tests()
{
    run_case("client: invalid config and ABI mismatch rejected", [] {
        A840ScClientConfig cfg{};
        A840ScClient *c = nullptr;
        CHECK(a840_sc_client_create(&cfg, &c) == A840_SC_INVALID_ARG && !c);

        ClientServerHarness env(1u);
        cfg.stack_key = kStackKey;
        cfg.abi_fingerprint = a840_compile_abi_fingerprint() + 1u;
        cfg.client_pid = kClientPid;
        cfg.send_fn = SocketBridge::client_send;
        cfg.recv_fn = SocketBridge::client_recv;
        cfg.user_ctx = &env.bridge;
        CHECK(a840_sc_client_create(&cfg, &c) == A840_SC_ABI_MISMATCH && !c);
    });

    run_case("client: ping, sync FG_SYNC, async BG_ASYNC, and prewarm over socketpair", [] {
        ClientServerHarness env(2u);

        CHECK(a840_sc_client_ping(env.client, 1000u) == A840_SC_OK);

        A840PipelineKey k1 = make_key(501u);
        uint8_t p1 = 0x2au;
        uint8_t *blob = nullptr;
        size_t blob_len = 0u;
        CHECK(a840_sc_client_compile_sync(env.client, &k1, &p1, 1u, 2000u, &blob, &blob_len) == A840_SC_OK);
        CHECK(verify_record_marker(ByteVec(blob, blob + blob_len), 501u) == 0x2au);
        std::free(blob);

        CompletionCollector cc;
        A840PipelineKey k2 = make_key(502u);
        uint8_t p2 = 0x3bu;
        uint64_t async_id = 0u;
        std::thread::id caller_tid = std::this_thread::get_id();
        CHECK(a840_sc_client_compile_async(env.client, &k2, &p2, 1u, 2000u,
                  CompletionCollector::client_cb, &cc, &async_id) == A840_SC_OK);
        CHECK(async_id != 0u);
        auto item = cc.wait_id(async_id);
        CHECK(item.status == A840_SC_OK);
        CHECK(item.callback_thread != caller_tid);
        CHECK(verify_record_marker(item.record, 502u) == 0x3bu);

        A840PipelineKey k3 = make_key(503u);
        uint8_t p3 = 0x4cu;
        uint64_t prewarm_id = 0u;
        CHECK(a840_sc_client_prewarm_async(env.client, &k3, &p3, 1u, 2000u,
                  CompletionCollector::client_cb, &cc, &prewarm_id) == A840_SC_OK);
        auto pitem = cc.wait_id(prewarm_id);
        CHECK(pitem.status == A840_SC_OK);
        CHECK(verify_record_marker(pitem.record, 503u) == 0x4cu);
    });

    run_case("client: sync FG_SYNC boosts in-flight async BG_ASYNC across socketpair", [] {
        ClientServerHarness env(1u);
        env.h.compiler.hold(601u);

        CompletionCollector cc;
        A840PipelineKey k = make_key(601u);
        uint8_t p = 0x77u;
        uint64_t bg_id = 0u;
        CHECK(a840_sc_client_compile_async(env.client, &k, &p, 1u, 4000u,
                  CompletionCollector::client_cb, &cc, &bg_id) == A840_SC_OK);
        env.h.compiler.wait_call_count(1u);

        std::thread fg([&] {
            uint8_t *blob = nullptr;
            size_t len = 0u;
            CHECK(a840_sc_client_compile_sync(env.client, &k, &p, 1u, 4000u, &blob, &len) == A840_SC_OK);
            CHECK(verify_record_marker(ByteVec(blob, blob + len), 601u) == 0x77u);
            std::free(blob);
        });

        auto ev = env.h.compiler.wait_first_boost();
        CHECK(ev.from == A840_SC_PRIO_BG_ASYNC && ev.to == A840_SC_PRIO_FG_SYNC);
        env.h.compiler.release(601u);
        fg.join();

        auto bg_res = cc.wait_id(bg_id);
        CHECK(bg_res.status == A840_SC_OK);
        CHECK(verify_record_marker(bg_res.record, 601u) == 0x77u);
        CHECK(env.h.stats().dedup_hits == 1u && env.h.stats().priority_boosts == 1u &&
              env.h.stats().compiled_ok == 1u);
    });

    run_case("client: async cancel propagates CANCEL_REQUEST to service", [] {
        ClientServerHarness env(1u);
        env.h.compiler.hold(701u);

        CompletionCollector cc;
        A840PipelineKey k1 = make_key(701u);
        A840PipelineKey k2 = make_key(702u);
        uint8_t body = 1u;
        uint64_t id1 = 0u, id2 = 0u;
        CHECK(a840_sc_client_compile_async(env.client, &k1, &body, 1u, 4000u,
                  CompletionCollector::client_cb, &cc, &id1) == A840_SC_OK);
        env.h.compiler.wait_call_count(1u);

        CHECK(a840_sc_client_compile_async(env.client, &k2, &body, 1u, 4000u,
                  CompletionCollector::client_cb, &cc, &id2) == A840_SC_OK);
        std::this_thread::sleep_for(30ms);
        CHECK(a840_sc_client_cancel(env.client, id2) == A840_SC_OK);
        CHECK(cc.wait_id(id2).status == A840_SC_CANCELED);
        std::this_thread::sleep_for(30ms);

        env.h.compiler.release(701u);
        CHECK(cc.wait_id(id1).status == A840_SC_OK);
        CHECK(env.h.compiler.call_tags() == std::vector<uint32_t>({701u}));
    });

    run_case("client: sync timeout abandons cleanly without leak", [] {
        ClientServerHarness env(1u);
        env.h.compiler.hold(801u);

        A840PipelineKey k = make_key(801u);
        uint8_t p = 9u;
        uint8_t *blob = nullptr;
        size_t len = 0u;
        CHECK(a840_sc_client_compile_sync(env.client, &k, &p, 1u, 60u, &blob, &len) == A840_SC_TIMED_OUT);
        CHECK(blob == nullptr && len == 0u);

        env.h.compiler.release(801u);
        A840PipelineKey k2 = make_key(802u);
        CHECK(a840_sc_client_compile_sync(env.client, &k2, &p, 1u, 2000u, &blob, &len) == A840_SC_OK);
        std::free(blob);
    });

    run_case("client: daemon unavailable returns A840_SC_DAEMON_UNAVAILABLE with circuit-breaker backoff and recovery", [] {
        ClientServerHarness env(1u, 60u, 120u);

        env.bridge.set_down(true);
        CHECK(a840_sc_client_ping(env.client, 500u) == A840_SC_DAEMON_UNAVAILABLE);
        CHECK(a840_sc_client_is_circuit_open(env.client));

        A840PipelineKey k = make_key(901u);
        uint8_t p = 5u;
        uint8_t *blob = nullptr;
        size_t len = 0u;
        CHECK(a840_sc_client_compile_sync(env.client, &k, &p, 1u, 500u, &blob, &len) ==
              A840_SC_DAEMON_UNAVAILABLE);

        env.bridge.set_down(false);
        std::this_thread::sleep_for(80ms);
        CHECK(!a840_sc_client_is_circuit_open(env.client));
        CHECK(a840_sc_client_ping(env.client, 1000u) == A840_SC_OK);
        CHECK(a840_sc_client_compile_sync(env.client, &k, &p, 1u, 1000u, &blob, &len) == A840_SC_OK);
        std::free(blob);
    });

    run_case("client: async in-flight calls fail with A840_SC_SHUTDOWN on destroy", [] {
        ClientServerHarness env(1u);
        env.h.compiler.hold(951u);

        CompletionCollector cc;
        A840PipelineKey k = make_key(951u);
        uint8_t p = 3u;
        uint64_t id = 0u;
        CHECK(a840_sc_client_compile_async(env.client, &k, &p, 1u, 5000u,
                  CompletionCollector::client_cb, &cc, &id) == A840_SC_OK);
        env.h.compiler.wait_call_count(1u);

        env.destroy_client();
        CHECK(cc.wait_id(id).status == A840_SC_SHUTDOWN);
    });

    run_case("client: corrupted reply frame dropped safely until timeout", [] {
        ClientServerHarness env(1u);

        env.bridge.corrupt_next_reply();
        A840PipelineKey k = make_key(961u);
        uint8_t p = 7u;
        uint8_t *blob = nullptr;
        size_t len = 0u;
        CHECK(a840_sc_client_compile_sync(env.client, &k, &p, 1u, 80u, &blob, &len) == A840_SC_TIMED_OUT);
        CHECK(blob == nullptr);

        CHECK(a840_sc_client_compile_sync(env.client, &k, &p, 1u, 1000u, &blob, &len) == A840_SC_OK);
        std::free(blob);
    });
}

/* ---------------- 4. TSan High-Concurrency Stress (20x) ---------------- */

void run_tsan_stress_round(unsigned round)
{
    TempDir tmp;
    SocketBridge bridge;
    ServiceHarness h(4u, 8u, kMemBudget, tmp.dir.c_str());
    bridge.start(h.svc);
    A840ScClient *client = make_client(bridge, 30u, 90u);

    constexpr unsigned kThreads = 6u;
    constexpr unsigned kOpsPerThread = 24u;
    constexpr unsigned kKeySpace = 8u;

    std::atomic<uint64_t> next_id{100000u + uint64_t(round) * 10000u};
    std::atomic<uint64_t> direct_accepted{0};
    std::atomic<uint64_t> direct_completed{0};
    std::atomic<uint64_t> async_accepted{0};
    std::atomic<uint64_t> async_completed{0};
    std::mutex done_mu;
    std::condition_variable done_cv;

    struct CountCtx {
        std::atomic<uint64_t> *counter;
        std::mutex *mu;
        std::condition_variable *cv;
    };
    CountCtx direct_ctx{&direct_completed, &done_mu, &done_cv};
    CountCtx async_ctx{&async_completed, &done_mu, &done_cv};

    auto direct_cb = [](void *ctx, const A840ScFrameHeader *, const uint8_t *, size_t) {
        auto *c = static_cast<CountCtx *>(ctx);
        c->counter->fetch_add(1u, std::memory_order_release);
        std::lock_guard<std::mutex> lk(*c->mu);
        c->cv->notify_all();
    };
    auto async_cb = [](void *ctx, uint64_t, A840ScStatus, const uint8_t *, size_t) {
        auto *c = static_cast<CountCtx *>(ctx);
        c->counter->fetch_add(1u, std::memory_order_release);
        std::lock_guard<std::mutex> lk(*c->mu);
        c->cv->notify_all();
    };

    SpinBarrier start(kThreads);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (unsigned t = 0u; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            start.wait();
            for (unsigned i = 0u; i < kOpsPerThread; ++i) {
                uint32_t tag = 1u + ((round * 13u + t * 5u + i) % kKeySpace);
                uint64_t id = next_id.fetch_add(1u, std::memory_order_relaxed);
                uint8_t marker = static_cast<uint8_t>(0x10u + tag);
                unsigned mode = (i + t) % 5u;

                if (mode == 0u || mode == 1u) {
                    A840ScPriority prio = (mode == 0u) ? A840_SC_PRIO_FG_SYNC
                                                       : ((i % 2u) ? A840_SC_PRIO_BG_ASYNC
                                                                   : A840_SC_PRIO_WARMUP_LOW);
                    auto hdr = make_req_hdr(tag, id, prio, 1u, kStackKey, kClientPid + t);
                    A840ScStatus st = a840_sc_service_submit(h.svc, &hdr, &marker,
                        direct_cb, &direct_ctx);
                    if (st == A840_SC_OK) {
                        direct_accepted.fetch_add(1u, std::memory_order_release);
                        if ((i % 7u) == 0u) {
                            A840PipelineKey k = make_key(tag);
                            (void)a840_sc_service_cancel(h.svc, &k, kClientPid + t, id);
                        }
                    } else {
                        CHECK(st == A840_SC_SHUTDOWN);
                    }
                } else if (mode == 2u) {
                    A840PipelineKey k = make_key(tag);
                    uint8_t *blob = nullptr;
                    size_t len = 0u;
                    A840ScStatus st = a840_sc_client_compile_sync(client, &k, &marker, 1u,
                        1500u, &blob, &len);
                    if (st == A840_SC_OK) {
                        CHECK(blob != nullptr && len > 0u);
                        std::free(blob);
                    } else {
                        CHECK(st == A840_SC_SHUTDOWN || st == A840_SC_TIMED_OUT ||
                              st == A840_SC_CANCELED || st == A840_SC_DAEMON_UNAVAILABLE);
                    }
                } else if (mode == 3u) {
                    A840PipelineKey k = make_key(tag);
                    uint64_t cid = 0u;
                    A840ScStatus st = a840_sc_client_compile_async(client, &k, &marker, 1u,
                        1500u, async_cb, &async_ctx, &cid);
                    if (st == A840_SC_OK) {
                        async_accepted.fetch_add(1u, std::memory_order_release);
                        if ((i % 4u) == 0u)
                            (void)a840_sc_client_cancel(client, cid);
                    } else {
                        CHECK(st == A840_SC_SHUTDOWN || st == A840_SC_DAEMON_UNAVAILABLE);
                    }
                } else {
                    A840ScServiceStats snap{};
                    a840_sc_service_get_stats(h.svc, &snap);
                    (void)a840_sc_client_ping(client, 500u);
                }
            }
        });
    }

    for (auto &w : workers)
        w.join();

    a840_sc_client_destroy(client);
    bridge.stop();
    if ((round % 2u) == 1u) {
        std::thread sh1([&] { a840_sc_service_shutdown(h.svc); });
        std::thread sh2([&] { a840_sc_service_shutdown(h.svc); });
        sh1.join();
        sh2.join();
    } else {
        a840_sc_service_shutdown(h.svc);
    }

    {
        std::unique_lock<std::mutex> lk(done_mu);
        CHECK(done_cv.wait_for(lk, kWaitLimit, [&] {
            return direct_completed.load(std::memory_order_acquire) ==
                       direct_accepted.load(std::memory_order_acquire) &&
                   async_completed.load(std::memory_order_acquire) ==
                       async_accepted.load(std::memory_order_acquire);
        }));
    }
}

} /* namespace */

int main(int argc, char **argv)
{
    unsigned tsan_rounds = 1u;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            tsan_rounds = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
            CHECK(tsan_rounds >= 1u);
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    run_protocol_tests();
    run_service_tests();
    run_client_tests();

    run_case("tsan: concurrent FG/BG/Cancel/Shutdown stress", [&] {
        for (unsigned r = 0u; r < tsan_rounds; ++r)
            run_tsan_stress_round(r);
    });

    CHECK(g_cases_passed >= 45u);
    std::printf("PASS test_shader_compiler_service: %u cases, %llu checks (stress_rounds=%u)\n",
        g_cases_passed, static_cast<unsigned long long>(g_assertions.load()), tsan_rounds);
    return 0;
}
