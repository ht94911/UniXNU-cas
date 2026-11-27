/* SPDX-License-Identifier: MIT */
#include "a840_emit_gmem.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"
#include "gen/a840_gmem_bitfields.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::abort(); } } while (0)
static unsigned cases;
struct Stream {
    std::array<uint32_t,4096> words{};
    std::array<A840Reloc,128> relocs{};
    A840CmdStream cs{};
    Stream() { CHECK(a840_cs_init(&cs,words.data(),words.size(),relocs.data(),relocs.size()) == A840_EMIT_OK); }
    std::vector<A840PM4Packet> packets() const {
        std::vector<A840PM4Packet> out(cs.used_dwords);
        size_t n = 0;
        CHECK(a840_pm4_decode(words.data(),cs.used_dwords,out.data(),out.size(),&n) == A840_EMIT_OK);
        out.resize(n);
        return out;
    }
    std::vector<uint32_t> writes(uint32_t reg) const {
        std::vector<uint32_t> out;
        for (auto p : packets())
            if (p.type == 4 && reg >= p.identifier && reg-p.identifier < p.payload_words)
                out.push_back(words[p.header_word+1+reg-p.identifier]);
        return out;
    }
    unsigned op_count(uint32_t op) const {
        unsigned n = 0;
        for (auto p : packets()) n += p.type == 7 && p.identifier == op;
        return n;
    }
};

using Bindings = std::array<A840GmemBinding,A840_GMEM_PLANES>;
static A840GmemPass screen()
{
    A840GmemPass d{};
    d.width = 1200; d.height = 2608;
    d.attachments[0] = {4,1,A840_LOAD_CLEAR,A840_STORE_STORE,false,true};
    d.attachments[8] = {4,1,A840_LOAD_CLEAR,A840_STORE_DONT_CARE,true,false};
    return d;
}
static Bindings bindings()
{
    Bindings b{};
    b[0].format = A840_FMT6_8_8_8_8_UNORM;
    b[0].clear[0] = 4278190335u;
    auto &s = b[0].image;
    s.tiled = true; s.pitch = 4864; s.flag_pitch = 128; s.flag_array_pitch = 86016;
    s.data = {1,UINT64_C(1)<<32,uint64_t(s.pitch)*2608};
    s.flags = {2,UINT64_C(2)<<32,s.flag_array_pitch};
    b[8].format = A840_FMT6_32_FLOAT; b[8].clear[0] = 1065353216;
    return b;
}
static A840GmemPlan plan(const A840GmemPass &d)
{
    A840GmemPlan p{};
    CHECK(a840_gmem_plan(&d,&p) == A840_EMIT_OK);
    return p;
}

