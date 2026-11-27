/* SPDX-License-Identifier: MIT */
#include "a840_render_pass_gmem.h"
#include "gen/a840_bitfields.h"
#include "gen/a840_gmem_bitfields.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::abort(); } } while (0)
static unsigned cases;
static constexpr uint32_t rw = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE;
struct Device {
    A840MockTransport *mock{};
    A840RtGmemQueue q{};
    std::vector<A840RtBuffer*> allocations;
    Device() {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg); cfg.auto_complete_submits = false;
        CHECK(a840_mock_transport_create(&cfg,&mock) == A840_TRANSPORT_OK);
        CHECK(a840_rt_gmem_open(&q,a840_mock_transport_interface(mock)) == A840_RT_OK);
    }
    A840RtBuffer *buffer(uint64_t bytes,uint32_t usage) {
        A840RtBufferDesc desc{}; desc.size_bytes = bytes; desc.usage_flags = usage;
        A840RtBuffer *b{};
        CHECK(a840_rt_buffer_create(q.device,&desc,&b) == A840_RT_OK); allocations.push_back(b);
        return b;
    }
    ~Device() {
        for (auto b : allocations) a840_rt_buffer_release(b);
        a840_rt_gmem_close(&q);
        CHECK(a840_mock_transport_live_queue_count(mock) == 0);
        a840_mock_transport_destroy(mock);
    }
};

static A840GmemRange range(A840RtBuffer *b)
{ return {b->bo_handle,b->gpu_va,b->size_bytes}; }

struct Frame {
    Device &dev;
    A840RtBuffer *ib;
    std::vector<A840RtBuffer*> resources;
    A840RtGmemPass p{};
    std::array<A840Reloc,128> scratch{};
    A840RtGmemFlight flight{};
    explicit Frame(Device &d):dev(d),ib(d.buffer(32768,A840_BUF_USAGE_READ|A840_BUF_USAGE_CPU_VISIBLE)) {
        auto image = d.buffer(4864u*2608,rw);
        auto flags = d.buffer(86016,rw);
        resources = {image,flags};
        p.desc.width = 1200; p.desc.height = 2608;
        p.desc.attachments[0] = {4,1,A840_LOAD_CLEAR,A840_STORE_STORE,false,true};
        p.desc.attachments[8] = {4,1,A840_LOAD_CLEAR,A840_STORE_DONT_CARE,true,false};
        p.bindings[0].format = A840_FMT6_8_8_8_8_UNORM;
        p.bindings[0].clear[0] = 4278190335u;
        p.bindings[0].image = {range(image),range(flags),4864,128,86016,0,true};
        p.bindings[8].format = A840_FMT6_32_FLOAT;
        p.bindings[8].clear[0] = 1065353216u;
    }
    A840RtStatus submit(size_t capacity = 128) {
        return a840_rt_gmem_submit(&dev.q,&p,ib,scratch.data(),capacity,
                                   resources.data(),resources.size(),&flight);
    }
    void done() {
        a840_mock_transport_complete_all(dev.mock);
        CHECK(a840_rt_gmem_retire(&flight,1000000) == A840_RT_OK);
    }
};

static std::vector<A840PM4Packet> decode(const A840MockSubmissionView &v)
{
    std::vector<A840PM4Packet> out(v.dword_count); size_t n = 0;
    CHECK(a840_pm4_decode(v.words,v.dword_count,out.data(),out.size(),&n) == A840_EMIT_OK);
    out.resize(n); return out;
}

