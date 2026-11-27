/* SPDX-License-Identifier: MIT */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include "a840_pack_v1.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define META_WORDS 149u
#define PLAN_WORDS (sizeof(A840BindingPlan) / 4u)
#define VIEW_VALID UINT32_C(0x31564b50)
#define CACHE_ROOT "/home/hua/nezha-work/ios27/adreno840-native/.cache/"
#define TRY(call) do { A840PackV1Status st_=(call); if(st_!=A840_PACK_OK)return st_; } while(0)

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint64_t get64(const uint8_t *p)
{ return (uint64_t)get32(p) | (uint64_t)get32(p+4)<<32; }
static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1]<<8); }
static void put32(uint8_t *p,uint32_t value)
{ unsigned i;for(i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i)); }
static void put64(uint8_t *p,uint64_t value)
{ unsigned i;for(i=0;i<8;++i)p[i]=(uint8_t)(value>>(8*i)); }
static void put16(uint8_t *p,uint16_t value)
{ p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8); }
static int little_endian(void)
{ const uint32_t n=1;return *(const uint8_t *)&n==1; }
static int valid_region(const void *p,size_t bytes,size_t alignment)
{ return p && (uintptr_t)p%alignment==0 && bytes<=UINTPTR_MAX-(uintptr_t)p; }
static int overlaps(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an && bn && (x<=y ? y-x<an : x-y<bn);
}
static int compare_bytes(const void *a,const void *b,size_t bytes)
{
    const uint8_t *x=a,*y=b;size_t i;
    for(i=0;i<bytes;++i)if(x[i]!=y[i])return x[i]<y[i] ? -1 : 1;
    return 0;
}
static void copy_bytes(uint8_t *out,const uint8_t *input,size_t bytes)
{ size_t i;for(i=0;i<bytes;++i)out[i]=input[i]; }
static void zeros(uint8_t *p,size_t bytes)
{ size_t i;for(i=0;i<bytes;++i)p[i]=0; }
static int zero_bytes(const uint8_t *p,size_t bytes)
{ size_t i;for(i=0;i<bytes;++i)if(p[i])return 0;return 1; }
static int key_valid(const A840PipelineKey *key)
{ return valid_region(key,sizeof(*key),1) && !zero_bytes(key->sha256,32); }
static uint64_t key_hash(const uint8_t key[32])
{
    uint64_t hash=UINT64_C(14695981039346656037);unsigned i;
    for(i=0;i<32;++i){hash^=key[i];hash*=UINT64_C(1099511628211);}
    return hash;
}
static uint32_t align_to(uint32_t n,uint32_t alignment)
{ return (n+alignment-1)&~(alignment-1); }
static int add_u32(uint32_t *at,uint64_t bytes)
{ if(bytes>UINT32_MAX-*at)return 0;*at+=(uint32_t)bytes;return 1; }
static int align_u32(uint32_t *at,uint32_t alignment)
{
    if(*at>UINT32_MAX-(alignment-1))return 0;
    *at=align_to(*at,alignment);return 1;
}
static int within(size_t bytes,uint32_t offset,uint64_t count)
{ return offset<=bytes && count<=bytes-offset; }
static uint32_t metadata_word(const A840CompiledStage *s,uint32_t index)
{
    const uint32_t first[16]={s->abi_version,s->stage,s->isa_dwords,s->instruction_groups,
        (uint32_t)s->max_full_reg,(uint32_t)s->max_half_reg,s->branchstack,s->constlen_vec4,
        s->const_data_dwords,s->private_mem_bytes,s->shared_mem_bytes,s->local_size[0],
        s->local_size[1],s->local_size[2],s->flags_out,s->ubo_push_count};
    if(index<16)return first[index];
    if(index<112) {
        const A840UboPushRange *u=&s->ubo_push[(index-16)/6];
        const uint32_t words[6]={u->set_index,u->binding_index,u->src_offset_vec4,
            u->dst_const_vec4,u->size_vec4,u->reserved};
        return words[(index-16)%6];
    }
    if(index==112)return s->vpc_varying_count;
    if(index<145)return s->vpc_varyings[index-113];
    if(index==145)return s->sds_state_dwords;
    if(index==146)return s->reg_spills;
    if(index==147)return s->reg_fills;
    return s->max_waves;
}
static int32_t signed_word(uint32_t value)
{ return value<=INT32_MAX ? (int32_t)value : -(int32_t)(UINT32_MAX-value)-1; }

/* SHA256 math/LE discipline matches the read-only A840PR01 implementation.
 * Updates use scalar byte loops: no memcpy or allocation in open/lookup.
 */
typedef struct Sha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} Sha256;

static uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha_block(Sha256 *s, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64], a,b,c,d,e,f,g,h;
    size_t i;
    for (i=0;i<16;++i)
        w[i]=(uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
    for (;i<64;++i) {
        uint32_t x=w[i-15], y=w[i-2];
        w[i]=w[i-16]+(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-7]+(rotate(y,17)^rotate(y,19)^(y>>10));
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3]; e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for (i=0;i<64;++i) {
        uint32_t t1=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}

static void sha_init(Sha256 *s)
{
    static const uint32_t init[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    {size_t n;for(n=0;n<8;++n)s->h[n]=init[n];}s->bytes=0;s->used=0;
}
static void sha_update(Sha256 *s, const void *data, size_t bytes)
{
    const uint8_t *p=data;
    s->bytes+=bytes;
    while (bytes) {
        size_t n=sizeof(s->block)-s->used;
        if (n>bytes) n=bytes;
        {size_t i;for(i=0;i<n;++i)s->block[s->used+i]=p[i];}s->used+=n;p+=n;bytes-=n;
        if (s->used==sizeof(s->block)) {sha_block(s,s->block);s->used=0;}
    }
}
static void sha_finish(Sha256 *s, uint8_t out[32])
{
    uint64_t bits=s->bytes*8;
    size_t i;
    s->block[s->used++]=0x80;
    if (s->used>56) {{size_t i;for(i=s->used;i<64;++i)s->block[i]=0;}sha_block(s,s->block);s->used=0;}
    {size_t j;for(j=s->used;j<56;++j)s->block[j]=0;}
    for (i=0;i<8;++i) s->block[63-i]=(uint8_t)(bits>>(i*8));
    sha_block(s,s->block);
    for (i=0;i<32;++i) out[i]=(uint8_t)(s->h[i/4]>>(24-(i%4)*8));
}



static void file_digest(const uint8_t *p,size_t bytes,uint8_t digest[32])
{
    static const uint8_t empty[32]={0};Sha256 sha;
    sha_init(&sha);sha_update(&sha,p,32);sha_update(&sha,empty,32);
    sha_update(&sha,p+64,bytes-64);sha_finish(&sha,digest);
}
A840PackV1Status a840_pack_v1_sha256(const void *p,size_t bytes,uint8_t digest[32])
{
    Sha256 sha;
    if(!valid_region(digest,32,1) || (bytes && !valid_region(p,bytes,1)))return A840_PACK_INVALID_ARG;
    if(bytes>UINT64_MAX/8)return A840_PACK_RANGE;
    sha_init(&sha);sha_update(&sha,p,bytes);sha_finish(&sha,digest);
    return A840_PACK_OK;
}
static A840PackV1Status stage_valid(const A840CompiledStage *s)
{
    if(s->abi_version!=ADRENO_COMPILE_ABI_VERSION)return A840_PACK_ABI_MISMATCH;
    if(s->stage>A840_STAGE_COMPUTE || !s->isa_dwords || !s->instruction_groups ||
       s->max_full_reg < -1 || s->max_half_reg < -1)return A840_PACK_FORMAT;
    if(s->isa_dwords>A840_RECORD_MAX_ISA_DWORDS ||
       s->const_data_dwords>A840_RECORD_MAX_CONST_DWORDS ||
       s->sds_state_dwords>A840_RECORD_MAX_SDS_DWORDS ||
       s->ubo_push_count>ADRENO_COMPILE_MAX_UBO_PUSH ||
       s->vpc_varying_count>ADRENO_COMPILE_MAX_VARYINGS)return A840_PACK_RANGE;
    if(!valid_region(s->isa_words,(size_t)s->isa_dwords*4,4) ||
       (s->const_data_dwords && !valid_region(s->const_data_words,(size_t)s->const_data_dwords*4,4)) ||
       (s->sds_state_dwords && !valid_region(s->sds_state_words,(size_t)s->sds_state_dwords*4,4)))
        return A840_PACK_INVALID_ARG;
    return A840_PACK_OK;
}
static int copy_range_valid(A840ConstantCopy c,uint32_t bytes)
{ return c.dst_offset_bytes<=bytes && c.size_bytes<=bytes-c.dst_offset_bytes; }
static A840PackV1Status plan_valid(const A840BindingPlan *p,uint32_t stages)
{
    uint32_t i,j;
    if(p->binding_count>A840_BINDING_PLAN_MAX_BINDINGS ||
       p->metal_lookup_count>A840_BINDING_PLAN_MAX_LOOKUPS ||
       p->stage_count!=stages ||
       p->total_table_bytes>ADRENO_COMPILE_MAX_BIND_SETS*A840_BINDING_PLAN_MAX_SLOTS_PER_SET*A840_BINDING_PLAN_SLOT_BYTES)
        return A840_PACK_FORMAT;
    for(i=0;i<ADRENO_COMPILE_MAX_BIND_SETS;++i) {
        const A840BindingSetPlan *s=&p->sets[i];
        if(s->first_binding>p->binding_count || s->binding_count>p->binding_count-s->first_binding ||
           s->base_offset_bytes>p->total_table_bytes ||
           s->table_bytes>p->total_table_bytes-s->base_offset_bytes)return A840_PACK_FORMAT;
    }
    for(i=0;i<p->binding_count;++i) {
        const A840BindingLocation *b=&p->bindings[i];
        const uint32_t slots[5]={b->tex_slot,b->samp_slot,b->ubo_slot,b->ssbo_slot,b->storage_img_slot};
        if(b->set>=ADRENO_COMPILE_MAX_BIND_SETS || !b->descriptor_count ||
           !b->stage_mask || (b->stage_mask & ~(stages==1 ? 4u : 3u)) ||
           b->table_offset_bytes>p->total_table_bytes)return A840_PACK_FORMAT;
        for(j=0;j<5;++j)if(slots[j]!=UINT32_MAX && slots[j]>=A840_BINDING_PLAN_MAX_SLOTS_PER_SET)
            return A840_PACK_FORMAT;
    }
    for(i=0;i<p->metal_lookup_count;++i) {
        const A840MetalBindingLocation *b=&p->metal_lookup[i];
        if(b->stage>A840_STAGE_COMPUTE || b->kind>A840_METAL_SLOT_SAMPLER ||
           b->set>=ADRENO_COMPILE_MAX_BIND_SETS || !b->descriptor_count ||
           b->table_offset_bytes>p->total_table_bytes)return A840_PACK_FORMAT;
    }
    for(i=0;i<stages;++i) {
        const A840StageBindingPlan *s=&p->stages[i];
        if(s->stage!=(stages==1 ? A840_STAGE_COMPUTE : i) ||
           s->const_bytes>A840_BINDING_PLAN_MAX_CONST_VEC4*16 ||
           s->inline_ubo_count>ADRENO_COMPILE_MAX_UBO_PUSH ||
           !copy_range_valid(s->push_constants,s->const_bytes))return A840_PACK_FORMAT;
        for(j=0;j<s->inline_ubo_count;++j)
            if(s->inline_ubos[j].source>A840_INLINE_UBO_IR3_BINDFUL ||
               s->inline_ubos[j].set>=ADRENO_COMPILE_MAX_BIND_SETS ||
               !copy_range_valid(s->inline_ubos[j].copy,s->const_bytes))return A840_PACK_FORMAT;
    }
    return A840_PACK_OK;
}
static A840PackV1Status record_valid(const A840PipelineRecord *r,const A840BindingPlan *plan,
    uint64_t stack,uint64_t abi)
{
    uint32_t i;
    if(!valid_region(r,sizeof(*r),_Alignof(A840PipelineRecord)) ||
       !valid_region(plan,sizeof(*plan),_Alignof(A840BindingPlan)))return A840_PACK_INVALID_ARG;
    if(r->abi_fingerprint!=abi || abi!=a840_compile_abi_fingerprint())return A840_PACK_ABI_MISMATCH;
    if(!stack || r->stack_key!=stack)return A840_PACK_STACK_MISMATCH;
    if(r->chip_id!=A840_PACK_V1_CHIP_ID)return A840_PACK_CHIP_MISMATCH;
    if(r->highest_bank_bit!=A840_PACK_V1_HBB)return A840_PACK_HBB_MISMATCH;
    if(!key_valid(&r->pipeline_key) || r->stage_count<1 || r->stage_count>2 ||
       r->binding_count>A840_RECORD_MAX_BINDINGS)return A840_PACK_FORMAT;
    if((r->stage_count==1 && r->stages[0].stage!=A840_STAGE_COMPUTE) ||
       (r->stage_count==2 && (r->stages[0].stage!=A840_STAGE_VERTEX || r->stages[1].stage!=A840_STAGE_FRAGMENT)))
        return A840_PACK_FORMAT;
    for(i=0;i<r->stage_count;++i)TRY(stage_valid(&r->stages[i]));
    for(i=0;i<r->binding_count;++i) {
        const A840RecordBinding *b=&r->bindings[i];
        if(b->descriptor_set>=ADRENO_COMPILE_MAX_BIND_SETS || !b->descriptor_count ||
           !b->stage_mask || (b->stage_mask & ~(r->stage_count==1 ? 4u : 3u)))return A840_PACK_FORMAT;
    }
    return plan_valid(plan,r->stage_count);
}


static A840PackV1Status wire_stage_valid(const uint8_t *p,uint32_t expected)
{
    uint32_t isa=get32(p+8), constants=get32(p+32), sds=get32(p+145*4);
    uint32_t full=get32(p+16),half=get32(p+20);
    if(get32(p)!=ADRENO_COMPILE_ABI_VERSION)return A840_PACK_ABI_MISMATCH;
    if(get32(p+4)!=expected || !isa || !get32(p+12) ||
       (full>INT32_MAX && full!=UINT32_MAX) || (half>INT32_MAX && half!=UINT32_MAX))
        return A840_PACK_FORMAT;
    if(isa>A840_RECORD_MAX_ISA_DWORDS || constants>A840_RECORD_MAX_CONST_DWORDS ||
       sds>A840_RECORD_MAX_SDS_DWORDS || get32(p+60)>ADRENO_COMPILE_MAX_UBO_PUSH ||
       get32(p+112*4)>ADRENO_COMPILE_MAX_VARYINGS)return A840_PACK_RANGE;
    if(!zero_bytes(p+152*4,32))return A840_PACK_FORMAT;
    return A840_PACK_OK;
}
static A840PackV1Status canonical_entry(const A840PackV1View *v,uint32_t index,uint32_t *cursor)
{
    const uint8_t *e=v->base+v->directory_offset+(size_t)index*A840_PACK_V1_ENTRY_BYTES;
    uint32_t count=get32(e+36),meta=get32(e+40),boff=get32(e+44),bn=get32(e+48);
    uint32_t plan=get32(e+52),pbytes=get32(e+56),start=get32(e+60),total=get32(e+64);
    uint32_t i,at=*cursor;
    if(zero_bytes(e,32) || count<1 || count>2 || bn>A840_RECORD_MAX_BINDINGS ||
       start!=at || meta!=at || meta%128 || pbytes!=sizeof(A840BindingPlan) ||
       !zero_bytes(e+68,60))return A840_PACK_FORMAT;
    if(!within(v->bytes,meta,(uint64_t)count*A840_PACK_V1_STAGE_BYTES))return A840_PACK_LENGTH;
    if(!add_u32(&at,(uint64_t)count*A840_PACK_V1_STAGE_BYTES))return A840_PACK_RANGE;
    if((bn ? boff!=at : boff!=0) || !within(v->bytes,at,(uint64_t)bn*sizeof(A840RecordBinding)))
        return A840_PACK_FORMAT;
    for(i=0;i<bn;++i) {
        const uint8_t *b=v->base+boff+(size_t)i*32;
        uint32_t mask=get32(b+16);
        if(get32(b)>=ADRENO_COMPILE_MAX_BIND_SETS || !get32(b+12) ||
           !mask || (mask & ~(count==1 ? 4u : 3u)))return A840_PACK_FORMAT;
    }
    if(!add_u32(&at,(uint64_t)bn*32))return A840_PACK_RANGE;
    {
        uint32_t aligned=at;
        if(!align_u32(&aligned,64) || plan!=aligned || !within(v->bytes,at,aligned-at) ||
           !zero_bytes(v->base+at,aligned-at))return A840_PACK_FORMAT;
        at=aligned;
    }
    if(!within(v->bytes,plan,pbytes))return A840_PACK_LENGTH;
    TRY(plan_valid((const A840BindingPlan *)(const void *)(v->base+plan),count));
    if(!add_u32(&at,pbytes))return A840_PACK_RANGE;
    {
        uint32_t aligned=at;
        if(!align_u32(&aligned,128) || !within(v->bytes,at,aligned-at) ||
           !zero_bytes(v->base+at,aligned-at))return A840_PACK_FORMAT;
        at=aligned;
    }
    for(i=0;i<count;++i) {
        const uint8_t *s=v->base+meta+(size_t)i*A840_PACK_V1_STAGE_BYTES;
        uint32_t isa=get32(s+8),constants=get32(s+32),sds=get32(s+145*4);
        uint32_t isaoff=get32(s+149*4),coff=get32(s+150*4),soff=get32(s+151*4);
        TRY(wire_stage_valid(s,count==1 ? A840_STAGE_COMPUTE : i));
        if(isaoff!=at || isaoff%128 || !within(v->bytes,at,(uint64_t)isa*4))return A840_PACK_FORMAT;
        if(!add_u32(&at,(uint64_t)isa*4))return A840_PACK_RANGE;
        if((constants ? coff!=at : coff!=0) || !within(v->bytes,at,(uint64_t)constants*4))
            return A840_PACK_FORMAT;
        if(!add_u32(&at,(uint64_t)constants*4))return A840_PACK_RANGE;
        if((sds ? soff!=at : soff!=0) || !within(v->bytes,at,(uint64_t)sds*4))
            return A840_PACK_FORMAT;
        if(!add_u32(&at,(uint64_t)sds*4))return A840_PACK_RANGE;
        {
            uint32_t aligned=at;
            if(!align_u32(&aligned,128) || !within(v->bytes,at,aligned-at) ||
               !zero_bytes(v->base+at,aligned-at))return A840_PACK_FORMAT;
            at=aligned;
        }
    }
    if(total!=at-start || at>v->bytes)return A840_PACK_FORMAT;
    *cursor=at;
    return A840_PACK_OK;
}
static int view_valid(const A840PackV1View *v)
{
    return valid_region(v,sizeof(*v),_Alignof(A840PackV1View)) && v->valid==VIEW_VALID &&
        valid_region(v->base,v->bytes,128) && v->bytes<=UINT32_MAX &&
        v->entry_count<=A840_PACK_V1_MAX_ENTRIES && v->hash_bucket_count>=2 &&
        v->hash_bucket_count<=A840_PACK_V1_MAX_BUCKETS &&
        (v->hash_bucket_count&(v->hash_bucket_count-1))==0 &&
        within(v->bytes,64,(uint64_t)v->hash_bucket_count*4) &&
        within(v->bytes,v->directory_offset,(uint64_t)v->entry_count*A840_PACK_V1_ENTRY_BYTES);
}
A840PackV1Status a840_pack_v1_open_memory(const void *base,size_t bytes,
    uint64_t stack,uint64_t abi,A840PackV1View *out)
{
    const uint8_t *p=base;
    uint32_t log,version,tier,hbb,buckets,entries,directory,payload,cursor,i,occupied=0;
    uint8_t digest[32];
    A840PackV1View v;
    if(!valid_region(out,sizeof(*out),_Alignof(A840PackV1View)) ||
       !stack || !abi || !valid_region(base,bytes,1))return A840_PACK_INVALID_ARG;
    if(overlaps(out,sizeof(*out),base,bytes))return A840_PACK_ALIAS;
    if((uintptr_t)base%128)return A840_PACK_FORMAT;
    if(bytes<128 || bytes>UINT32_MAX)return A840_PACK_LENGTH;
    if(!little_endian())return A840_PACK_UNSUPPORTED;
    if(get32(p)!=A840_PACK_V1_MAGIC)return A840_PACK_MAGIC;
    version=p[4]&3u;tier=(p[4]>>2)&1u;hbb=p[4]>>3;log=p[5];
    if(version!=A840_PACK_V1_VERSION)return A840_PACK_VERSION;
    if(hbb!=A840_PACK_V1_HBB)return A840_PACK_HBB_MISMATCH;
    if(abi!=a840_compile_abi_fingerprint() || get64(p+8)!=abi)return A840_PACK_ABI_MISMATCH;
    if(get64(p+16)!=stack)return A840_PACK_STACK_MISMATCH;
    if(get32(p+24)!=A840_PACK_V1_CHIP_ID)return A840_PACK_CHIP_MISMATCH;
    if(log<1 || log>16)return A840_PACK_FORMAT;
    buckets=UINT32_C(1)<<log;entries=get16(p+6);
    if(entries>A840_PACK_V1_MAX_ENTRIES || entries>buckets/2)return A840_PACK_FORMAT;
    directory=align_to(64+buckets*4,128);
    payload=align_to(directory+entries*A840_PACK_V1_ENTRY_BYTES,128);
    if(payload>bytes || get32(p+28)!=bytes-payload)return A840_PACK_LENGTH;
    file_digest(p,bytes,digest);
    if(compare_bytes(digest,p+32,32))return A840_PACK_HASH_MISMATCH;
    if(!zero_bytes(p+64+buckets*4,directory-64-buckets*4) ||
       !zero_bytes(p+directory+entries*A840_PACK_V1_ENTRY_BYTES,
                   payload-directory-entries*A840_PACK_V1_ENTRY_BYTES))return A840_PACK_FORMAT;
    v.base=p;v.bytes=bytes;v.stack_key=stack;v.abi_fingerprint=abi;v.tier=tier;
    v.entry_count=entries;v.hash_bucket_count=buckets;v.directory_offset=directory;
    v.payload_offset=payload;v.valid=VIEW_VALID;
    cursor=payload;
    for(i=0;i<entries;++i) {
        const uint8_t *e=p+directory+(size_t)i*A840_PACK_V1_ENTRY_BYTES;
        if(i && compare_bytes(e-A840_PACK_V1_ENTRY_BYTES,e,32)>=0)return A840_PACK_FORMAT;
        TRY(canonical_entry(&v,i,&cursor));
    }
    if(cursor!=bytes)return A840_PACK_LENGTH;
    for(i=0;i<buckets;++i) {
        uint32_t n=get32(p+64+(size_t)i*4);
        if(n!=A840_PACK_V1_EMPTY) {
            if(n>=entries)return A840_PACK_FORMAT;
            ++occupied;
        }
    }
    if(occupied!=entries)return A840_PACK_FORMAT;
    for(i=0;i<entries;++i) {
        const uint8_t *key=p+directory+(size_t)i*A840_PACK_V1_ENTRY_BYTES;
        uint32_t bucket=(uint32_t)key_hash(key)&(buckets-1),probe,found=0;
        for(probe=0;probe<A840_PACK_V1_MAX_PROBES;++probe) {
            uint32_t n=get32(p+64+(size_t)bucket*4);
            if(n==A840_PACK_V1_EMPTY)break;
            if(n==i){found=1;break;}
            bucket=(bucket+1)&(buckets-1);
        }
        if(!found)return A840_PACK_FORMAT;
    }
    out->base=v.base;out->bytes=v.bytes;out->stack_key=v.stack_key;out->abi_fingerprint=v.abi_fingerprint;
    out->tier=v.tier;out->entry_count=v.entry_count;out->hash_bucket_count=v.hash_bucket_count;
    out->directory_offset=v.directory_offset;out->payload_offset=v.payload_offset;out->valid=v.valid;
    return A840_PACK_OK;
}
static void entry_pointer(const A840PackV1View *v,uint32_t index,A840PackV1Entry *out)
{
    const uint8_t *e=v->base+v->directory_offset+(size_t)index*A840_PACK_V1_ENTRY_BYTES;
    uint32_t count=get32(e+36),meta=get32(e+40),i;
    out->pipeline_key=(const A840PipelineKey *)(const void *)e;
    out->flags=get32(e+32);out->stage_count=count;out->binding_count=get32(e+48);
    out->bindings=out->binding_count ? (const A840RecordBinding *)(const void *)(v->base+get32(e+44)) : NULL;
    out->binding_plan=(const A840BindingPlan *)(const void *)(v->base+get32(e+52));
    for(i=0;i<2;++i) {
        A840PackV1StageView *stage=&out->stages[i];
        stage->metadata=NULL;stage->isa_words=NULL;stage->const_data_words=NULL;stage->sds_state_words=NULL;
        stage->stage=0;stage->isa_dwords=0;stage->const_data_dwords=0;stage->sds_state_dwords=0;
        stage->instruction_groups=0;stage->flags_out=0;
        if(i<count) {
            const uint8_t *s=v->base+meta+(size_t)i*A840_PACK_V1_STAGE_BYTES;
            stage->metadata=(const A840PackV1StageWire *)(const void *)s;
            stage->stage=get32(s+4);stage->isa_dwords=get32(s+8);
            stage->const_data_dwords=get32(s+32);stage->sds_state_dwords=get32(s+145*4);
            stage->instruction_groups=get32(s+12);stage->flags_out=get32(s+56);
            stage->isa_words=(const uint32_t *)(const void *)(v->base+get32(s+149*4));
            stage->const_data_words=stage->const_data_dwords ?
                (const uint32_t *)(const void *)(v->base+get32(s+150*4)) : NULL;
            stage->sds_state_words=stage->sds_state_dwords ?
                (const uint32_t *)(const void *)(v->base+get32(s+151*4)) : NULL;
        }
    }
}
A840PackV1Status a840_pack_v1_entry_at(const A840PackV1View *v,uint32_t index,A840PackV1Entry *out)
{
    if(!view_valid(v) || !valid_region(out,sizeof(*out),_Alignof(A840PackV1Entry)))
        return A840_PACK_INVALID_ARG;
    if(overlaps(out,sizeof(*out),v->base,v->bytes) || overlaps(out,sizeof(*out),v,sizeof(*v)))
        return A840_PACK_ALIAS;
    if(index>=v->entry_count)return A840_PACK_NOT_FOUND;
    entry_pointer(v,index,out);return A840_PACK_OK;
}
A840PackV1Status a840_pack_v1_lookup(const A840PackV1View *v,
    const A840PipelineKey *key,A840PackV1Entry *out)
{
    uint32_t bucket,probe;
    if(!view_valid(v) || !key_valid(key) ||
       !valid_region(out,sizeof(*out),_Alignof(A840PackV1Entry)))return A840_PACK_INVALID_ARG;
    if(overlaps(out,sizeof(*out),v->base,v->bytes) || overlaps(out,sizeof(*out),v,sizeof(*v)) ||
       overlaps(out,sizeof(*out),key,sizeof(*key)))return A840_PACK_ALIAS;
    bucket=(uint32_t)key_hash(key->sha256)&(v->hash_bucket_count-1);
    for(probe=0;probe<A840_PACK_V1_MAX_PROBES;++probe) {
        uint32_t index=get32(v->base+64+(size_t)bucket*4);
        const uint8_t *stored;
        if(index==A840_PACK_V1_EMPTY)return A840_PACK_NOT_FOUND;
        if(index>=v->entry_count)return A840_PACK_FORMAT;
        stored=v->base+v->directory_offset+(size_t)index*A840_PACK_V1_ENTRY_BYTES;
        if(!compare_bytes(stored,key->sha256,32)) {
            entry_pointer(v,index,out);return A840_PACK_OK;
        }
        bucket=(bucket+1)&(v->hash_bucket_count-1);
    }
    return A840_PACK_NOT_FOUND;
}
A840PackV1Status a840_pack_v1_entry_record(const A840PackV1View *v,
    const A840PackV1Entry *entry,A840PipelineRecord *out)
{
    uint32_t i,j;
    if(!view_valid(v) || !valid_region(entry,sizeof(*entry),_Alignof(A840PackV1Entry)) ||
       !valid_region(out,sizeof(*out),_Alignof(A840PipelineRecord)) ||
       !key_valid(entry->pipeline_key) || entry->stage_count<1 || entry->stage_count>2 ||
       entry->binding_count>A840_RECORD_MAX_BINDINGS)return A840_PACK_INVALID_ARG;
    if(overlaps(out,sizeof(*out),v->base,v->bytes) || overlaps(out,sizeof(*out),entry,sizeof(*entry)))
        return A840_PACK_ALIAS;
    zeros((uint8_t *)out,sizeof(*out));
    out->abi_fingerprint=v->abi_fingerprint;out->stack_key=v->stack_key;
    out->chip_id=A840_PACK_V1_CHIP_ID;out->highest_bank_bit=A840_PACK_V1_HBB;
    out->flags=entry->flags;out->stage_count=entry->stage_count;out->binding_count=entry->binding_count;
    copy_bytes(out->pipeline_key.sha256,entry->pipeline_key->sha256,32);
    for(i=0;i<entry->stage_count;++i) {
        const uint8_t *p=(const uint8_t *)(const void *)entry->stages[i].metadata;
        A840CompiledStage *s=&out->stages[i];
        if(!valid_region(p,A840_PACK_V1_STAGE_BYTES,4))return A840_PACK_INVALID_ARG;
        s->abi_version=get32(p);s->stage=get32(p+4);s->isa_dwords=get32(p+8);
        s->instruction_groups=get32(p+12);s->max_full_reg=signed_word(get32(p+16));
        s->max_half_reg=signed_word(get32(p+20));s->branchstack=get32(p+24);
        s->constlen_vec4=get32(p+28);s->const_data_dwords=get32(p+32);
        s->private_mem_bytes=get32(p+36);s->shared_mem_bytes=get32(p+40);
        for(j=0;j<3;++j)s->local_size[j]=get32(p+(11+j)*4);
        s->flags_out=get32(p+56);s->ubo_push_count=get32(p+60);
        for(j=0;j<ADRENO_COMPILE_MAX_UBO_PUSH;++j) {
            A840UboPushRange *u=&s->ubo_push[j];const uint8_t *b=p+(16+j*6)*4;
            u->set_index=get32(b);u->binding_index=get32(b+4);u->src_offset_vec4=get32(b+8);
            u->dst_const_vec4=get32(b+12);u->size_vec4=get32(b+16);u->reserved=get32(b+20);
        }
        s->vpc_varying_count=get32(p+112*4);
        for(j=0;j<ADRENO_COMPILE_MAX_VARYINGS;++j)s->vpc_varyings[j]=get32(p+(113+j)*4);
        s->sds_state_dwords=get32(p+145*4);s->reg_spills=get32(p+146*4);
        s->reg_fills=get32(p+147*4);s->max_waves=get32(p+148*4);
        s->isa_words=entry->stages[i].isa_words;s->const_data_words=entry->stages[i].const_data_words;
        s->sds_state_words=entry->stages[i].sds_state_words;
    }
    for(i=0;i<entry->binding_count;++i) {
        const A840RecordBinding *b=&entry->bindings[i];
        out->bindings[i].descriptor_set=b->descriptor_set;out->bindings[i].binding=b->binding;
        out->bindings[i].descriptor_type=b->descriptor_type;out->bindings[i].descriptor_count=b->descriptor_count;
        out->bindings[i].stage_mask=b->stage_mask;out->bindings[i].buffer_index=b->buffer_index;
        out->bindings[i].texture_index=b->texture_index;out->bindings[i].sampler_index=b->sampler_index;
    }
    return A840_PACK_OK;
}


static int item_compare(const void *left,const void *right)
{
    const A840PackV1BuilderItem *a=left,*b=right;
    return compare_bytes(a->record->pipeline_key.sha256,b->record->pipeline_key.sha256,32);
}
static int records_equal(const A840PipelineRecord *a,const A840BindingPlan *ap,
    const A840PipelineRecord *b,const A840BindingPlan *bp)
{
    uint32_t i,j;
    if(a->stage_count!=b->stage_count || a->binding_count!=b->binding_count || a->flags!=b->flags ||
       compare_bytes(ap,bp,sizeof(*ap)) ||
       compare_bytes(a->bindings,b->bindings,(size_t)a->binding_count*sizeof(A840RecordBinding)))return 0;
    for(i=0;i<a->stage_count;++i) {
        const A840CompiledStage *x=&a->stages[i],*y=&b->stages[i];
        for(j=0;j<META_WORDS;++j)if(metadata_word(x,j)!=metadata_word(y,j))return 0;
        if(compare_bytes(x->isa_words,y->isa_words,(size_t)x->isa_dwords*4) ||
           compare_bytes(x->const_data_words,y->const_data_words,(size_t)x->const_data_dwords*4) ||
           compare_bytes(x->sds_state_words,y->sds_state_words,(size_t)x->sds_state_dwords*4))return 0;
    }
    return 1;
}
static int builder_valid(const A840PackV1Builder *b)
{
    return valid_region(b,sizeof(*b),_Alignof(A840PackV1Builder)) && b->tier<=1 &&
        b->stack_key && b->abi_fingerprint && b->count<=b->capacity &&
        b->capacity<=A840_PACK_V1_MAX_ENTRIES && b->bucket_capacity>=2 &&
        b->bucket_capacity<=A840_PACK_V1_MAX_BUCKETS &&
        (!b->capacity || valid_region(b->items,b->capacity*sizeof(*b->items),_Alignof(A840PackV1BuilderItem))) &&
        valid_region(b->buckets,b->bucket_capacity*sizeof(*b->buckets),_Alignof(uint32_t));
}
A840PackV1Status a840_pack_v1_builder_init(A840PackV1Builder *b,uint32_t tier,
    uint64_t stack,uint64_t abi,A840PackV1BuilderItem *items,size_t capacity,
    uint32_t *buckets,size_t bucket_capacity)
{
    if(!valid_region(b,sizeof(*b),_Alignof(A840PackV1Builder)) || !stack || !abi ||
       capacity>A840_PACK_V1_MAX_ENTRIES || tier>1 || bucket_capacity<2 ||
       bucket_capacity>A840_PACK_V1_MAX_BUCKETS ||
       (bucket_capacity&(bucket_capacity-1)) ||
       (capacity && !valid_region(items,capacity*sizeof(*items),_Alignof(A840PackV1BuilderItem))) ||
       !valid_region(buckets,bucket_capacity*4,4))return A840_PACK_INVALID_ARG;
    if(abi!=a840_compile_abi_fingerprint())return A840_PACK_ABI_MISMATCH;
    if(overlaps(b,sizeof(*b),items,capacity*sizeof(*items)) ||
       overlaps(b,sizeof(*b),buckets,bucket_capacity*4) ||
       overlaps(items,capacity*sizeof(*items),buckets,bucket_capacity*4))return A840_PACK_ALIAS;
    b->items=items;b->buckets=buckets;b->capacity=capacity;b->bucket_capacity=bucket_capacity;b->count=0;
    b->stack_key=stack;b->abi_fingerprint=abi;b->tier=tier;b->bucket_count=0;
    b->directory_offset=0;b->payload_offset=0;b->total_bytes=0;b->ready=0;
    return A840_PACK_OK;
}
A840PackV1Status a840_pack_v1_builder_add(A840PackV1Builder *b,
    const A840PipelineRecord *r,const A840BindingPlan *p)
{
    size_t i;
    if(!builder_valid(b))return A840_PACK_INVALID_ARG;
    TRY(record_valid(r,p,b->stack_key,b->abi_fingerprint));
    if(overlaps(r,sizeof(*r),b,sizeof(*b)) || overlaps(p,sizeof(*p),b,sizeof(*b)) ||
       overlaps(r,sizeof(*r),b->items,b->capacity*sizeof(*b->items)) ||
       overlaps(p,sizeof(*p),b->items,b->capacity*sizeof(*b->items)) ||
       overlaps(r,sizeof(*r),b->buckets,b->bucket_capacity*4) ||
       overlaps(p,sizeof(*p),b->buckets,b->bucket_capacity*4))return A840_PACK_ALIAS;
    for(i=0;i<b->count;++i)if(!compare_bytes(b->items[i].record->pipeline_key.sha256,r->pipeline_key.sha256,32))
        return records_equal(b->items[i].record,b->items[i].plan,r,p) ? A840_PACK_OK : A840_PACK_KEY_CONFLICT;
    if(b->count==b->capacity)return A840_PACK_CAPACITY;
    b->items[b->count].record=r;b->items[b->count].plan=p;++b->count;b->ready=0;
    return A840_PACK_OK;
}
A840PackV1Status a840_pack_v1_builder_measure(A840PackV1Builder *b,size_t *out_bytes)
{
    uint32_t buckets=2,cursor;size_t i;
    if(!builder_valid(b) || !valid_region(out_bytes,sizeof(*out_bytes),_Alignof(size_t)))
        return A840_PACK_INVALID_ARG;
    if(overlaps(out_bytes,sizeof(*out_bytes),b,sizeof(*b)) ||
       overlaps(out_bytes,sizeof(*out_bytes),b->items,b->capacity*sizeof(*b->items)) ||
       overlaps(out_bytes,sizeof(*out_bytes),b->buckets,b->bucket_capacity*4))return A840_PACK_ALIAS;
    for(i=0;i<b->count;++i)TRY(record_valid(b->items[i].record,b->items[i].plan,b->stack_key,b->abi_fingerprint));
    if(b->count>1)qsort(b->items,b->count,sizeof(*b->items),item_compare);
    while(buckets<b->count*2)buckets*=2;
    for(;;) {
        int collision=0;
        if(buckets>b->bucket_capacity)return A840_PACK_CAPACITY;
        for(i=0;i<buckets;++i)b->buckets[i]=A840_PACK_V1_EMPTY;
        for(i=0;i<b->count;++i) {
            uint32_t bucket=(uint32_t)key_hash(b->items[i].record->pipeline_key.sha256)&(buckets-1),probe;
            for(probe=0;probe<A840_PACK_V1_MAX_PROBES;++probe) {
                if(b->buckets[bucket]==A840_PACK_V1_EMPTY){b->buckets[bucket]=(uint32_t)i;break;}
                bucket=(bucket+1)&(buckets-1);
            }
            if(probe==A840_PACK_V1_MAX_PROBES){collision=1;break;}
        }
        if(!collision)break;
        if(buckets==A840_PACK_V1_MAX_BUCKETS)return A840_PACK_CAPACITY;
        buckets*=2;
    }
    b->bucket_count=buckets;b->directory_offset=align_to(64+buckets*4,128);
    b->payload_offset=align_to(b->directory_offset+(uint32_t)b->count*A840_PACK_V1_ENTRY_BYTES,128);
    cursor=b->payload_offset;
    for(i=0;i<b->count;++i) {
        A840PackV1BuilderItem *item=&b->items[i];
        const A840PipelineRecord *r=item->record;
        uint32_t stage;
        item->payload_offset=cursor;item->metadata_offset=cursor;
        if(!add_u32(&cursor,(uint64_t)r->stage_count*A840_PACK_V1_STAGE_BYTES))return A840_PACK_RANGE;
        item->binding_offset=r->binding_count ? cursor : 0;
        if(!add_u32(&cursor,(uint64_t)r->binding_count*sizeof(A840RecordBinding)) ||
           !align_u32(&cursor,64))return A840_PACK_RANGE;
        item->plan_offset=cursor;
        if(!add_u32(&cursor,sizeof(A840BindingPlan)) || !align_u32(&cursor,128))return A840_PACK_RANGE;
        for(stage=0;stage<r->stage_count;++stage) {
            const A840CompiledStage *s=&r->stages[stage];
            item->isa_offsets[stage]=cursor;
            if(!add_u32(&cursor,(uint64_t)s->isa_dwords*4))return A840_PACK_RANGE;
            item->const_offsets[stage]=s->const_data_dwords ? cursor : 0;
            if(!add_u32(&cursor,(uint64_t)s->const_data_dwords*4))return A840_PACK_RANGE;
            item->sds_offsets[stage]=s->sds_state_dwords ? cursor : 0;
            if(!add_u32(&cursor,(uint64_t)s->sds_state_dwords*4) || !align_u32(&cursor,128))
                return A840_PACK_RANGE;
        }
        item->payload_bytes=cursor-item->payload_offset;
    }
    b->total_bytes=cursor;b->ready=1;*out_bytes=cursor;
    return A840_PACK_OK;
}
static int source_overlap(const A840PackV1Builder *b,const void *buffer,size_t bytes)
{
    size_t i;uint32_t stage;
    if(overlaps(buffer,bytes,b,sizeof(*b)) ||
       overlaps(buffer,bytes,b->items,b->capacity*sizeof(*b->items)) ||
       overlaps(buffer,bytes,b->buckets,b->bucket_capacity*4))return 1;
    for(i=0;i<b->count;++i) {
        const A840PipelineRecord *r=b->items[i].record;
        if(overlaps(buffer,bytes,r,sizeof(*r)) || overlaps(buffer,bytes,b->items[i].plan,sizeof(A840BindingPlan)))return 1;
        for(stage=0;stage<r->stage_count;++stage) {
            const A840CompiledStage *s=&r->stages[stage];
            if(overlaps(buffer,bytes,s->isa_words,(size_t)s->isa_dwords*4) ||
               overlaps(buffer,bytes,s->const_data_words,(size_t)s->const_data_dwords*4) ||
               overlaps(buffer,bytes,s->sds_state_words,(size_t)s->sds_state_dwords*4))return 1;
        }
    }
    return 0;
}
A840PackV1Status a840_pack_v1_builder_build(A840PackV1Builder *b,
    void *buffer,size_t capacity,size_t *out_bytes)
{
    uint8_t *p=buffer;uint32_t log=0;size_t bytes,i;uint32_t j,stage;
    if(!builder_valid(b) || !valid_region(buffer,capacity,128) ||
       !valid_region(out_bytes,sizeof(*out_bytes),_Alignof(size_t)))return A840_PACK_INVALID_ARG;
    for(i=0;i<b->count;++i)TRY(record_valid(b->items[i].record,b->items[i].plan,b->stack_key,b->abi_fingerprint));
    if(source_overlap(b,buffer,capacity) || overlaps(out_bytes,sizeof(*out_bytes),buffer,capacity))
        return A840_PACK_ALIAS;
    TRY(a840_pack_v1_builder_measure(b,&bytes));
    if(capacity<bytes)return A840_PACK_CAPACITY;
    zeros(p,bytes);
    put32(p,A840_PACK_V1_MAGIC);
    p[4]=(uint8_t)(A840_PACK_V1_VERSION | b->tier<<2 | A840_PACK_V1_HBB<<3);
    while((UINT32_C(1)<<log)<b->bucket_count)++log;
    p[5]=(uint8_t)log;put16(p+6,(uint16_t)b->count);
    put64(p+8,b->abi_fingerprint);put64(p+16,b->stack_key);put32(p+24,A840_PACK_V1_CHIP_ID);
    put32(p+28,(uint32_t)bytes-b->payload_offset);
    for(i=0;i<b->bucket_count;++i)put32(p+64+i*4,b->buckets[i]);
    for(i=0;i<b->count;++i) {
        const A840PackV1BuilderItem *item=&b->items[i];
        const A840PipelineRecord *r=item->record;
        uint8_t *e=p+b->directory_offset+i*A840_PACK_V1_ENTRY_BYTES;
        copy_bytes(e,r->pipeline_key.sha256,32);
        put32(e+32,r->flags);put32(e+36,r->stage_count);put32(e+40,item->metadata_offset);
        put32(e+44,item->binding_offset);put32(e+48,r->binding_count);
        put32(e+52,item->plan_offset);put32(e+56,sizeof(A840BindingPlan));
        put32(e+60,item->payload_offset);put32(e+64,item->payload_bytes);
        for(j=0;j<r->binding_count;++j) {
            const A840RecordBinding *v=&r->bindings[j];uint8_t *q=p+item->binding_offset+j*32;
            const uint32_t values[8]={v->descriptor_set,v->binding,v->descriptor_type,v->descriptor_count,
                v->stage_mask,v->buffer_index,v->texture_index,v->sampler_index};
            uint32_t n;for(n=0;n<8;++n)put32(q+n*4,values[n]);
        }
        for(j=0;j<PLAN_WORDS;++j) {
            const uint8_t *native=(const uint8_t *)(const void *)item->plan+j*4;
            uint32_t value=little_endian() ? get32(native) :
                (uint32_t)native[0]<<24 | (uint32_t)native[1]<<16 | (uint32_t)native[2]<<8 | native[3];
            put32(p+item->plan_offset+j*4,value);
        }
        for(stage=0;stage<r->stage_count;++stage) {
            const A840CompiledStage *s=&r->stages[stage];
            uint8_t *meta=p+item->metadata_offset+stage*A840_PACK_V1_STAGE_BYTES;
            for(j=0;j<META_WORDS;++j)put32(meta+j*4,metadata_word(s,j));
            put32(meta+149*4,item->isa_offsets[stage]);put32(meta+150*4,item->const_offsets[stage]);
            put32(meta+151*4,item->sds_offsets[stage]);
            for(j=0;j<s->isa_dwords;++j)put32(p+item->isa_offsets[stage]+(size_t)j*4,s->isa_words[j]);
            for(j=0;j<s->const_data_dwords;++j)put32(p+item->const_offsets[stage]+(size_t)j*4,s->const_data_words[j]);
            for(j=0;j<s->sds_state_dwords;++j)put32(p+item->sds_offsets[stage]+(size_t)j*4,s->sds_state_words[j]);
        }
    }
    file_digest(p,bytes,p+32);
    *out_bytes=bytes;return A840_PACK_OK;
}
static int sync_fd(int fd)
{ int result;do{result=fsync(fd);}while(result<0 && errno==EINTR);return result; }
A840PackV1Status a840_pack_v1_save_file(const char *path,const void *buffer,size_t bytes,
    uint64_t stack,uint64_t abi)
{
    char parent[PATH_MAX],resolved[PATH_MAX],name[PATH_MAX],temporary[PATH_MAX];
    size_t length=0,slash=0,i,at=0;int dirfd=-1,fd=-1;unsigned attempt;
    static _Atomic uint64_t sequence=0;A840PackV1View view;
    A840PackV1Status status;
    TRY(a840_pack_v1_open_memory(buffer,bytes,stack,abi,&view));
    if(!path)return A840_PACK_INVALID_ARG;
    while(length<PATH_MAX && path[length])++length;
    if(!length || length==PATH_MAX)return A840_PACK_RANGE;
    for(i=0;i<length;++i) {
        if(path[i]=='\\')return A840_PACK_INVALID_ARG;
        if(path[i]=='/')slash=i;
    }
    if(!slash || slash==length-1)return A840_PACK_INVALID_ARG;
    for(i=0;i<slash;++i)parent[i]=path[i];
    parent[slash]=0;
    for(i=slash+1;i<=length;++i)name[i-slash-1]=path[i];
    if(!realpath(parent,resolved))return A840_PACK_IO;
    for(i=0;CACHE_ROOT[i];++i)if(resolved[i]!=CACHE_ROOT[i])return A840_PACK_INVALID_ARG;
    if(!name[0] || (name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2]))))return A840_PACK_INVALID_ARG;
    dirfd=open(resolved,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(dirfd<0)return A840_PACK_IO;
    for(attempt=0;attempt<64;++attempt) {
        uint64_t n=atomic_fetch_add_explicit(&sequence,1,memory_order_relaxed);
        int count=snprintf(temporary,sizeof(temporary),".a840-pack.tmp.%ld.%llu",
            (long)getpid(),(unsigned long long)n);
        if(count<0 || (size_t)count>=sizeof(temporary)){close(dirfd);return A840_PACK_RANGE;}
        fd=openat(dirfd,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0)break;
        if(errno!=EEXIST){close(dirfd);return A840_PACK_IO;}
    }
    if(fd<0){close(dirfd);return A840_PACK_IO;}
    status=A840_PACK_OK;
    while(at<bytes) {
        ssize_t n=write(fd,(const uint8_t *)buffer+at,bytes-at);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){status=A840_PACK_IO;break;}
        at+=(size_t)n;
    }
    if(status==A840_PACK_OK && sync_fd(fd)<0)status=A840_PACK_IO;
    if(close(fd)<0)status=A840_PACK_IO;
    if(status==A840_PACK_OK && renameat(dirfd,temporary,dirfd,name)<0)status=A840_PACK_IO;
    if(status!=A840_PACK_OK)(void)unlinkat(dirfd,temporary,0);
    else if(sync_fd(dirfd)<0)status=A840_PACK_IO;
    close(dirfd);return status;
}
const char *a840_pack_v1_status_string(A840PackV1Status status)
{
    static const char *const text[]={"OK","INVALID_ARG","RANGE","CAPACITY","MAGIC","VERSION","LENGTH",
        "ABI_MISMATCH","STACK_MISMATCH","CHIP_MISMATCH","HBB_MISMATCH","HASH_MISMATCH",
        "FORMAT","NOT_FOUND","ALIAS","IO","UNSUPPORTED","KEY_CONFLICT"};
    return (unsigned)status<sizeof(text)/sizeof(text[0]) ? text[status] : "UNKNOWN";
}
#undef TRY
