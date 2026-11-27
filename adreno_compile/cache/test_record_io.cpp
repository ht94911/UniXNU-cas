/* SPDX-License-Identifier: MIT */
#include "a840_record_io.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
std::size_t assertions=0;
std::filesystem::path disk;
constexpr std::uint64_t stack=UINT64_C(0x123456789abcdef0);
[[noreturn]] void fail(const char *what,int line)
{std::fprintf(stderr,"test_record_io.cpp:%d: %s\n",line,what);std::abort();}
#define CHECK(x) do {++assertions;if(!(x))fail(#x,__LINE__);}while(false)

struct Fixture {
    A840PipelineRecord record{};
    std::array<std::array<std::uint32_t,8>,2> isa{};
    std::array<std::array<std::uint32_t,4>,2> constants{};
    std::array<std::array<std::uint32_t,6>,2> sds{};
    explicit Fixture(bool render=false)
    {
        record.abi_fingerprint=a840_compile_abi_fingerprint();record.stack_key=stack;
        record.chip_id=A840_RECORD_CHIP_ID;record.highest_bank_bit=16;
        record.stage_count=render?2:1;record.binding_count=3;record.flags=0x12345678;
        for(std::size_t i=0;i<32;++i)record.pipeline_key.sha256[i]=static_cast<std::uint8_t>(i+1);
        for(std::size_t i=0;i<record.stage_count;++i) {
            for(std::size_t j=0;j<8;++j)isa[i][j]=0xf0000000u+static_cast<std::uint32_t>(i*32+j);
            for(std::size_t j=0;j<4;++j)constants[i][j]=0xb0000000u+static_cast<std::uint32_t>(i*16+j);
            for(std::size_t j=0;j<6;++j)sds[i][j]=0xc0000000u+static_cast<std::uint32_t>(i*16+j);
            auto &s=record.stages[i];s.abi_version=1;s.stage=render?static_cast<std::uint32_t>(i):2;
            s.isa_words=isa[i].data();s.isa_dwords=8;s.instruction_groups=2;s.max_full_reg=23;
            s.max_half_reg=-1;s.branchstack=7;s.constlen_vec4=9;s.const_data_dwords=4;
            s.const_data_words=constants[i].data();s.private_mem_bytes=17;s.shared_mem_bytes=1024;
            s.local_size[0]=8;s.local_size[1]=4;s.local_size[2]=1;s.flags_out=31;
            s.ubo_push_count=2;s.vpc_varying_count=3;s.sds_state_words=sds[i].data();s.sds_state_dwords=6;
            s.reg_spills=3;s.reg_fills=4;s.max_waves=8;
            for(std::size_t j=0;j<16;++j)s.ubo_push[j]={static_cast<std::uint32_t>(j),2,3,4,5,0x80000000u};
            for(std::size_t j=0;j<32;++j)s.vpc_varyings[j]=0xabcdef00u+static_cast<std::uint32_t>(j);
        }
        for(auto &b:record.bindings)b={};
        for(std::size_t i=0;i<record.binding_count;++i)
            record.bindings[i]={static_cast<std::uint32_t>(i),static_cast<std::uint32_t>(i+2),7,2,
                render?3u:4u,UINT32_MAX,static_cast<std::uint32_t>(i),UINT32_MAX};
    }
};

struct Owned {
    A840PipelineRecord record{};
    ~Owned(){a840_record_free(&record);}
};
struct Cache {
    A840PipelineMemCache *value=nullptr;
    Cache(std::size_t capacity=2,std::size_t bytes=1u<<20)
    {CHECK(a840_pipeline_mem_cache_create(capacity,bytes,stack,a840_compile_abi_fingerprint(),&value)==A840_RECORD_OK);}
    ~Cache(){a840_pipeline_mem_cache_destroy(value);}
};