static void main_frame(const char *dump)
{
    ++cases;
    Device d; Frame f(d);
    CHECK(f.submit() == A840_RT_OK);
    CHECK(a840_rt_cb_state(f.flight.cb) == A840_CB_SCHEDULED);
    CHECK(a840_rt_cb_submit_fence(f.flight.cb) != 0);
    CHECK(a840_rt_queue_pending_count(d.q.queue) == 1 && a840_mock_transport_submission_count(d.mock) == 1);
    CHECK(f.ib->refcount == 2 && f.resources[0]->refcount == 2);
    CHECK(f.submit() == A840_RT_STATE_ERROR);
    CHECK(a840_rt_gmem_retire(&f.flight,0) == A840_RT_TIMEOUT && f.flight.cb);
    A840MockSubmissionView v{};
    CHECK(a840_mock_transport_get_submission(d.mock,0,&v) == A840_TRANSPORT_OK);
    CHECK(v.ib_count == 2 && v.queue == a840_rt_queue_handle(d.q.queue));
    auto packets = decode(v);
    auto last = packets.back();
    CHECK(last.type == 7 && last.identifier == A840_CP_EVENT_WRITE7 && last.payload_words == 5);
    CHECK((v.words[last.header_word+1] & UINT8_MAX) == A840_EVENT_RB_DONE_TS);
    CHECK((v.words[last.header_word+1] & A840_EVENT_WRITE_ENABLED) != 0);
    CHECK(v.words[last.header_word+4] == a840_rt_cb_completion_marker(f.flight.cb));
    A840GmemPlan plan{};
    CHECK(a840_gmem_plan(&f.p.desc,&plan) == A840_EMIT_OK);
    auto nr = plan.tiles_x*plan.tiles_y*2+1;
    CHECK(v.reloc_count == nr);
    CHECK(f.scratch[nr-1].dword_offset == last.header_word+2);
    for (size_t i = 0; i < nr; ++i) {
        auto r = f.scratch[i]; size_t at = r.dword_offset;
        CHECK(at+1 < v.dword_count && r.flags == A840_RELOC_WRITE);
        CHECK((uint64_t(v.words[at+1])<<32 | v.words[at]) == r.gpu_va);
        if (i+1 < nr) CHECK(r.bo_handle == f.resources[i%2]->bo_handle);
    }
    if (dump) CHECK(a840_mock_transport_dump_pm4(d.mock,dump) == A840_TRANSPORT_OK);
    std::printf("Mock submit: %u IBs, %zu dwords, %u relocs, RB_DONE_TS fence=%llu\n",
        v.ib_count,v.dword_count,v.reloc_count,(unsigned long long)v.fence);
    f.done(); CHECK(f.ib->refcount == 1 && f.resources[0]->refcount == 1 && !f.flight.cb);
    CHECK(a840_rt_gmem_retire(&f.flight,0) == A840_RT_STATE_ERROR);
}

static void invalid_frames()
{
    Device d, other; Frame f(d);
    auto foreign = other.buffer(4096,rw);
    auto small_ib = d.buffer(12,A840_BUF_USAGE_READ|A840_BUF_USAGE_CPU_VISIBLE);
    auto read_only = d.buffer(4864u*2608,A840_BUF_USAGE_READ);
    auto original = f.p; auto resources = f.resources; auto ib = f.ib;
    for (unsigned k = 0; k < 26; ++k) {
        ++cases;
        f.p = original; f.resources = resources; f.ib = ib;
        auto &s = f.p.bindings[0].image; size_t cap = f.scratch.size();
        switch (k) {
        case 0: cap = 0; break;
        case 1: cap = 1; break;
        case 2: cap = 8; break; /* all stores fit, completion reloc does not */
        case 3: f.ib = foreign; break;
        case 4: f.resources[1] = foreign; break;
        case 5: f.resources[1] = ib; break;
        case 6: f.resources.clear(); break;
        case 7: f.resources.pop_back(); break;
        case 8: s.data.va--; break;
        case 9: s.data.bytes++; break;
        case 10: s.flags.bytes++; break;
        case 11: s.data.bo = 0; break;
        case 12: s.flags.va += 4; break;
        case 13: s.data = range(read_only); f.resources[0] = read_only; break;
        case 14: f.ib = small_ib; break;
        case 15: f.p.desc.width = 0; break;
        case 16: f.p.desc.attachments[0].samples = 3; break;
        case 17: f.p.bindings[8].format = 0; break;
        case 18: f.p.desc.draw_count = 1; break;
        case 19: f.p.tile_draws = range(resources[0]); break;
        case 20: f.p.bin_draws = range(resources[0]); break;
        case 21: f.p.desc.attachments[0].store = A840_STORE_RESOLVE; break;
        case 22: s.flag_pitch = 8; break;
        case 23: s.flag_array_pitch = 8192; break;
        case 24: s.tiled = false; break;
        case 25: f.p.desc.attachments[8].cpp = UINT32_MAX; break;
        }
        CHECK(f.submit(cap) != A840_RT_OK && !f.flight.cb);
        CHECK(a840_mock_transport_submission_count(d.mock) == 0 && a840_rt_queue_pending_count(d.q.queue) == 0);
    }
    f.p = original; f.resources = resources; f.ib = ib;
    CHECK(f.submit() == A840_RT_OK); f.done();
}