static void planner_tests()
{
    for (uint32_t n : {1u,3u,8u}) for (uint32_t cpp : {1u,2u,4u,16u})
    for (uint32_t samples : {1u,2u,4u}) {
        ++cases;
        auto d = screen();
        for (unsigned i = 0; i < n; ++i) d.attachments[i] = {cpp,samples,A840_LOAD_CLEAR,A840_STORE_STORE,false,false};
        auto p = plan(d);
        CHECK(a840_gmem_validate(&p) == A840_EMIT_OK);
        CHECK(p.attachment_budget == 16564224 && p.ccu_budget == 17203200);
        CHECK(p.tile_w % 96 == 0 && p.tile_h % 32 == 0 && p.used_bytes <= p.attachment_budget);
        /* Independent exhaustive oracle, including the inter-plane gaps. */
        uint32_t best = 0;
        uint32_t step = cpp*samples == 1 ? 192 : 96;
        for (uint32_t w = step; w < d.width+step; w += step)
            for (uint32_t h = 32; h < d.height+32; h += 32) {
                uint64_t end = 0;
                for (unsigned i = 0; i < A840_GMEM_PLANES; ++i) {
                    auto a = d.attachments[i];
                    if (!a.cpp) continue;
                    uint32_t align = (i >= 8 || a.samples > 1) ? 16384 : 4096;
                    end = (end+align-1)/align*align + uint64_t(w)*h*a.cpp*a.samples;
                }
                if (end <= p.attachment_budget) best = std::max(best,w*h);
            }
        CHECK(p.tile_w*p.tile_h == best);
        uint64_t pixels = 0;
        for (uint32_t y = 0; y < p.tiles_y; ++y) for (uint32_t x = 0; x < p.tiles_x; ++x) {
            A840GmemTile t{};
            CHECK(a840_gmem_tile(&p,x,y,&t) == A840_EMIT_OK && t.pipe < 32);
            pixels += uint64_t(t.width)*t.height;
            CHECK(t.x == x*p.tile_w && t.y == y*p.tile_h);
        }
        CHECK(pixels == uint64_t(d.width)*d.height);
    }
    for (unsigned k = 0; k < 12; ++k) {
        ++cases;
        auto d = screen();
        A840GmemPlan p{};
        p.used_bytes = 987;
        switch (k) {
        case 0: d.width = 0; break;
        case 1: d.height = 0; break;
        case 2: d.width = UINT32_MAX; break;
        case 3: d.height = UINT32_MAX; break;
        case 4: d.covered_pixels = UINT64_MAX; break;
        case 5: d.attachments[0].samples = 0; break;
        case 6: d.attachments[0].samples = 3; break;
        case 7: d.attachments[0].samples = 8; break;
        case 8: d.attachments[0].store = A840_STORE_RESOLVE; break;
        case 9: d.attachments[0] = {}; d.attachments[8] = {}; break;
        case 10: d.attachments[0].cpp = UINT32_MAX; break;
        case 11: d.attachments[8].cpp = 6000; break;
        }
        CHECK(a840_gmem_plan(&d,&p) != A840_EMIT_OK && p.used_bytes == 987);
    }
    for (unsigned k = 0; k < 8; ++k) {
        ++cases;
        A840GmemPass d{};
        d.width = 96; d.height = 32; d.draw_count = 1;
        auto &a = d.attachments[0];
        a = {4,1,A840_LOAD_LOAD,A840_STORE_STORE,false,false};
        switch (k) {
        case 1: d.uses_fbfetch = true; break;
        case 2: a.memoryless = true; break;
        case 3: d.has_msaa_resolve = true; break;
        case 4: a.store = A840_STORE_DONT_CARE; break;
        case 5: d.draw_count = 2; break;
        case 6: a.load = A840_LOAD_CLEAR; break;
        case 7: a.cpp = UINT32_MAX; d.draw_count = 2; break;
        }
        CHECK(plan(d).path == ((k == 0 || k == 7) ? A840_PATH_SYSMEM : A840_PATH_GMEM));
    }
}