std::vector<std::uint8_t> serialize(const A840PipelineRecord &r)
{
    std::uint8_t *data=nullptr;std::size_t bytes=0;
    CHECK(a840_record_serialize(&r,&data,&bytes)==A840_RECORD_OK);
    std::vector<std::uint8_t> result(data,data+bytes);std::free(data);return result;
}
void compare_stage(const A840CompiledStage &a,const A840CompiledStage &b)
{
    CHECK(a.abi_version==b.abi_version && a.stage==b.stage && a.isa_dwords==b.isa_dwords);
    CHECK(a.instruction_groups==b.instruction_groups && a.max_full_reg==b.max_full_reg && a.max_half_reg==b.max_half_reg);
    CHECK(a.branchstack==b.branchstack && a.constlen_vec4==b.constlen_vec4 && a.const_data_dwords==b.const_data_dwords);
    CHECK(a.private_mem_bytes==b.private_mem_bytes && a.shared_mem_bytes==b.shared_mem_bytes);
    CHECK(std::memcmp(a.local_size,b.local_size,sizeof(a.local_size))==0 && a.flags_out==b.flags_out);
    CHECK(a.ubo_push_count==b.ubo_push_count && std::memcmp(a.ubo_push,b.ubo_push,sizeof(a.ubo_push))==0);
    CHECK(a.vpc_varying_count==b.vpc_varying_count && std::memcmp(a.vpc_varyings,b.vpc_varyings,sizeof(a.vpc_varyings))==0);
    CHECK(a.sds_state_dwords==b.sds_state_dwords && a.reg_spills==b.reg_spills && a.reg_fills==b.reg_fills && a.max_waves==b.max_waves);
    CHECK(std::memcmp(a.isa_words,b.isa_words,a.isa_dwords*4)==0);
    if(a.const_data_dwords)CHECK(std::memcmp(a.const_data_words,b.const_data_words,a.const_data_dwords*4)==0);
    if(a.sds_state_dwords)CHECK(std::memcmp(a.sds_state_words,b.sds_state_words,a.sds_state_dwords*4)==0);
    CHECK(b.owner==nullptr);
}
void compare_record(const A840PipelineRecord &a,const A840PipelineRecord &b)
{
    CHECK(a.abi_fingerprint==b.abi_fingerprint && a.stack_key==b.stack_key && a.chip_id==b.chip_id);
    CHECK(a.highest_bank_bit==b.highest_bank_bit && a.stage_count==b.stage_count && a.binding_count==b.binding_count && a.flags==b.flags);
    CHECK(std::memcmp(a.pipeline_key.sha256,b.pipeline_key.sha256,32)==0);
    for(std::size_t i=0;i<a.stage_count;++i)compare_stage(a.stages[i],b.stages[i]);
    CHECK(std::memcmp(a.bindings,b.bindings,a.binding_count*sizeof(A840RecordBinding))==0);
}
void decode_error(const std::vector<std::uint8_t> &data,A840RecordStatus expected,
    std::uint64_t expected_stack=stack,std::uint64_t abi=a840_compile_abi_fingerprint())
{
    Owned out;out.record.flags=0xdeadbeef;const auto before=out.record;
    CHECK(a840_record_deserialize(data.data(),data.size(),expected_stack,abi,nullptr,&out.record)==expected);
    CHECK(std::memcmp(&out.record,&before,sizeof(before))==0);
}
void serialize_error(const A840PipelineRecord &record,A840RecordStatus expected)
{
    auto *data=reinterpret_cast<std::uint8_t *>(std::uintptr_t{0x1000});std::size_t bytes=19;
    CHECK(a840_record_serialize(&record,&data,&bytes)==expected);
    CHECK(data==reinterpret_cast<std::uint8_t *>(std::uintptr_t{0x1000}) && bytes==19);
}
void put32(std::vector<std::uint8_t>&data,std::size_t at,std::uint32_t value)
{for(unsigned i=0;i<4;++i)data[at+i]=static_cast<std::uint8_t>(value>>(8*i));}
void rehash(std::vector<std::uint8_t>&data)
{std::memset(data.data()+96,0,32);std::uint8_t hash[32];CHECK(a840_record_sha256(data.data(),data.size(),hash)==A840_RECORD_OK);std::memcpy(data.data()+96,hash,32);}
std::string hex(const std::uint8_t *bytes,std::size_t count)
{const char *digits="0123456789abcdef";std::string s;for(std::size_t i=0;i<count;++i){s+=digits[bytes[i]>>4];s+=digits[bytes[i]&15];}return s;}
std::filesystem::path filename(const A840PipelineKey &key)
{return disk/(hex(key.sha256,32)+".a840pr");}
void write_file(const std::filesystem::path &path,const std::vector<std::uint8_t> &data)
{std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));CHECK(bool(f));}
A840PipelineCacheStats stats(const Cache &cache)
{A840PipelineCacheStats s{};CHECK(a840_pipeline_mem_cache_stats(cache.value,&s)==A840_RECORD_OK);return s;}

