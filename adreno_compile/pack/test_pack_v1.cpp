/* SPDX-License-Identifier: MIT */
#include "a840_pack_v1.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::size_t checks=0,cases=0;
const char *active="start";
void begin(const char *name){active=name;++cases;}
void check(bool value,const char *expr,int line)
{
    ++checks;
    if(!value){std::fprintf(stderr,"FAIL [%s] line%d: %s\n",active,line,expr);std::exit(EXIT_FAILURE);}
}
#define CASE(name) begin(name)
#define CHECK(x) check((x),#x,__LINE__)
constexpr std::uint64_t STACK=UINT64_C(0x1122334455667788);
struct Aligned {
    std::uint8_t *data=nullptr;
    std::size_t bytes=0;
    explicit Aligned(std::size_t size):bytes(size)
    {void *p=nullptr;CHECK(posix_memalign(&p,128,size)==0);data=static_cast<std::uint8_t *>(p);std::memset(data,0xa5,size);}
    ~Aligned(){std::free(data);}
    Aligned(const Aligned &)=delete;
    Aligned &operator=(const Aligned &)=delete;
};
struct Fixture {
    A840PipelineRecord record{};
    A840BindingPlan plan{};
    std::array<std::array<std::uint32_t,8>,2> isa{};
    std::array<std::array<std::uint32_t,4>,2> constants{};
    std::array<std::array<std::uint32_t,4>,2> sds{};
    void setup(unsigned id,bool render=false)
    {
        std::memset(&record,0,sizeof(record));std::memset(&plan,0,sizeof(plan));
        record.abi_fingerprint=a840_compile_abi_fingerprint();record.stack_key=STACK;
        record.chip_id=A840_PACK_V1_CHIP_ID;record.highest_bank_bit=16;
        record.stage_count=render?2:1;record.pipeline_key.sha256[0]=static_cast<std::uint8_t>(id);
        record.pipeline_key.sha256[31]=static_cast<std::uint8_t>(id+1);
        plan.stage_count=record.stage_count;
        for(unsigned i=0;i<record.stage_count;++i) {
            auto &s=record.stages[i];
            s.abi_version=1;s.stage=render?i:2;s.isa_words=isa[i].data();s.isa_dwords=8;s.instruction_groups=2;
            s.max_full_reg=7;s.max_half_reg=-1;s.constlen_vec4=4;s.const_data_words=constants[i].data();
            s.const_data_dwords=4;s.sds_state_words=sds[i].data();s.sds_state_dwords=4;s.max_waves=16;
            s.local_size[0]=1;s.local_size[1]=1;s.local_size[2]=1;
            isa[i].fill(0x12345670u+id+i);constants[i].fill(id+i);sds[i].fill(0x01020304u+id);
            plan.stages[i].stage=s.stage;plan.stages[i].const_bytes=64;
        }
    }
};
struct Builder {
    std::array<A840PackV1BuilderItem,64> items{};
    std::array<std::uint32_t,1024> buckets{};
    A840PackV1Builder b{};
    Builder(std::uint32_t tier=0,std::size_t capacity=64)
    {CHECK(a840_pack_v1_builder_init(&b,tier,STACK,a840_compile_abi_fingerprint(),items.data(),capacity,buckets.data(),buckets.size())==A840_PACK_OK);}
    void add(Fixture &f){CHECK(a840_pack_v1_builder_add(&b,&f.record,&f.plan)==A840_PACK_OK);}
    std::size_t size(){std::size_t n=0;CHECK(a840_pack_v1_builder_measure(&b,&n)==A840_PACK_OK);return n;}
    void build(Aligned &out){std::size_t n=0;CHECK(a840_pack_v1_builder_build(&b,out.data,out.bytes,&n)==A840_PACK_OK);CHECK(n==out.bytes);}
};
std::uint32_t word(const std::uint8_t *p)
{return p[0]|std::uint32_t(p[1])<<8|std::uint32_t(p[2])<<16|std::uint32_t(p[3])<<24;}
void put(std::uint8_t *p,std::uint32_t n)
{for(unsigned i=0;i<4;++i)p[i]=static_cast<std::uint8_t>(n>>(8*i));}
void rehash(Aligned &buffer)
{
    std::memset(buffer.data+32,0,32);
    std::uint8_t digest[32];
    CHECK(a840_pack_v1_sha256(buffer.data,buffer.bytes,digest)==A840_PACK_OK);
    std::memcpy(buffer.data+32,digest,32);
}
std::string hex(const std::uint8_t *p,std::size_t size)
{
    static const char digits[]="0123456789abcdef";std::string out;
    for(std::size_t i=0;i<size;++i){out+=digits[p[i]>>4];out+=digits[p[i]&15];}return out;
}
A840PackV1View open(const Aligned &b)
{
    A840PackV1View view{};
    CHECK(a840_pack_v1_open_memory(b.data,b.bytes,STACK,a840_compile_abi_fingerprint(),&view)==A840_PACK_OK);
    return view;
}
void reject(const Aligned &b,std::uint64_t stack=STACK,std::uint64_t abi=a840_compile_abi_fingerprint())
{
    A840PackV1View view{};
    std::memset(&view,0xa5,sizeof(view));const auto before=view;
    CHECK(a840_pack_v1_open_memory(b.data,b.bytes,stack,abi,&view)!=A840_PACK_OK);
    CHECK(std::memcmp(&view,&before,sizeof(view))==0);
}
void test_hash()
{
    CASE("SHA256 known empty vector");
    std::uint8_t digest[32];
    CHECK(a840_pack_v1_sha256(nullptr,0,digest)==A840_PACK_OK);
    CHECK(hex(digest,32)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CASE("SHA256 known abc vector");
    CHECK(a840_pack_v1_sha256("abc",3,digest)==A840_PACK_OK);
    CHECK(hex(digest,32)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CASE("SHA256 padding boundary lengths");
    for(std::size_t n=1;n<130;++n) {
        std::array<std::uint8_t,130> p{};p.fill(7);
        CHECK(a840_pack_v1_sha256(p.data(),n,digest)==A840_PACK_OK);
    }
    CASE("SHA256 null and overflow rejection");
    CHECK(a840_pack_v1_sha256(nullptr,1,digest)==A840_PACK_INVALID_ARG);
    CHECK(a840_pack_v1_sha256("abc",3,nullptr)==A840_PACK_INVALID_ARG);
}
void test_container(const std::filesystem::path &directory)
{
    static_assert(sizeof(A840PackV1Header)==64 && offsetof(A840PackV1Header,file_sha256)==32,"fixed header");
    CASE("one compute pipeline build and zero-copy pointer views");
    Fixture a;a.setup(5);
    Builder single;single.add(a);Aligned one(single.size());single.build(one);
    auto view=open(one);A840PackV1Entry entry{};
    CHECK(view.entry_count==1 && view.tier==0 && view.hash_bucket_count>=2);
    CHECK(a840_pack_v1_lookup(&view,&a.record.pipeline_key,&entry)==A840_PACK_OK);
    CHECK(entry.pipeline_key->sha256[0]==5 && entry.stage_count==1 && entry.stages[0].stage==2);
    CHECK(reinterpret_cast<std::uintptr_t>(entry.stages[0].isa_words)%128==0);
    CHECK(reinterpret_cast<const std::uint8_t *>(entry.stages[0].isa_words)>=one.data &&
          reinterpret_cast<const std::uint8_t *>(entry.stages[0].isa_words)+32<=one.data+one.bytes);
    CHECK(std::memcmp(entry.stages[0].isa_words,a.isa[0].data(),32)==0);
    CHECK(std::memcmp(entry.stages[0].const_data_words,a.constants[0].data(),16)==0);
    CHECK(std::memcmp(entry.stages[0].sds_state_words,a.sds[0].data(),16)==0);
    CHECK(std::memcmp(entry.binding_plan,&a.plan,sizeof(a.plan))==0);
    CASE("cold writer record interop retains borrowed data without owner");
    A840PipelineRecord cold{};
    CHECK(a840_pack_v1_entry_record(&view,&entry,&cold)==A840_PACK_OK);
    CHECK(cold.owned_storage==nullptr && cold.stages[0].owner==nullptr &&
          cold.stages[0].isa_words==entry.stages[0].isa_words && cold.highest_bank_bit==16);
    CASE("unknown key leaves output untouched");
    auto unknown=a.record.pipeline_key;unknown.sha256[4]=99;
    const auto before=entry;
    CHECK(a840_pack_v1_lookup(&view,&unknown,&entry)==A840_PACK_NOT_FOUND);
    CHECK(std::memcmp(&entry,&before,sizeof(entry))==0);
    CASE("directory entry access bounds");
    CHECK(a840_pack_v1_entry_at(&view,0,&entry)==A840_PACK_OK);
    CHECK(a840_pack_v1_entry_at(&view,1,&entry)==A840_PACK_NOT_FOUND);
    CASE("null and alias lookup/open boundaries");
    CHECK(a840_pack_v1_lookup(nullptr,&unknown,&entry)==A840_PACK_INVALID_ARG);
    CHECK(a840_pack_v1_lookup(&view,nullptr,&entry)==A840_PACK_INVALID_ARG);
    CHECK(a840_pack_v1_lookup(&view,&unknown,nullptr)==A840_PACK_INVALID_ARG);
    CHECK(a840_pack_v1_open_memory(one.data,one.bytes,STACK,a840_compile_abi_fingerprint(),
          reinterpret_cast<A840PackV1View *>(one.data))==A840_PACK_ALIAS);
    CASE("strict stack identity and expected ABI");
    reject(one,STACK+1);reject(one,STACK,a840_compile_abi_fingerprint()+1);
    CASE("Tier1 is represented and authenticated");
    Builder tier1(1);tier1.add(a);Aligned second(tier1.size());tier1.build(second);
    CHECK(open(second).tier==1 && second.data[4]!=one.data[4]);
    CASE("ordered VS and FS stage payloads both 128-byte aligned");
    Fixture render;render.setup(6,true);Builder graphics;graphics.add(render);
    Aligned render_bytes(graphics.size());graphics.build(render_bytes);
    auto graphics_view=open(render_bytes);
    CHECK(a840_pack_v1_entry_at(&graphics_view,0,&entry)==A840_PACK_OK && entry.stage_count==2);
    CHECK(entry.stages[0].stage==0 && entry.stages[1].stage==1);
    for(auto &stage:entry.stages)CHECK(reinterpret_cast<std::uintptr_t>(stage.isa_words)%128==0);
    CASE("constant/SDS sections may be empty without ownership");
    Fixture empty_sections;empty_sections.setup(7);empty_sections.record.stages[0].const_data_dwords=0;
    empty_sections.record.stages[0].sds_state_dwords=0;Builder es;es.add(empty_sections);
    Aligned eb(es.size());es.build(eb);auto ev=open(eb);
    CHECK(a840_pack_v1_entry_at(&ev,0,&entry)==A840_PACK_OK &&
          entry.stages[0].const_data_words==nullptr && entry.stages[0].sds_state_words==nullptr);
    CASE("exact deterministic output independent of input order");
    std::array<Fixture,3> fixtures;for(unsigned i=0;i<3;++i)fixtures[i].setup(20+i,i==1);
    Builder first,last;for(auto &f:fixtures)first.add(f);
    for(auto it=fixtures.rbegin();it!=fixtures.rend();++it)last.add(*it);
    Aligned ordered(first.size()),reversed(last.size());first.build(ordered);last.build(reversed);
    CHECK(ordered.bytes==reversed.bytes && std::memcmp(ordered.data,reversed.data,ordered.bytes)==0);
    CASE("same-key equal pipeline is deduplicated");
    const auto count=first.b.count;first.add(fixtures[0]);CHECK(first.b.count==count);
    CASE("same-key unequal payload is rejected instead of order-dependent output");
    auto wrong=fixtures[0].record;wrong.flags=1;
    CHECK(a840_pack_v1_builder_add(&first.b,&wrong,&fixtures[0].plan)==A840_PACK_KEY_CONFLICT);
    CASE("empty Tier0 container remains valid and has no hit");
    Builder empty;Aligned empty_bytes(empty.size());empty.build(empty_bytes);
    auto empty_view=open(empty_bytes);CHECK(empty_view.entry_count==0);
    CHECK(a840_pack_v1_lookup(&empty_view,&unknown,&entry)==A840_PACK_NOT_FOUND);
    CASE("insufficient output capacity leaves buffer/output length unchanged");
    Aligned short_buffer(one.bytes);
    const std::vector<std::uint8_t> old(short_buffer.data,short_buffer.data+short_buffer.bytes);
    std::size_t output_bytes=777;
    CHECK(a840_pack_v1_builder_build(&single.b,short_buffer.data,one.bytes-1,&output_bytes)==A840_PACK_CAPACITY);
    CHECK(output_bytes==777 && std::equal(old.begin(),old.end(),short_buffer.data));
    CASE("builder rejects output alias with workspace");
    CHECK(a840_pack_v1_builder_build(&single.b,&single.items[0],sizeof(single.items),&output_bytes)!=A840_PACK_OK);

    for(unsigned mutation=0;mutation<30;++mutation) {
        CASE("corrupted/truncated/misaligned/forged metadata pack rejection");
        Aligned bad(one.bytes);std::memcpy(bad.data,one.data,one.bytes);
        const auto dir=view.directory_offset;
        const auto meta=word(bad.data+dir+40);
        const auto plan=word(bad.data+dir+52);
        switch(mutation) {
        case 0:bad.data[0]^=1;break;
        case 1:bad.data[4]=(bad.data[4]&~3u)|2u;break;
        case 2:bad.data[4]=static_cast<std::uint8_t>(1u|(15u<<3));break;
        case 3:bad.data[8]^=1;break;
        case 4:bad.data[16]^=1;break;
        case 5:bad.data[24]^=1;break;
        case 6:bad.data[28]^=1;break;
        case 7:bad.data[32]^=1;break;
        case 8:bad.data[64]^=1;break;
        case 9:bad.data[dir]^=1;break;
        case 10:bad.data[one.bytes-1]^=1;break;
        case 11:bad.data[5]=17;rehash(bad);break;
        case 12:bad.data[6]=3;rehash(bad);break;
        case 13:put(bad.data+64,99);rehash(bad);break;
        case 14:put(bad.data+dir+36,3);rehash(bad);break;
        case 15:put(bad.data+dir+40,meta+4);rehash(bad);break;
        case 16:put(bad.data+dir+44,4);rehash(bad);break;
        case 17:put(bad.data+dir+48,129);rehash(bad);break;
        case 18:put(bad.data+dir+52,plan+4);rehash(bad);break;
        case 19:put(bad.data+dir+56,64);rehash(bad);break;
        case 20:put(bad.data+dir+60,0);rehash(bad);break;
        case 21:put(bad.data+dir+64,0);rehash(bad);break;
        case 22:bad.data[dir+68]=1;rehash(bad);break;
        case 23:put(bad.data+meta,2);rehash(bad);break;
        case 24:put(bad.data+meta+4,0);rehash(bad);break;
        case 25:put(bad.data+meta+8,0);rehash(bad);break;
        case 26:put(bad.data+meta+15*4,17);rehash(bad);break;
        case 27:put(bad.data+meta+149*4,word(bad.data+meta+149*4)+4);rehash(bad);break;
        case 28:put(bad.data+plan+4,129);rehash(bad);break;
        case 29:put(bad.data+plan+12,2);rehash(bad);break;
        }
        reject(bad);
    }
    CASE("bounded open addressing grows a crowded hash and finds all keys");
    std::array<Fixture,20> collision;
    unsigned found=0;
    for(std::uint32_t n=1;found<collision.size();++n) {
        auto &f=collision[found];f.setup(80+found);
        std::memset(f.record.pipeline_key.sha256,0,32);
        put(f.record.pipeline_key.sha256,n);
        std::uint64_t h=UINT64_C(14695981039346656037);
        for(auto byte:f.record.pipeline_key.sha256){h^=byte;h*=UINT64_C(1099511628211);}
        if((h&63u)==0)++found;
    }
    Builder crowded;
    for(auto &f:collision)crowded.add(f);
    Aligned crowded_bytes(crowded.size());crowded.build(crowded_bytes);
    auto crowded_view=open(crowded_bytes);
    CHECK(crowded_view.entry_count==20 && crowded_view.hash_bucket_count>64);
    for(auto &f:collision)CHECK(a840_pack_v1_lookup(&crowded_view,&f.record.pipeline_key,&entry)==A840_PACK_OK);
    CASE("every short header length is rejected");
    for(std::size_t n=0;n<128;++n) {
        A840PackV1View wrong_view{};
        CHECK(a840_pack_v1_open_memory(one.data,n,STACK,a840_compile_abi_fingerprint(),&wrong_view)!=A840_PACK_OK);
    }
    CASE("truncated and appended images reject exact file length/hash");
    A840PackV1View wrong_view{};
    CHECK(a840_pack_v1_open_memory(one.data,one.bytes-1,STACK,a840_compile_abi_fingerprint(),&wrong_view)!=A840_PACK_OK);
    Aligned extra_bytes(one.bytes+128);std::memcpy(extra_bytes.data,one.data,one.bytes);
    reject(extra_bytes);
    CASE("128-byte base alignment is mandatory for real pointer ISA alignment");
    CHECK(a840_pack_v1_open_memory(one.data+1,one.bytes-1,STACK,a840_compile_abi_fingerprint(),&wrong_view)!=A840_PACK_OK);
    CASE("writer atomically saves beneath arbitrary cache subdirectory");
    const auto path=directory/"readonly.pack.bin";
    CHECK(a840_pack_v1_save_file(path.c_str(),one.data,one.bytes,STACK,a840_compile_abi_fingerprint())==A840_PACK_OK);
    const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);CHECK(fd>=0);
    void *mapped=mmap(nullptr,one.bytes,PROT_READ,MAP_PRIVATE,fd,0);CHECK(mapped!=MAP_FAILED);
    CASE("readonly mmap open and lookup directly borrow its data");
    A840PackV1View mapped_view{};
    CHECK(a840_pack_v1_open_memory(mapped,one.bytes,STACK,a840_compile_abi_fingerprint(),&mapped_view)==A840_PACK_OK);
    CHECK(a840_pack_v1_lookup(&mapped_view,&a.record.pipeline_key,&entry)==A840_PACK_OK);
    const auto *map_begin=static_cast<const std::uint8_t *>(mapped);
    CHECK(reinterpret_cast<const std::uint8_t *>(entry.stages[0].isa_words)>=map_begin &&
          reinterpret_cast<const std::uint8_t *>(entry.stages[0].isa_words)<map_begin+one.bytes);
    CHECK(reinterpret_cast<std::uintptr_t>(entry.stages[0].isa_words)%128==0);
    CHECK(munmap(mapped,one.bytes)==0 && close(fd)==0);
    CASE("Tier1 replace never publishes partial bytes");
    CHECK(a840_pack_v1_save_file(path.c_str(),second.data,second.bytes,STACK,a840_compile_abi_fingerprint())==A840_PACK_OK);
    CHECK(std::filesystem::file_size(path)==second.bytes);
    CASE("source/C-drive outputs are rejected without file creation");
    CHECK(a840_pack_v1_save_file("/mnt/c/forbidden.pack.bin",one.data,one.bytes,STACK,a840_compile_abi_fingerprint())!=A840_PACK_OK);
}
} /* namespace */
int main(int argc,char **argv)
{
    if(argc!=2){std::fprintf(stderr,"usage: test_pack_v1 <project-cache-subdirectory>\n");return 2;}
    const auto path=std::filesystem::weakly_canonical(argv[1]);
    const auto cache=std::filesystem::path("/home/hua/nezha-work/ios27/adreno840-native/.cache");
    const auto relative=path.lexically_relative(cache);
    CHECK(!relative.empty() && !relative.is_absolute() && *relative.begin()!="..");
    std::filesystem::create_directories(path);
    test_hash();test_container(path);
    CHECK(cases>=50);
    std::printf("PASS: %zu cases / %zu checks; header64, readonly mmap, zero-copy lookup, ISA128, SHA/HBB16/stack identity\n",cases,checks);
    return EXIT_SUCCESS;
}