static void op_matrix()
{
    for (bool mem : {false,true}) for (auto load : {A840_LOAD_LOAD,A840_LOAD_CLEAR,A840_LOAD_DONT_CARE})
    for (auto store : {A840_STORE_STORE,A840_STORE_DONT_CARE,A840_STORE_RESOLVE}) {
        ++cases;
        auto d = screen(); d.attachments[8] = {};
        d.attachments[0] = {4,store == A840_STORE_RESOLVE ? 4u : 1u,load,store,mem,true};
        auto b = bindings();
        b[0].resolve = b[0].image;
        b[0].resolve.data.bo = 3; b[0].resolve.flags.bo = 4;
        if (store == A840_STORE_RESOLVE) {
            b[0].image.pitch *= 4; b[0].image.data.bytes *= 4;
            b[0].image.flag_pitch *= 4; b[0].image.flag_array_pitch *= 4;
            b[0].image.flags.bytes *= 4;
        }
        auto p = plan(d);
        Stream clear, read, write;
        CHECK(a840_emit_gmem_tile_clear(&clear.cs,&p,b.data()) == A840_EMIT_OK);
        CHECK(clear.cs.reloc_count == 0 && clear.writes(A840_REG_RB_RESOLVE_SYSTEM_BUFFER_BASE).empty());
        CHECK(clear.writes(A840_REG_RB_RESOLVE_CLEAR_COLOR_DW0).size() == size_t(load == A840_LOAD_CLEAR));
        CHECK(a840_emit_gmem_tile_load(&read.cs,&p,b.data()) == A840_EMIT_OK);
        CHECK(a840_emit_gmem_tile_store(&write.cs,&p,b.data()) == A840_EMIT_OK);
        for (auto *s : {&read,&write}) {
            bool reading = s == &read;
            bool active = !mem && (reading ? load == A840_LOAD_LOAD : store != A840_STORE_DONT_CARE);
            CHECK(s->cs.reloc_count == (active ? 2u : 0u));
            CHECK(s->writes(A840_REG_RB_RESOLVE_SYSTEM_BUFFER_BASE).size() == size_t(active));
            CHECK(s->writes(A840_REG_RB_RESOLVE_SYSTEM_FLAG_BUFFER_BASE).size() == size_t(active));
            if (!active) { CHECK(s->writes(A840_REG_RB_RESOLVE_OPERATION).empty()); continue; }
            bool resolve = !reading && store == A840_STORE_RESOLVE;
            CHECK(s->relocs[0].bo_handle == (resolve ? 3u : 1u));
            CHECK(s->relocs[1].bo_handle == (resolve ? 4u : 2u));
            for (unsigned j = 0; j < 2; ++j) {
                auto r = s->relocs[j]; CHECK(r.flags == (reading ? A840_RELOC_READ : A840_RELOC_WRITE));
                CHECK((uint64_t(s->words[r.dword_offset+1])<<32 | s->words[r.dword_offset]) == r.gpu_va);
            }
            auto &surface = resolve ? b[0].resolve : b[0].image;
            CHECK(s->writes(A840_REG_RB_RESOLVE_SYSTEM_FLAG_BUFFER_PITCH).at(0) ==
                ((surface.flag_pitch/64) | ((surface.flag_array_pitch/512)<<11)));
            CHECK((s->writes(A840_REG_RB_RESOLVE_SYSTEM_BUFFER_INFO).at(0) & GM_FLAGS) != 0);
            CHECK((s->writes(A840_REG_RB_RESOLVE_OPERATION).at(0) & 3u) == (reading ? GM_LOAD : GM_STORE));
        }
    }
}

static void wide_resolves()
{
    for (auto fmt : {A840_FMT6_16_16_16_16_FLOAT,A840_FMT6_32_FLOAT,
                     A840_FMT6_8_8_8_8_SNORM,A840_FMT6_32_UINT}) {
        ++cases;
        auto d = screen();
        uint32_t cpp = fmt == A840_FMT6_16_16_16_16_FLOAT ? 8 : 4;
        d.attachments[0] = {cpp,4,A840_LOAD_CLEAR,A840_STORE_RESOLVE,false,true};
        auto p = plan(d); auto b = bindings();
        b[0].format = fmt;
        b[0].resolve = b[0].image;
        b[0].resolve.pitch = 1216*cpp;
        b[0].resolve.data.bytes = uint64_t(b[0].resolve.pitch)*2608;
        b[0].resolve.flag_pitch = 256;
        b[0].resolve.flags.bytes = b[0].resolve.flag_array_pitch = 172032;
        Stream s;
        CHECK(a840_emit_gmem_tile_store(&s.cs,&p,b.data()) == A840_EMIT_OK);
        CHECK(s.op_count(A840_CP_BLIT) == 1 && s.op_count(A840_CP_WAIT_FOR_IDLE) == 1);
        CHECK(s.writes(A840_REG_RB_RESOLVE_OPERATION).empty());
        CHECK(s.writes(A840_REG_TPL1_A2D_SRC_TEXTURE_BASE).at(0) == p.planes[0].offset);
        uint32_t info = s.writes(A840_REG_TPL1_A2D_SRC_TEXTURE_INFO).at(0);
        CHECK(((info >> 8) & 3) == 2 && ((info >> 14) & 3) == 2);
        CHECK(((info >> 18) & 1) == unsigned(fmt != A840_FMT6_32_UINT));
        CHECK(s.writes(A840_REG_RB_A2D_DEST_FLAG_BUFFER_PITCH).at(0) == (4u | (336u<<11)));
        CHECK(s.cs.reloc_count == 2 && s.relocs[0].bo_handle == 1 && s.relocs[1].bo_handle == 2);
    }
}