void test_sha_empty(){std::uint8_t hash[32];CHECK(a840_record_sha256(nullptr,0,hash)==A840_RECORD_OK);CHECK(hex(hash,32)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");}
void test_sha_abc(){std::uint8_t hash[32];CHECK(a840_record_sha256("abc",3,hash)==A840_RECORD_OK);CHECK(hex(hash,32)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");}
void test_sha_multiblock(){const char *v="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";std::uint8_t hash[32];CHECK(a840_record_sha256(v,std::strlen(v),hash)==A840_RECORD_OK);CHECK(hex(hash,32)=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");}
void test_sha_million_a(){std::vector<char> a(1000000,'a');std::uint8_t hash[32];CHECK(a840_record_sha256(a.data(),a.size(),hash)==A840_RECORD_OK);CHECK(hex(hash,32)=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");}
void test_sha_null(){std::uint8_t hash[32];std::memset(hash,0xa5,32);CHECK(a840_record_sha256(nullptr,1,hash)==A840_RECORD_INVALID_ARG);CHECK(a840_record_sha256("a",1,nullptr)==A840_RECORD_INVALID_ARG);CHECK(hash[0]==0xa5);}

void test_compute_roundtrip(){Fixture f;const auto data=serialize(f.record);Owned out;CHECK(a840_record_deserialize(data.data(),data.size(),stack,f.record.abi_fingerprint,&f.record.pipeline_key,&out.record)==A840_RECORD_OK);compare_record(f.record,out.record);CHECK(out.record.owned_storage && out.record.owned_bytes==72);CHECK(serialize(out.record)==data);}
void test_render_roundtrip(){Fixture f(true);const auto data=serialize(f.record);Owned out;CHECK(a840_record_deserialize(data.data(),data.size(),stack,f.record.abi_fingerprint,nullptr,&out.record)==A840_RECORD_OK);compare_record(f.record,out.record);CHECK(out.record.stage_count==2 && serialize(out.record)==data);}
void test_all_metadata_arrays(){Fixture f(true);f.record.stages[0].ubo_push_count=16;f.record.stages[1].vpc_varying_count=32;f.record.stages[0].max_full_reg=-1;f.record.stages[1].max_half_reg=79;auto data=serialize(f.record);Owned out;CHECK(a840_record_deserialize(data.data(),data.size(),stack,f.record.abi_fingerprint,nullptr,&out.record)==A840_RECORD_OK);compare_record(f.record,out.record);}
void test_optional_payloads(){Fixture f;auto &s=f.record.stages[0];s.const_data_dwords=s.sds_state_dwords=0;s.const_data_words=s.sds_state_words=nullptr;f.record.binding_count=0;auto data=serialize(f.record);Owned out;CHECK(a840_record_deserialize(data.data(),data.size(),stack,f.record.abi_fingerprint,nullptr,&out.record)==A840_RECORD_OK);compare_record(f.record,out.record);CHECK(!out.record.stages[0].const_data_words && !out.record.stages[0].sds_state_words);}
void test_owner_not_persisted(){Fixture f;f.record.stages[0].owner=reinterpret_cast<void*>(std::uintptr_t{0x1000});auto a=serialize(f.record);f.record.stages[0].owner=nullptr;CHECK(a==serialize(f.record));a840_record_free(&f.record);CHECK(f.isa[0][0]==0xf0000000u);}
void test_determinism(){Fixture a,b;a.record.stages[0].owner=&a;b.record.stages[0].owner=&b;CHECK(serialize(a.record)==serialize(b.record));}
void test_header_layout(){Fixture f;auto d=serialize(f.record);CHECK(sizeof(A840RecordHeader)==64 && std::memcmp(d.data(),"A840PR01",8)==0);CHECK(d[8]==1 && d[12]==128 && d[48]==16 && d[52]==1 && d[56]==3);CHECK(d[40]==0x31 && d[41]==0x0a && d[42]==0x05 && d[43]==0x44);CHECK(std::memcmp(d.data()+64,f.record.pipeline_key.sha256,32)==0);}
void test_whole_record_single_bit(){Fixture f(true);auto data=serialize(f.record);for(std::size_t i=0;i<data.size();++i){data[i]^=1;Owned out;auto st=a840_record_deserialize(data.data(),data.size(),stack,f.record.abi_fingerprint,nullptr,&out.record);CHECK(st!=A840_RECORD_OK && !out.record.owned_storage);data[i]^=1;}}
void test_all_truncations(){Fixture f;auto data=serialize(f.record);for(std::size_t n=0;n<data.size();++n){Owned out;CHECK(a840_record_deserialize(data.data(),n,stack,f.record.abi_fingerprint,nullptr,&out.record)==A840_RECORD_LENGTH_ERROR);CHECK(!out.record.owned_storage);}}
void test_tail_garbage(){Fixture f;auto d=serialize(f.record);d.push_back(0);decode_error(d,A840_RECORD_LENGTH_ERROR);d.resize(d.size()+3);decode_error(d,A840_RECORD_LENGTH_ERROR);}
void test_magic(){Fixture f;auto d=serialize(f.record);d[0]='X';decode_error(d,A840_RECORD_MAGIC_ERROR);}
void test_version(){Fixture f;auto d=serialize(f.record);put32(d,8,2);decode_error(d,A840_RECORD_VERSION_ERROR);}
void test_prefix_size(){Fixture f;auto d=serialize(f.record);put32(d,12,64);decode_error(d,A840_RECORD_VERSION_ERROR);}
void test_length_header(){Fixture f;auto d=serialize(f.record);d[16]^=4;decode_error(d,A840_RECORD_LENGTH_ERROR);}
void test_stack_mismatch(){Fixture f;auto d=serialize(f.record);decode_error(d,A840_RECORD_STACK_MISMATCH,stack+1);d[32]^=1;decode_error(d,A840_RECORD_STACK_MISMATCH);}
void test_abi_mismatch(){Fixture f;auto d=serialize(f.record);decode_error(d,A840_RECORD_ABI_MISMATCH,stack,f.record.abi_fingerprint+1);d[24]^=1;decode_error(d,A840_RECORD_ABI_MISMATCH);}
void test_chip_mismatch(){Fixture f;auto d=serialize(f.record);d[40]^=1;decode_error(d,A840_RECORD_CHIP_MISMATCH);}
void test_hbb15_rejected(){Fixture f;auto d=serialize(f.record);put32(d,48,15);rehash(d);decode_error(d,A840_RECORD_HBB_MISMATCH);f.record.highest_bank_bit=15;serialize_error(f.record,A840_RECORD_HBB_MISMATCH);}
void test_payload_hash(){Fixture f;auto d=serialize(f.record);d.back()^=0x80;decode_error(d,A840_RECORD_HASH_MISMATCH);}
void test_hash_digest(){Fixture f;auto d=serialize(f.record);d[96]^=1;decode_error(d,A840_RECORD_HASH_MISMATCH);}
void test_key_hash(){Fixture f;auto d=serialize(f.record);d[64]^=1;decode_error(d,A840_RECORD_HASH_MISMATCH);}
void test_wrong_expected_key(){Fixture f;auto d=serialize(f.record);auto key=f.record.pipeline_key;key.sha256[0]^=1;Owned out;CHECK(a840_record_deserialize(d.data(),d.size(),stack,f.record.abi_fingerprint,&key,&out.record)==A840_RECORD_HASH_MISMATCH);}
void test_unknown_counts(){Fixture f;auto d=serialize(f.record);put32(d,52,3);rehash(d);decode_error(d,A840_RECORD_RANGE_ERROR);d=serialize(f.record);put32(d,56,129);rehash(d);decode_error(d,A840_RECORD_RANGE_ERROR);}
void test_stage_shape(){Fixture f;f.record.stages[0].stage=A840_STAGE_VERTEX;serialize_error(f.record,A840_RECORD_FORMAT_ERROR);Fixture r(true);r.record.stages[1].stage=A840_STAGE_VERTEX;serialize_error(r.record,A840_RECORD_FORMAT_ERROR);}
void test_wire_stage_overflow(){Fixture f;auto d=serialize(f.record);put32(d,128+8,UINT32_MAX);rehash(d);decode_error(d,A840_RECORD_RANGE_ERROR);}
void test_wire_ubo_overflow(){Fixture f;auto d=serialize(f.record);put32(d,128+15*4,17);rehash(d);decode_error(d,A840_RECORD_RANGE_ERROR);}
void test_zero_key(){Fixture f;std::memset(f.record.pipeline_key.sha256,0,32);serialize_error(f.record,A840_RECORD_FORMAT_ERROR);}
void test_invalid_record_counts(){Fixture f;f.record.stage_count=0;serialize_error(f.record,A840_RECORD_RANGE_ERROR);f.record.stage_count=1;f.record.binding_count=129;serialize_error(f.record,A840_RECORD_RANGE_ERROR);}
void test_missing_payload_pointer(){Fixture f;f.record.stages[0].isa_words=nullptr;serialize_error(f.record,A840_RECORD_INVALID_ARG);f.record.stages[0].isa_words=f.isa[0].data();f.record.stages[0].const_data_words=nullptr;serialize_error(f.record,A840_RECORD_INVALID_ARG);}
void test_binding_validation(){Fixture f;f.record.bindings[0].descriptor_count=0;serialize_error(f.record,A840_RECORD_FORMAT_ERROR);f.record.bindings[0].descriptor_count=1;f.record.bindings[0].stage_mask=3;serialize_error(f.record,A840_RECORD_FORMAT_ERROR);}
void test_null_serialize(){std::uint8_t *p=nullptr;std::size_t n=0;CHECK(a840_record_serialize(nullptr,&p,&n)==A840_RECORD_INVALID_ARG);Fixture f;CHECK(a840_record_serialize(&f.record,nullptr,&n)==A840_RECORD_INVALID_ARG);CHECK(a840_record_serialize(&f.record,&p,nullptr)==A840_RECORD_INVALID_ARG);}
void test_null_deserialize(){Fixture f;auto d=serialize(f.record);Owned out;CHECK(a840_record_deserialize(nullptr,1,stack,f.record.abi_fingerprint,nullptr,&out.record)==A840_RECORD_INVALID_ARG);CHECK(a840_record_deserialize(d.data(),d.size(),0,f.record.abi_fingerprint,nullptr,&out.record)==A840_RECORD_INVALID_ARG);CHECK(a840_record_deserialize(d.data(),d.size(),stack,f.record.abi_fingerprint,nullptr,nullptr)==A840_RECORD_INVALID_ARG);}
void test_alias_outputs(){Fixture f;auto d=serialize(f.record);CHECK(a840_record_deserialize(d.data(),d.size(),stack,f.record.abi_fingerprint,nullptr,reinterpret_cast<A840PipelineRecord*>(d.data()))==A840_RECORD_ALIAS);auto before=f.record;CHECK(a840_record_serialize(&f.record,reinterpret_cast<std::uint8_t**>(&f.record),&f.record.owned_bytes)==A840_RECORD_ALIAS);CHECK(std::memcmp(&f.record,&before,sizeof(before))==0);}
void test_free_twice(){Fixture f;auto d=serialize(f.record);Owned out;CHECK(a840_record_deserialize(d.data(),d.size(),stack,f.record.abi_fingerprint,nullptr,&out.record)==A840_RECORD_OK);a840_record_free(&out.record);a840_record_free(&out.record);a840_record_free(nullptr);CHECK(!out.record.owned_storage && out.record.stage_count==0);}

A840RecordKeyStage key_stage(){A840RecordKeyStage s{};s.entry_point="copyTexture";s.stage=A840_STAGE_COMPUTE;s.sample_count=1;s.local_size[0]=8;s.local_size[1]=8;s.local_size[2]=1;s.spirv_sha256[0]=42;return s;}
void test_compute_key_repeat(){auto s=key_stage();A840PipelineKey a{},b{};CHECK(a840_record_compute_key(&s,1,&a)==A840_RECORD_OK);CHECK(a840_record_compute_key(&s,1,&b)==A840_RECORD_OK);CHECK(std::memcmp(&a,&b,sizeof(a))==0);}
void test_compute_key_fields(){auto s=key_stage();A840PipelineKey base{};CHECK(a840_record_compute_key(&s,1,&base)==A840_RECORD_OK);for(unsigned change=0;change<8;++change){auto t=s;switch(change){case 0:t.entry_point="another";break;case 1:t.spirv_sha256[3]^=1;break;case 2:t.sample_count=2;break;case 3:t.local_size[0]=4;break;case 4:t.flags=1;break;case 5:t.const_push_dwords[63]=1;break;case 6:t.const_push_mask=1;break;default:t.color_rt_mask=1;t.half_rt_mask=1;}A840PipelineKey key{};CHECK(a840_record_compute_key(&t,1,&key)==A840_RECORD_OK);CHECK(std::memcmp(&key,&base,32)!=0);}}
void test_render_key_shape(){A840RecordKeyStage s[2]={key_stage(),key_stage()};s[0].stage=0;s[1].stage=1;A840PipelineKey key{};CHECK(a840_record_compute_key(s,2,&key)==A840_RECORD_OK);s[1].stage=0;CHECK(a840_record_compute_key(s,2,&key)==A840_RECORD_FORMAT_ERROR);}
void test_key_invalid(){auto s=key_stage();A840PipelineKey key{};s.entry_point="";CHECK(a840_record_compute_key(&s,1,&key)==A840_RECORD_RANGE_ERROR);s=key_stage();s.sample_count=3;CHECK(a840_record_compute_key(&s,1,&key)==A840_RECORD_RANGE_ERROR);s=key_stage();s.half_rt_mask=1;CHECK(a840_record_compute_key(&s,1,&key)==A840_RECORD_RANGE_ERROR);CHECK(a840_record_compute_key(nullptr,1,&key)==A840_RECORD_INVALID_ARG);}

void test_disk_roundtrip(){Fixture f(true);CHECK(a840_record_save_file(disk.c_str(),&f.record)==A840_RECORD_OK);Owned out;CHECK(a840_record_load_file(disk.c_str(),&f.record.pipeline_key,stack,f.record.abi_fingerprint,&out.record)==A840_RECORD_OK);compare_record(f.record,out.record);}
void test_disk_replace(){Fixture f;CHECK(a840_record_save_file(disk.c_str(),&f.record)==A840_RECORD_OK);f.isa[0][0]=42;CHECK(a840_record_save_file(disk.c_str(),&f.record)==A840_RECORD_OK);Owned out;CHECK(a840_record_load_file(disk.c_str(),&f.record.pipeline_key,stack,f.record.abi_fingerprint,&out.record)==A840_RECORD_OK);CHECK(out.record.stages[0].isa_words[0]==42);}
void test_disk_no_temporary(){for(const auto &p:std::filesystem::directory_iterator(disk))CHECK(p.path().filename().string().find(".tmp.")==std::string::npos);}
void test_disk_unknown_key(){Fixture f;f.record.pipeline_key.sha256[0]=0xfa;Owned out;CHECK(a840_record_load_file(disk.c_str(),&f.record.pipeline_key,stack,f.record.abi_fingerprint,&out.record)==A840_RECORD_NOT_FOUND);}
void test_disk_missing_directory(){Fixture f;CHECK(a840_record_save_file((disk/"absent").c_str(),&f.record)==A840_RECORD_IO_ERROR);}
void test_disk_corrupt_then_restore(){Fixture f;auto d=serialize(f.record);d.back()^=1;write_file(filename(f.record.pipeline_key),d);Owned out;CHECK(a840_record_load_file(disk.c_str(),&f.record.pipeline_key,stack,f.record.abi_fingerprint,&out.record)==A840_RECORD_HASH_MISMATCH);CHECK(a840_record_save_file(disk.c_str(),&f.record)==A840_RECORD_OK);}
void test_disk_truncate_then_restore(){Fixture f;auto d=serialize(f.record);d.resize(64);write_file(filename(f.record.pipeline_key),d);Owned out;CHECK(a840_record_load_file(disk.c_str(),&f.record.pipeline_key,stack,f.record.abi_fingerprint,&out.record)==A840_RECORD_LENGTH_ERROR);CHECK(a840_record_save_file(disk.c_str(),&f.record)==A840_RECORD_OK);}
void test_disk_symlink_reject(){Fixture f;auto key=f.record.pipeline_key;key.sha256[0]=0xfc;std::filesystem::create_symlink(filename(f.record.pipeline_key),filename(key));Owned out;CHECK(a840_record_load_file(disk.c_str(),&key,stack,f.record.abi_fingerprint,&out.record)==A840_RECORD_IO_ERROR);std::filesystem::remove(filename(key));}
void test_disk_interrupted_temporary(){Fixture f;auto name=disk/".crashed.tmp.record";write_file(name,{1,2,3});CHECK(a840_record_save_file(disk.c_str(),&f.record)==A840_RECORD_OK);Owned out;CHECK(a840_record_load_file(disk.c_str(),&f.record.pipeline_key,stack,f.record.abi_fingerprint,&out.record)==A840_RECORD_OK);std::filesystem::remove(name);}
void test_disk_stale_identity(){Fixture f;Owned out;CHECK(a840_record_load_file(disk.c_str(),&f.record.pipeline_key,stack+1,f.record.abi_fingerprint,&out.record)==A840_RECORD_STACK_MISMATCH);CHECK(a840_record_load_file(disk.c_str(),&f.record.pipeline_key,stack,f.record.abi_fingerprint+1,&out.record)==A840_RECORD_ABI_MISMATCH);}

void test_cache_empty_miss(){Cache cache;Fixture f;CHECK(!a840_pipeline_mem_cache_get(cache.value,&f.record.pipeline_key));auto s=stats(cache);CHECK(s.misses==1 && s.hits==0 && s.entries==0);}
void test_cache_hit_clone(){Cache cache;Fixture f;CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_OK);f.isa[0][0]=42;const auto *r=a840_pipeline_mem_cache_get(cache.value,&f.record.pipeline_key);CHECK(r && r->stages[0].isa_words[0]==0xf0000000u);auto s=stats(cache);CHECK(s.hits==1 && s.entries==1 && s.puts==1);}
void test_cache_lru_eviction(){Cache cache;Fixture a,b,c;b.record.pipeline_key.sha256[0]=42;c.record.pipeline_key.sha256[0]=43;CHECK(a840_pipeline_mem_cache_put(cache.value,&a.record)==A840_RECORD_OK);CHECK(a840_pipeline_mem_cache_put(cache.value,&b.record)==A840_RECORD_OK);CHECK(a840_pipeline_mem_cache_get(cache.value,&a.record.pipeline_key));CHECK(a840_pipeline_mem_cache_put(cache.value,&c.record)==A840_RECORD_OK);CHECK(!a840_pipeline_mem_cache_get(cache.value,&b.record.pipeline_key));CHECK(a840_pipeline_mem_cache_get(cache.value,&a.record.pipeline_key));CHECK(stats(cache).evictions==1);}
void test_cache_replace(){Cache cache;Fixture f;CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_OK);f.isa[0][0]=42;CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_OK);CHECK(a840_pipeline_mem_cache_get(cache.value,&f.record.pipeline_key)->stages[0].isa_words[0]==42);auto s=stats(cache);CHECK(s.entries==1 && s.replacements==1 && s.evictions==0);}
void test_cache_byte_capacity(){Fixture f;auto size=serialize(f.record).size();Cache cache(10,size);CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_OK);f.record.pipeline_key.sha256[0]=91;CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_OK);auto s=stats(cache);CHECK(s.entries==1 && s.bytes==size && s.evictions==1);}
void test_cache_oversize_preserves(){Fixture f;Cache cache(2,1);auto before=stats(cache);CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_CAPACITY);auto after=stats(cache);CHECK(std::memcmp(&before,&after,sizeof(before))==0);}
void test_cache_invalid_identity(){Cache cache;Fixture f;CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_OK);auto before=stats(cache);f.record.highest_bank_bit=15;CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_HBB_MISMATCH);f.record.highest_bank_bit=16;f.record.stack_key=1;CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_STACK_MISMATCH);auto after=stats(cache);CHECK(std::memcmp(&before,&after,sizeof(before))==0);}
void test_cache_repeat_churn(){Cache cache(3);Fixture f;for(unsigned i=0;i<1000;++i){f.record.pipeline_key.sha256[0]=static_cast<std::uint8_t>(i%255+1);CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_OK);CHECK(a840_pipeline_mem_cache_get(cache.value,&f.record.pipeline_key));}auto s=stats(cache);CHECK(s.entries==3 && s.evictions==997 && s.hits==1000);}
void test_cache_capacity_one(){Cache cache(1);Fixture a,b;b.record.pipeline_key.sha256[0]=88;CHECK(a840_pipeline_mem_cache_put(cache.value,&a.record)==A840_RECORD_OK);CHECK(a840_pipeline_mem_cache_put(cache.value,&b.record)==A840_RECORD_OK);CHECK(!a840_pipeline_mem_cache_get(cache.value,&a.record.pipeline_key));CHECK(stats(cache).entries==1);}
void test_cache_invalid_create(){A840PipelineMemCache *cache=nullptr;CHECK(a840_pipeline_mem_cache_create(0,1,stack,a840_compile_abi_fingerprint(),&cache)==A840_RECORD_INVALID_ARG);CHECK(a840_pipeline_mem_cache_create(1,1,stack,1,&cache)==A840_RECORD_ABI_MISMATCH);CHECK(a840_pipeline_mem_cache_create(65537,1,stack,a840_compile_abi_fingerprint(),&cache)==A840_RECORD_RANGE_ERROR);CHECK(!cache);}
void test_cache_null(){a840_pipeline_mem_cache_destroy(nullptr);CHECK(!a840_pipeline_mem_cache_get(nullptr,nullptr));CHECK(a840_pipeline_mem_cache_put(nullptr,nullptr)==A840_RECORD_INVALID_ARG);CHECK(a840_pipeline_mem_cache_stats(nullptr,nullptr)==A840_RECORD_INVALID_ARG);}