static void load_store_cases()
{
    Device d; Frame f(d);
    for (bool mem : {true,false}) for (auto load : {A840_LOAD_LOAD,A840_LOAD_CLEAR,A840_LOAD_DONT_CARE})
    for (auto store : {A840_STORE_STORE,A840_STORE_DONT_CARE}) {
        ++cases;
        f.p.desc.attachments[0] = {4,1,load,store,mem,true};
        A840GmemPlan p{}; CHECK(a840_gmem_plan(&f.p.desc,&p) == A840_EMIT_OK);
        CHECK(f.submit() == A840_RT_OK);
        A840MockSubmissionView v{}; size_t idx = a840_mock_transport_submission_count(d.mock)-1;
        CHECK(a840_mock_transport_get_submission(d.mock,idx,&v) == A840_TRANSPORT_OK);
        uint32_t transfers = !mem ? uint32_t(load == A840_LOAD_LOAD)+uint32_t(store == A840_STORE_STORE) : 0;
        CHECK(v.reloc_count == 1+2*p.tiles_x*p.tiles_y*transfers);
        f.done();
    }
}

/* Check real draw PM4 in the nested IB and the A8xx primary's per-pipe
 * addresses, mode markers and per-tile IB dispatch. The mock records IB1. */
static void visibility_case()
{
    ++cases;
    Device d; Frame f(d);
    A840GmemVsc vsc{};
    auto vbuf = [&](size_t bytes) { auto b = d.buffer(bytes,rw); f.resources.push_back(b); return range(b); };
    vsc.draw_stride = 1024; vsc.primitive_stride = 2048;
    vsc.draw = vbuf(32*vsc.draw_stride); vsc.primitive = vbuf(32*vsc.primitive_stride); vsc.sizes = vbuf(128);
    f.p.vsc = &vsc; f.p.desc.draw_count = 2;
    for (bool visibility : {false,true}) {
        auto b = d.buffer(4096,A840_BUF_USAGE_CPU_VISIBLE|A840_BUF_USAGE_READ);
        f.resources.push_back(b); A840CmdStream cs{};
        CHECK(a840_cs_init(&cs,static_cast<uint32_t*>(b->cpu_ptr),1024,nullptr,0) == A840_EMIT_OK);
        A840DrawDesc draw{}; draw.primitive = A840_PRIM_TRIANGLES; draw.instance_count = 1;
        draw.count = 3; draw.use_visibility = visibility;
        for (unsigned i = 0; i < f.p.desc.draw_count; ++i) CHECK(a840_emit_3d_draw(&cs,&draw) == A840_EMIT_OK);
        auto r = range(b); r.bytes = cs.used_dwords*4;
        (visibility ? f.p.tile_draws : f.p.bin_draws) = r;
    }
    CHECK(f.submit() == A840_RT_OK);
    A840MockSubmissionView v{}; CHECK(a840_mock_transport_get_submission(d.mock,0,&v) == A840_TRANSPORT_OK);
    A840GmemPlan p{}; CHECK(a840_gmem_plan(&f.p.desc,&p) == A840_EMIT_OK);
    unsigned indirect = 0, masks = 0, pseudo = 0;
    for (auto pkt : decode(v)) {
        if (pkt.type != 7) continue;
        auto data = v.words+pkt.header_word+1;
        if (pkt.identifier == A840_CP_INDIRECT_BUFFER) {
            auto ib = indirect++ ? f.p.tile_draws : f.p.bin_draws;
            CHECK((uint64_t(data[1])<<32 | data[0]) == ib.va && data[2] == ib.bytes/4);
        }
        if (pkt.identifier == A840_CP_SET_PSEUDO_REG) {
            ++pseudo; CHECK(pkt.payload_words == 9 && data[0] == GM_DRAW_STREAM && data[6] == GM_PRIM_STREAM);
        }
        if (pkt.identifier == A840_CP_SET_BIN_DATA5_OFFSET) {
            A840GmemTile t{}; CHECK(a840_gmem_tile(&p,masks%p.tiles_x,masks/p.tiles_x,&t) == A840_EMIT_OK);
            CHECK(pkt.payload_words == 5 && data[1] == (1u<<t.slot) && data[2] == t.pipe*vsc.draw_stride);
            CHECK(data[3] == t.pipe*4 && data[4] == t.pipe*vsc.primitive_stride); ++masks;
        }
    }
    CHECK(pseudo == 1 && masks == p.tiles_x*p.tiles_y && indirect == masks+1); f.done();
}

int main(int argc,char **argv)
{
    CHECK(argc == 1 || (argc == 3 && !std::strcmp(argv[1],"--dump-rp-pm4")));
    main_frame(argc == 3 ? argv[2] : nullptr); invalid_frames(); load_store_cases(); visibility_case();
    std::printf("PASS test_render_pass_gmem: %u cases\n",cases);
}