static void rejects()
{
    for (unsigned k = 0; k < 14; ++k) {
        ++cases;
        auto p = plan(screen()); auto b = bindings(); auto &s = b[0].image;
        Stream out;
        switch (k) {
        case 0: s.data.va++; break;
        case 1: s.data.bytes--; break;
        case 2: s.data.va = UINT64_C(1)<<48; break;
        case 3: s.flags.va = 0; break;
        case 4: s.flags.bytes--; break;
        case 5: s.flags.va = s.data.va; break;
        case 6: s.flag_pitch = 64; break;
        case 7: s.flag_array_pitch = 4096; break;
        case 8: s.tiled = false; break;
        case 9: s.pitch--; break;
        case 10: p.planes[0].offset += 4096; break;
        case 11: out.cs.reloc_capacity = 1; break;
        case 12: out.cs.capacity_dwords = 5; break;
        case 13: b[0].format = A840_FMT6_8_UINT; break;
        }
        out.words.fill(899); out.relocs[0].bo_handle = 775;
        CHECK(a840_emit_gmem_tile_store(&out.cs,&p,b.data()) != A840_EMIT_OK);
        CHECK(out.cs.used_dwords == 0 && out.cs.reloc_count == 0 && out.relocs[0].bo_handle == 775);
        CHECK(std::all_of(out.words.begin(),out.words.end(),[](uint32_t x){ return x == 899; }));
    }
}

static void vsc_edges()
{
    ++cases;
    auto d = screen();
    d.width = 15001; d.height = 14003;
    for (unsigned i = 0; i < 8; ++i)
        d.attachments[i] = {4,4,A840_LOAD_CLEAR,A840_STORE_DONT_CARE,false,false};
    auto p = plan(d);
    CHECK(p.tiles_x*p.tiles_y > 32 && p.pipe_w*p.pipe_h > 1);
    A840GmemVsc v{};
    v.draw_stride = 512; v.primitive_stride = 1024;
    v.draw = {1,UINT64_C(1)<<29,32*v.draw_stride};
    v.primitive = {2,UINT64_C(1)<<30,32*v.primitive_stride};
    v.sizes = {3,UINT64_C(1)<<31,128};
    Stream begin;
    /* A grid larger than 32 slots per pipe cannot use the hardware VSC.
     * It still runs with unconditional tile replay. */
    CHECK(p.pipe_w*p.pipe_h > 32);
    CHECK(a840_emit_gmem_binning_begin(&begin.cs,&p,&v) == A840_EMIT_UNSUPPORTED);
    CHECK(!begin.cs.used_dwords && !begin.cs.reloc_count);
    CHECK(a840_emit_gmem_binning_begin(&begin.cs,&p,nullptr) == A840_EMIT_OK);
    begin.cs.used_dwords = begin.cs.reloc_count = 0;
    d.width = 7001; d.height = 6003; p = plan(d);
    CHECK(p.tiles_x*p.tiles_y > 32 && p.pipe_w*p.pipe_h > 1 && p.pipe_w*p.pipe_h <= 32);
    CHECK(a840_emit_gmem_binning_begin(&begin.cs,&p,&v) == A840_EMIT_OK);
    for (uint32_t y = 0; y < p.tiles_y; ++y) for (uint32_t x = 0; x < p.tiles_x; ++x) {
        ++cases;
        A840GmemTile t{};
        CHECK(a840_gmem_tile(&p,x,y,&t) == A840_EMIT_OK);
        auto cfg = begin.writes(A840_REG_VSC_PIPE_CONFIG_0_REG+t.pipe).at(0);
        auto px = cfg & 1023, py = (cfg>>10)&1023, w = (cfg>>20)&63, h = cfg>>26;
        CHECK(px <= x && x < px+w && py <= y && y < py+h);
        CHECK(t.slot == (y-py)*w+(x-px) && t.slot < 32);
        Stream selected;
        CHECK(a840_emit_gmem_tile_select(&selected.cs,&p,x,y,&v) == A840_EMIT_OK);
        bool found = false;
        for (auto packet : selected.packets()) {
            if (packet.type != 7 || packet.identifier != A840_CP_SET_BIN_DATA5_OFFSET) continue;
            auto data = selected.words.data()+packet.header_word+1;
            CHECK(((data[0]>>16)&63) == w*h && ((data[0]>>22)&31) == t.slot);
            CHECK(data[1] == (1u<<t.slot)); found = true;
        }
        CHECK(found);
        uint32_t right = t.x+t.width, bottom = t.y+t.height;
        CHECK(selected.writes(A840_REG_RB_RESOLVE_CNTL_2).at(0) ==
              (((right+15)/16*16-1) | (((bottom+3)/4*4-1)<<16)));
    }
}