void test_maximum_bindings()
{
    Fixture f;f.record.binding_count=128;
    for(auto &binding:f.record.bindings)binding={7,99,7,1,4,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    auto data=serialize(f.record);Owned out;
    CHECK(a840_record_deserialize(data.data(),data.size(),stack,f.record.abi_fingerprint,nullptr,&out.record)==A840_RECORD_OK);
    compare_record(f.record,out.record);
}
void test_key_output_alias()
{
    auto s=key_stage();const auto before=s;
    CHECK(a840_record_compute_key(&s,1,reinterpret_cast<A840PipelineKey*>(&s))==A840_RECORD_ALIAS);
    CHECK(std::memcmp(&s,&before,sizeof(s))==0);
}
void test_cache_borrowed_replacement()
{
    Cache cache;Fixture f;
    CHECK(a840_pipeline_mem_cache_put(cache.value,&f.record)==A840_RECORD_OK);
    const auto *borrowed=a840_pipeline_mem_cache_get(cache.value,&f.record.pipeline_key);
    CHECK(borrowed && a840_pipeline_mem_cache_put(cache.value,borrowed)==A840_RECORD_OK);
    const auto *replacement=a840_pipeline_mem_cache_get(cache.value,&f.record.pipeline_key);
    CHECK(replacement);compare_record(f.record,*replacement);
    CHECK(stats(cache).replacements==1);
}

struct Test{const char *name;void(*run)();};
#define TEST(x) {#x,x}
const Test tests[]={
 TEST(test_sha_empty),TEST(test_sha_abc),TEST(test_sha_multiblock),TEST(test_sha_million_a),TEST(test_sha_null),
 TEST(test_compute_roundtrip),TEST(test_render_roundtrip),TEST(test_all_metadata_arrays),TEST(test_optional_payloads),
 TEST(test_owner_not_persisted),TEST(test_determinism),TEST(test_header_layout),TEST(test_whole_record_single_bit),
 TEST(test_all_truncations),TEST(test_tail_garbage),TEST(test_magic),TEST(test_version),TEST(test_prefix_size),
 TEST(test_length_header),TEST(test_stack_mismatch),TEST(test_abi_mismatch),TEST(test_chip_mismatch),TEST(test_hbb15_rejected),
 TEST(test_payload_hash),TEST(test_hash_digest),TEST(test_key_hash),TEST(test_wrong_expected_key),TEST(test_unknown_counts),
 TEST(test_stage_shape),TEST(test_wire_stage_overflow),TEST(test_wire_ubo_overflow),TEST(test_zero_key),TEST(test_invalid_record_counts),
 TEST(test_missing_payload_pointer),TEST(test_binding_validation),TEST(test_null_serialize),TEST(test_null_deserialize),
 TEST(test_alias_outputs),TEST(test_free_twice),TEST(test_compute_key_repeat),TEST(test_compute_key_fields),
 TEST(test_render_key_shape),TEST(test_key_invalid),TEST(test_disk_roundtrip),TEST(test_disk_replace),TEST(test_disk_no_temporary),
 TEST(test_disk_unknown_key),TEST(test_disk_missing_directory),TEST(test_disk_corrupt_then_restore),TEST(test_disk_truncate_then_restore),
 TEST(test_disk_symlink_reject),TEST(test_disk_interrupted_temporary),TEST(test_disk_stale_identity),TEST(test_cache_empty_miss),
 TEST(test_cache_hit_clone),TEST(test_cache_lru_eviction),TEST(test_cache_replace),TEST(test_cache_byte_capacity),
 TEST(test_cache_oversize_preserves),TEST(test_cache_invalid_identity),TEST(test_cache_repeat_churn),TEST(test_cache_capacity_one),
 TEST(test_cache_invalid_create),TEST(test_cache_null),TEST(test_maximum_bindings),TEST(test_key_output_alias),
 TEST(test_cache_borrowed_replacement)
};
static_assert(sizeof(tests)/sizeof(tests[0])>=40,"40+ independent cache tests");
}

int main(int argc,char **argv)
{
    if(argc!=2){std::fprintf(stderr,"usage: test_record_io <cache-directory>\n");return 2;}
    disk=std::filesystem::absolute(argv[1]).lexically_normal();
    const std::string approved="/home/hua/nezha-work/ios27/adreno840-native/.cache/";
    if(disk.string().compare(0,approved.size(),approved)!=0 || disk.string().size()<=approved.size()){std::fprintf(stderr,"disk test must use .cache/<subdir>\n");return 2;}
    std::filesystem::create_directories(disk);
    for(const auto &test:tests){test.run();std::printf("PASS %s\n",test.name);}
    std::printf("PASS: %zu independent record_io cases, %zu assertions\n",sizeof(tests)/sizeof(tests[0]),assertions);
    return 0;
}