static void frame(const char *dump)
{
    ++cases;
    auto p = plan(screen()); auto b = bindings(); Stream s;
    CHECK(a840_emit_gmem_binning_begin(&s.cs,&p,nullptr) == A840_EMIT_OK);
    CHECK(a840_emit_gmem_binning_end(&s.cs,nullptr) == A840_EMIT_OK);
    for (uint32_t y = 0; y < p.tiles_y; ++y) for (uint32_t x = 0; x < p.tiles_x; ++x) {
        CHECK(a840_emit_gmem_tile_select(&s.cs,&p,x,y,nullptr) == A840_EMIT_OK);
        CHECK(a840_emit_gmem_tile_load(&s.cs,&p,b.data()) == A840_EMIT_OK);
        CHECK(a840_emit_gmem_tile_clear(&s.cs,&p,b.data()) == A840_EMIT_OK);
        CHECK(a840_emit_gmem_tile_store(&s.cs,&p,b.data()) == A840_EMIT_OK);
        if (x+1 < p.tiles_x || y+1 < p.tiles_y) CHECK(a840_emit_gmem_tile_end(&s.cs) == A840_EMIT_OK);
    }
    CHECK(a840_emit_gmem_pass_end(&s.cs) == A840_EMIT_OK);
    uint32_t tiles = p.tiles_x*p.tiles_y;
    CHECK(tiles > 1 && s.cs.reloc_count == 2*tiles && s.op_count(A840_CP_WAIT_FOR_IDLE) == tiles);
    CHECK(s.writes(A840_REG_RB_CCU_CACHE_CNTL) == (std::vector<uint32_t>{265081164,3469742064u}));
    for (uint32_t reg : {A840_REG_TPL1_BIN_SIZE,A840_REG_TPL1_A2D_BIN_SIZE,A840_REG_SP_BIN_SIZE})
        CHECK(s.writes(reg) == std::vector<uint32_t>(tiles+1,(p.tile_w/32) | ((p.tile_h/16)<<16)));
    for (unsigned i = 0; i < 10; ++i) {
        uint32_t reg = i < 8 ? A840_REG_RB_MRT_GMEM_DIMENSION_0_REG+i :
            (i == 8 ? A840_REG_RB_DEPTH_GMEM_DIMENSION : A840_REG_RB_STENCIL_GMEM_DIMENSION);
        CHECK(s.writes(reg) == std::vector<uint32_t>(tiles+1,p.tile_w | (p.tile_h<<16)));
    }
    for (auto reg : {A840_REG_RB_DBG_ECO_CNTL,A840_REG_RB_RBP_CNTL,A840_REG_RB_GC_GMEM_PROTECT,
                     A840_REG_SP_NC_MODE_CNTL_2,A840_REG_VPC_LB_MODE_CNTL,A840_REG_PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE})
        CHECK(s.writes(reg).empty());
    CHECK(s.writes(A840_REG_GRAS_SC_WINDOW_SCISSOR_BR).back() == (1199u | (2607u<<16)));
    CHECK(s.writes(A840_REG_RB_DEPTH_GMEM_BASE).back() == p.planes[8].offset);
    std::printf("1200x2608 tile=%ux%u grid=%ux%u used=%u dwords=%zu relocs=%zu\n",
        p.tile_w,p.tile_h,p.tiles_x,p.tiles_y,p.used_bytes,s.cs.used_dwords,s.cs.reloc_count);
    if (dump) {
        FILE *f = std::fopen(dump,"wb"); CHECK(f);
        CHECK(std::fwrite(s.words.data(),4,s.cs.used_dwords,f) == s.cs.used_dwords); CHECK(!std::fclose(f));
    }
}

int main(int argc,char **argv)
{
    CHECK(argc == 1 || (argc == 3 && !std::strcmp(argv[1],"--dump-gmem-pm4")));
    planner_tests(); op_matrix(); wide_resolves(); rejects(); vsc_edges(); frame(argc == 3 ? argv[2] : nullptr);
    std::printf("PASS test_emit_gmem: %u cases\n",cases);
}
