/* SPDX-License-Identifier: MIT */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "a840_record_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STAGE_META_WORDS (21u + 6u * ADRENO_COMPILE_MAX_UBO_PUSH + ADRENO_COMPILE_MAX_VARYINGS)
#define NO_SLOT SIZE_MAX

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
    memcpy(s->h,init,sizeof(init));s->bytes=0;s->used=0;
}
static void sha_update(Sha256 *s, const void *data, size_t bytes)
{
    const uint8_t *p=data;
    s->bytes+=bytes;
    while (bytes) {
        size_t n=sizeof(s->block)-s->used;
        if (n>bytes) n=bytes;
        memcpy(s->block+s->used,p,n);s->used+=n;p+=n;bytes-=n;
        if (s->used==sizeof(s->block)) {sha_block(s,s->block);s->used=0;}
    }
}
static void sha_finish(Sha256 *s, uint8_t out[32])
{
    uint64_t bits=s->bytes*8;
    size_t i;
    s->block[s->used++]=0x80;
    if (s->used>56) {memset(s->block+s->used,0,64-s->used);sha_block(s,s->block);s->used=0;}
    memset(s->block+s->used,0,56-s->used);
    for (i=0;i<8;++i) s->block[63-i]=(uint8_t)(bits>>(i*8));
    sha_block(s,s->block);
    for (i=0;i<32;++i) out[i]=(uint8_t)(s->h[i/4]>>(24-(i%4)*8));
}

static int region_valid(const void *p, size_t bytes, size_t alignment)
{
    uintptr_t at=(uintptr_t)p;
    return p && at%alignment==0 && bytes<=UINTPTR_MAX-at;
}
static int overlap(const void *a, size_t a_bytes, const void *b, size_t b_bytes)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a_bytes && b_bytes && (x<=y ? y-x<a_bytes : x-y<b_bytes);
}

A840RecordStatus a840_record_sha256(const void *data, size_t bytes, uint8_t out[32])
{
    Sha256 s;uint8_t digest[32];
    if (!region_valid(out,32,1) || (bytes && !region_valid(data,bytes,1))) return A840_RECORD_INVALID_ARG;
    if (bytes>UINT64_MAX/8) return A840_RECORD_RANGE_ERROR;
    sha_init(&s);sha_update(&s,data,bytes);sha_finish(&s,digest);memcpy(out,digest,32);
    return A840_RECORD_OK;
}

static void put32(uint8_t *p,uint32_t v)
{size_t i;for(i=0;i<4;++i)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v)
{size_t i;for(i=0;i<8;++i)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p)
{return (uint64_t)get32(p) | (uint64_t)get32(p+4)<<32;}
static int32_t signed32(uint32_t v)
{return v<=INT32_MAX ? (int32_t)v : -(int32_t)(UINT32_MAX-v)-1;}
static void sha32(Sha256 *s,uint32_t v) {uint8_t p[4];put32(p,v);sha_update(s,p,4);}
static int nonzero_key(const A840PipelineKey *key)
{uint8_t value=0;size_t i;for(i=0;i<32;++i)value|=key->sha256[i];return value!=0;}

A840RecordStatus a840_record_compute_key(const A840RecordKeyStage *stages,
    uint32_t count,A840PipelineKey *out)
{
    Sha256 hash;A840PipelineKey key;uint32_t i,j;
    if (count<1 || count>2 || !region_valid(stages,count*sizeof(*stages),_Alignof(A840RecordKeyStage)) ||
        !region_valid(out,sizeof(*out),_Alignof(A840PipelineKey))) return A840_RECORD_INVALID_ARG;
    if(overlap(stages,count*sizeof(*stages),out,sizeof(*out)))return A840_RECORD_ALIAS;
    if ((count==1 && stages[0].stage!=A840_STAGE_COMPUTE) ||
        (count==2 && (stages[0].stage!=A840_STAGE_VERTEX || stages[1].stage!=A840_STAGE_FRAGMENT)))
        return A840_RECORD_FORMAT_ERROR;
    sha_init(&hash);sha_update(&hash,"A840PipelineKey01",17);sha32(&hash,count);
    for(i=0;i<count;++i) {
        const A840RecordKeyStage *s=&stages[i];size_t len=0;
        if(!s->entry_point) return A840_RECORD_INVALID_ARG;
        while(len<4096 && s->entry_point[len])++len;
        if(!len || len==4096) return A840_RECORD_RANGE_ERROR;
        if(overlap(s->entry_point,len+1,out,sizeof(*out)))return A840_RECORD_ALIAS;
        if(s->sample_count!=1 && s->sample_count!=2 && s->sample_count!=4) return A840_RECORD_RANGE_ERROR;
        if((s->half_rt_mask&~s->color_rt_mask) || s->color_rt_mask>255) return A840_RECORD_RANGE_ERROR;
        sha32(&hash,(uint32_t)len);sha_update(&hash,s->entry_point,len);sha32(&hash,s->stage);
        sha_update(&hash,s->spirv_sha256,32);sha32(&hash,s->color_rt_mask);sha32(&hash,s->half_rt_mask);
        sha32(&hash,s->sample_count);for(j=0;j<3;++j)sha32(&hash,s->local_size[j]);
        sha32(&hash,s->flags);sha32(&hash,s->const_push_mask);
        for(j=0;j<ADRENO_COMPILE_MAX_CONST_PUSH_DWORDS;++j)sha32(&hash,s->const_push_dwords[j]);
    }
    sha_finish(&hash,key.sha256);*out=key;return A840_RECORD_OK;
}

static A840RecordStatus stage_metadata(const A840CompiledStage *s)
{
    if(s->abi_version!=ADRENO_COMPILE_ABI_VERSION) return A840_RECORD_ABI_MISMATCH;
    if(s->stage>A840_STAGE_COMPUTE || !s->isa_dwords || !s->instruction_groups ||
        s->max_full_reg < -1 || s->max_half_reg < -1) return A840_RECORD_FORMAT_ERROR;
    if(s->isa_dwords>A840_RECORD_MAX_ISA_DWORDS || s->const_data_dwords>A840_RECORD_MAX_CONST_DWORDS ||
        s->sds_state_dwords>A840_RECORD_MAX_SDS_DWORDS || s->ubo_push_count>ADRENO_COMPILE_MAX_UBO_PUSH ||
        s->vpc_varying_count>ADRENO_COMPILE_MAX_VARYINGS) return A840_RECORD_RANGE_ERROR;
    return A840_RECORD_OK;
}
static A840RecordStatus record_metadata(const A840PipelineRecord *r)
{
    uint32_t i,mask;
    if(r->abi_fingerprint!=a840_compile_abi_fingerprint()) return A840_RECORD_ABI_MISMATCH;
    if(!r->stack_key) return A840_RECORD_STACK_MISMATCH;
    if(r->chip_id!=A840_RECORD_CHIP_ID) return A840_RECORD_CHIP_MISMATCH;
    if(r->highest_bank_bit!=A840_RECORD_HBB) return A840_RECORD_HBB_MISMATCH;
    if(r->stage_count<1 || r->stage_count>2 || r->binding_count>A840_RECORD_MAX_BINDINGS)
        return A840_RECORD_RANGE_ERROR;
    if(!nonzero_key(&r->pipeline_key)) return A840_RECORD_FORMAT_ERROR;
    if((r->stage_count==1 && r->stages[0].stage!=A840_STAGE_COMPUTE) ||
        (r->stage_count==2 && (r->stages[0].stage!=A840_STAGE_VERTEX || r->stages[1].stage!=A840_STAGE_FRAGMENT)))
        return A840_RECORD_FORMAT_ERROR;
    mask=r->stage_count==1 ? 4u : 3u;
    for(i=0;i<r->binding_count;++i) {
        const A840RecordBinding *b=&r->bindings[i];
        if(b->descriptor_set>=ADRENO_COMPILE_MAX_BIND_SETS || !b->descriptor_count ||
            !b->stage_mask || (b->stage_mask&~mask)) return A840_RECORD_FORMAT_ERROR;
    }
    return A840_RECORD_OK;
}

static A840RecordStatus record_size(const A840PipelineRecord *r,size_t *bytes)
{
    uint32_t i;size_t n=A840_RECORD_PREFIX_BYTES;A840RecordStatus st;
    if(!region_valid(r,sizeof(*r),_Alignof(A840PipelineRecord))) return A840_RECORD_INVALID_ARG;
    st=record_metadata(r);if(st!=A840_RECORD_OK)return st;
    for(i=0;i<r->stage_count;++i) {
        const A840CompiledStage *s=&r->stages[i];
        size_t words;
        st=stage_metadata(s);if(st!=A840_RECORD_OK)return st;
        if(!region_valid(s->isa_words,(size_t)s->isa_dwords*4,_Alignof(uint32_t)) ||
            (s->const_data_dwords && !region_valid(s->const_data_words,(size_t)s->const_data_dwords*4,_Alignof(uint32_t))) ||
            (s->sds_state_dwords && !region_valid(s->sds_state_words,(size_t)s->sds_state_dwords*4,_Alignof(uint32_t))))
            return A840_RECORD_INVALID_ARG;
        words=(size_t)s->isa_dwords+s->const_data_dwords+s->sds_state_dwords+STAGE_META_WORDS;
        if(words>(A840_RECORD_MAX_BYTES-n)/4)return A840_RECORD_RANGE_ERROR;
        n+=words*4;
    }
    if(r->binding_count>(A840_RECORD_MAX_BYTES-n)/sizeof(A840RecordBinding)) return A840_RECORD_RANGE_ERROR;
    *bytes=n+(size_t)r->binding_count*sizeof(A840RecordBinding);return A840_RECORD_OK;
}

static void write_word(uint8_t **p,uint32_t v) {put32(*p,v);*p+=4;}
static void write_stage(uint8_t **p,const A840CompiledStage *s)
{
    uint32_t i;
    const uint32_t before[]={s->abi_version,s->stage,s->isa_dwords,s->instruction_groups,
        (uint32_t)s->max_full_reg,(uint32_t)s->max_half_reg,s->branchstack,s->constlen_vec4,
        s->const_data_dwords,s->private_mem_bytes,s->shared_mem_bytes,s->local_size[0],
        s->local_size[1],s->local_size[2],s->flags_out,s->ubo_push_count};
    for(i=0;i<sizeof(before)/sizeof(before[0]);++i)write_word(p,before[i]);
    for(i=0;i<ADRENO_COMPILE_MAX_UBO_PUSH;++i) {
        const A840UboPushRange *u=&s->ubo_push[i];
        write_word(p,u->set_index);write_word(p,u->binding_index);write_word(p,u->src_offset_vec4);
        write_word(p,u->dst_const_vec4);write_word(p,u->size_vec4);write_word(p,u->reserved);
    }
    write_word(p,s->vpc_varying_count);
    for(i=0;i<ADRENO_COMPILE_MAX_VARYINGS;++i)write_word(p,s->vpc_varyings[i]);
    write_word(p,s->sds_state_dwords);write_word(p,s->reg_spills);write_word(p,s->reg_fills);write_word(p,s->max_waves);
    for(i=0;i<s->isa_dwords;++i)write_word(p,s->isa_words[i]);
    for(i=0;i<s->const_data_dwords;++i)write_word(p,s->const_data_words[i]);
    for(i=0;i<s->sds_state_dwords;++i)write_word(p,s->sds_state_words[i]);
}
static void record_digest(const uint8_t *p,size_t bytes,uint8_t digest[32])
{
    static const uint8_t zeros[32]={0};Sha256 hash;
    sha_init(&hash);sha_update(&hash,p,96);sha_update(&hash,zeros,32);
    sha_update(&hash,p+128,bytes-128);sha_finish(&hash,digest);
}

A840RecordStatus a840_record_serialize(const A840PipelineRecord *r,uint8_t **out,size_t *out_bytes)
{
    A840RecordStatus st;uint8_t *p,*at;size_t bytes;uint32_t i;
    if(!region_valid(out,sizeof(*out),_Alignof(uint8_t *)) ||
        !region_valid(out_bytes,sizeof(*out_bytes),_Alignof(size_t)) ||
        overlap(out,sizeof(*out),out_bytes,sizeof(*out_bytes))) return A840_RECORD_INVALID_ARG;
    st=record_size(r,&bytes);if(st!=A840_RECORD_OK)return st;
    if(overlap(out,sizeof(*out),r,sizeof(*r)) || overlap(out_bytes,sizeof(*out_bytes),r,sizeof(*r)))
        return A840_RECORD_ALIAS;
    for(i=0;i<r->stage_count;++i) {
        const A840CompiledStage *s=&r->stages[i];
        const uint32_t *arrays[]={s->isa_words,s->const_data_words,s->sds_state_words};
        const uint32_t counts[]={s->isa_dwords,s->const_data_dwords,s->sds_state_dwords};
        uint32_t j;
        for(j=0;j<3;++j)
            if(overlap(out,sizeof(*out),arrays[j],(size_t)counts[j]*4) ||
                overlap(out_bytes,sizeof(*out_bytes),arrays[j],(size_t)counts[j]*4)) return A840_RECORD_ALIAS;
    }
    p=calloc(1,bytes);if(!p)return A840_RECORD_OOM;
    memcpy(p,"A840PR01",8);put32(p+8,A840_RECORD_VERSION);put32(p+12,A840_RECORD_PREFIX_BYTES);
    put64(p+16,bytes);put64(p+24,r->abi_fingerprint);put64(p+32,r->stack_key);put64(p+40,r->chip_id);
    put32(p+48,r->highest_bank_bit);put32(p+52,r->stage_count);put32(p+56,r->binding_count);put32(p+60,r->flags);
    memcpy(p+64,r->pipeline_key.sha256,32);at=p+128;
    for(i=0;i<r->stage_count;++i)write_stage(&at,&r->stages[i]);
    for(i=0;i<r->binding_count;++i) {
        const A840RecordBinding *b=&r->bindings[i];
        write_word(&at,b->descriptor_set);write_word(&at,b->binding);write_word(&at,b->descriptor_type);
        write_word(&at,b->descriptor_count);write_word(&at,b->stage_mask);write_word(&at,b->buffer_index);
        write_word(&at,b->texture_index);write_word(&at,b->sampler_index);
    }
    if((size_t)(at-p)!=bytes) {free(p);return A840_RECORD_FORMAT_ERROR;}
    record_digest(p,bytes,p+96);*out=p;*out_bytes=bytes;return A840_RECORD_OK;
}

typedef struct Cursor {const uint8_t *p;size_t bytes,at;} Cursor;
static uint32_t read_word(Cursor *c) {uint32_t v=get32(c->p+c->at);c->at+=4;return v;}
static A840RecordStatus read_stage(Cursor *c,A840CompiledStage *s,uint32_t **storage)
{
    uint32_t i;size_t words;A840RecordStatus st;
    if(STAGE_META_WORDS*4>c->bytes-c->at)return A840_RECORD_LENGTH_ERROR;
    s->abi_version=read_word(c);s->stage=read_word(c);s->isa_dwords=read_word(c);s->instruction_groups=read_word(c);
    s->max_full_reg=signed32(read_word(c));s->max_half_reg=signed32(read_word(c));s->branchstack=read_word(c);
    s->constlen_vec4=read_word(c);s->const_data_dwords=read_word(c);s->private_mem_bytes=read_word(c);
    s->shared_mem_bytes=read_word(c);for(i=0;i<3;++i)s->local_size[i]=read_word(c);
    s->flags_out=read_word(c);s->ubo_push_count=read_word(c);
    for(i=0;i<ADRENO_COMPILE_MAX_UBO_PUSH;++i) {
        A840UboPushRange *u=&s->ubo_push[i];
        u->set_index=read_word(c);u->binding_index=read_word(c);u->src_offset_vec4=read_word(c);
        u->dst_const_vec4=read_word(c);u->size_vec4=read_word(c);u->reserved=read_word(c);
    }
    s->vpc_varying_count=read_word(c);
    for(i=0;i<ADRENO_COMPILE_MAX_VARYINGS;++i)s->vpc_varyings[i]=read_word(c);
    s->sds_state_dwords=read_word(c);s->reg_spills=read_word(c);s->reg_fills=read_word(c);s->max_waves=read_word(c);
    st=stage_metadata(s);if(st!=A840_RECORD_OK)return st;
    words=(size_t)s->isa_dwords+s->const_data_dwords+s->sds_state_dwords;
    if(words>(c->bytes-c->at)/4)return A840_RECORD_LENGTH_ERROR;
    if(storage) {
        s->isa_words=*storage;for(i=0;i<s->isa_dwords;++i)*(*storage)++=read_word(c);
        s->const_data_words=s->const_data_dwords ? *storage : NULL;
        for(i=0;i<s->const_data_dwords;++i)*(*storage)++=read_word(c);
        s->sds_state_words=s->sds_state_dwords ? *storage : NULL;
        for(i=0;i<s->sds_state_dwords;++i)*(*storage)++=read_word(c);
    } else c->at+=words*4;
    return A840_RECORD_OK;
}
static A840RecordStatus parse_record(const uint8_t *p,size_t bytes,A840PipelineRecord *r,uint32_t *storage)
{
    Cursor c={p,bytes,A840_RECORD_PREFIX_BYTES};uint32_t i;A840RecordStatus st;
    r->abi_fingerprint=get64(p+24);r->stack_key=get64(p+32);r->chip_id=get64(p+40);
    r->highest_bank_bit=get32(p+48);r->stage_count=get32(p+52);r->binding_count=get32(p+56);r->flags=get32(p+60);
    memcpy(r->pipeline_key.sha256,p+64,32);
    if(r->stage_count<1 || r->stage_count>2 || r->binding_count>A840_RECORD_MAX_BINDINGS)
        return A840_RECORD_RANGE_ERROR;
    for(i=0;i<r->stage_count;++i) {
        st=read_stage(&c,&r->stages[i],storage ? &storage : NULL);if(st!=A840_RECORD_OK)return st;
    }
    if(r->binding_count>(c.bytes-c.at)/sizeof(A840RecordBinding))return A840_RECORD_LENGTH_ERROR;
    for(i=0;i<r->binding_count;++i) {
        A840RecordBinding *b=&r->bindings[i];
        b->descriptor_set=read_word(&c);b->binding=read_word(&c);b->descriptor_type=read_word(&c);
        b->descriptor_count=read_word(&c);b->stage_mask=read_word(&c);b->buffer_index=read_word(&c);
        b->texture_index=read_word(&c);b->sampler_index=read_word(&c);
    }
    if(c.at!=bytes)return A840_RECORD_LENGTH_ERROR;
    return record_metadata(r);
}

A840RecordStatus a840_record_deserialize(const uint8_t *p,size_t bytes,uint64_t stack,
    uint64_t abi,const A840PipelineKey *key,A840PipelineRecord *out)
{
    A840PipelineRecord record={0};A840RecordStatus st;uint8_t digest[32];size_t words=0;uint32_t i;
    if(!region_valid(out,sizeof(*out),_Alignof(A840PipelineRecord)) ||
        !stack || !abi || (bytes && !region_valid(p,bytes,1)) ||
        (key && !region_valid(key,sizeof(*key),1)))return A840_RECORD_INVALID_ARG;
    if(overlap(p,bytes,out,sizeof(*out)) || (key && overlap(key,sizeof(*key),out,sizeof(*out))))
        return A840_RECORD_ALIAS;
    if(bytes<A840_RECORD_PREFIX_BYTES || bytes>A840_RECORD_MAX_BYTES || bytes%4)return A840_RECORD_LENGTH_ERROR;
    if(memcmp(p,"A840PR01",8))return A840_RECORD_MAGIC_ERROR;
    if(get32(p+8)!=A840_RECORD_VERSION || get32(p+12)!=A840_RECORD_PREFIX_BYTES)return A840_RECORD_VERSION_ERROR;
    if(get64(p+16)!=bytes)return A840_RECORD_LENGTH_ERROR;
    if(abi!=a840_compile_abi_fingerprint() || get64(p+24)!=abi)return A840_RECORD_ABI_MISMATCH;
    if(get64(p+32)!=stack)return A840_RECORD_STACK_MISMATCH;
    if(get64(p+40)!=A840_RECORD_CHIP_ID)return A840_RECORD_CHIP_MISMATCH;
    if(get32(p+48)!=A840_RECORD_HBB)return A840_RECORD_HBB_MISMATCH;
    record_digest(p,bytes,digest);if(memcmp(digest,p+96,32))return A840_RECORD_HASH_MISMATCH;
    if(key && memcmp(key->sha256,p+64,32))return A840_RECORD_HASH_MISMATCH;
    st=parse_record(p,bytes,&record,NULL);if(st!=A840_RECORD_OK)return st;
    if(out->owned_storage)return A840_RECORD_INVALID_ARG;
    for(i=0;i<record.stage_count;++i)words+=(size_t)record.stages[i].isa_dwords+
        record.stages[i].const_data_dwords+record.stages[i].sds_state_dwords;
    record.owned_storage=malloc(words*4);if(!record.owned_storage)return A840_RECORD_OOM;
    record.owned_bytes=words*4;
    st=parse_record(p,bytes,&record,record.owned_storage);
    if(st!=A840_RECORD_OK){free(record.owned_storage);return st;}
    *out=record;return A840_RECORD_OK;
}

void a840_record_free(A840PipelineRecord *r)
{if(r){free(r->owned_storage);memset(r,0,sizeof(*r));}}

static A840RecordStatus open_directory(const char *dir,int *fd)
{
    if(!dir || !*dir)return A840_RECORD_INVALID_ARG;
    *fd=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    return *fd<0 ? A840_RECORD_IO_ERROR : A840_RECORD_OK;
}
static void key_filename(const A840PipelineKey *key,char filename[72])
{
    static const char hex[]="0123456789abcdef";size_t i;
    for(i=0;i<32;++i){filename[2*i]=hex[key->sha256[i]>>4];filename[2*i+1]=hex[key->sha256[i]&15];}
    memcpy(filename+64,".a840pr",8);
}
static int sync_fd(int fd) {int rc;do{rc=fsync(fd);}while(rc<0 && errno==EINTR);return rc;}

A840RecordStatus a840_record_save_file(const char *dir,const A840PipelineRecord *r)
{
    static _Atomic uint64_t sequence=0;
    uint8_t *data=NULL;size_t bytes=0,at=0;char name[72],temporary[128];
    int dirfd=-1,fd=-1;unsigned attempt;A840RecordStatus st;
    st=a840_record_serialize(r,&data,&bytes);if(st!=A840_RECORD_OK)return st;
    st=open_directory(dir,&dirfd);if(st!=A840_RECORD_OK){free(data);return st;}
    key_filename(&r->pipeline_key,name);
    for(attempt=0;attempt<64;++attempt) {
        uint64_t seq=atomic_fetch_add_explicit(&sequence,1,memory_order_relaxed);
        int n=snprintf(temporary,sizeof(temporary),".%s.tmp.%ld.%llu",name,(long)getpid(),(unsigned long long)seq);
        if(n<0 || (size_t)n>=sizeof(temporary)){st=A840_RECORD_RANGE_ERROR;break;}
        fd=openat(dirfd,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0)break;
        if(errno!=EEXIST){st=A840_RECORD_IO_ERROR;break;}
    }
    if(fd<0){free(data);close(dirfd);return st==A840_RECORD_OK ? A840_RECORD_IO_ERROR : st;}
    while(at<bytes) {
        ssize_t wrote=write(fd,data+at,bytes-at);
        if(wrote<0 && errno==EINTR)continue;
        if(wrote<=0){st=A840_RECORD_IO_ERROR;break;}
        at+=(size_t)wrote;
    }
    if(st==A840_RECORD_OK && sync_fd(fd)<0)st=A840_RECORD_IO_ERROR;
    if(close(fd)<0)st=A840_RECORD_IO_ERROR;
    if(st==A840_RECORD_OK && renameat(dirfd,temporary,dirfd,name)<0)st=A840_RECORD_IO_ERROR;
    if(st!=A840_RECORD_OK)(void)unlinkat(dirfd,temporary,0);
    else if(sync_fd(dirfd)<0)st=A840_RECORD_IO_ERROR;
    free(data);close(dirfd);return st;
}

A840RecordStatus a840_record_load_file(const char *dir,const A840PipelineKey *key,
    uint64_t stack,uint64_t abi,A840PipelineRecord *out)
{
    char name[72];int dirfd,fd;struct stat info;size_t at=0,bytes;uint8_t *data;
    A840RecordStatus st;uint8_t extra;
    if(!region_valid(key,sizeof(*key),1) || !nonzero_key(key) ||
        !region_valid(out,sizeof(*out),_Alignof(A840PipelineRecord)) || !stack || !abi)
        return A840_RECORD_INVALID_ARG;
    st=open_directory(dir,&dirfd);if(st!=A840_RECORD_OK)return st;
    key_filename(key,name);fd=openat(dirfd,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){st=errno==ENOENT ? A840_RECORD_NOT_FOUND : A840_RECORD_IO_ERROR;close(dirfd);return st;}
    close(dirfd);
    if(fstat(fd,&info)<0 || !S_ISREG(info.st_mode)){close(fd);return A840_RECORD_IO_ERROR;}
    if(info.st_size<A840_RECORD_PREFIX_BYTES || info.st_size>A840_RECORD_MAX_BYTES){close(fd);return A840_RECORD_LENGTH_ERROR;}
    bytes=(size_t)info.st_size;data=malloc(bytes);if(!data){close(fd);return A840_RECORD_OOM;}
    while(at<bytes) {
        ssize_t got=read(fd,data+at,bytes-at);
        if(got<0 && errno==EINTR)continue;
        if(got<0){st=A840_RECORD_IO_ERROR;break;}
        if(!got){st=A840_RECORD_LENGTH_ERROR;break;}
        at+=(size_t)got;
    }
    if(st==A840_RECORD_OK) {
        ssize_t got;do{got=read(fd,&extra,1);}while(got<0 && errno==EINTR);
        if(got<0)st=A840_RECORD_IO_ERROR;else if(got)st=A840_RECORD_LENGTH_ERROR;
    }
    if(close(fd)<0)st=A840_RECORD_IO_ERROR;
    if(st==A840_RECORD_OK)st=a840_record_deserialize(data,bytes,stack,abi,key,out);
    free(data);return st;
}

typedef struct CacheSlot {
    A840PipelineRecord record;
    size_t wire_bytes,hash_next,hash_prev,prev,next,free_next;
} CacheSlot;
struct A840PipelineMemCache {
    CacheSlot *slots;size_t *buckets,bucket_count,head,tail,free_head;
    uint64_t stack_key,abi_fingerprint;
    A840PipelineCacheStats stats;
};
static size_t bucket_for(const A840PipelineMemCache *c,const A840PipelineKey *key)
{
    uint64_t hash=UINT64_C(14695981039346656037);size_t i;
    for(i=0;i<32;++i){hash^=key->sha256[i];hash*=UINT64_C(1099511628211);}
    return (size_t)hash&(c->bucket_count-1);
}
static size_t cache_find(const A840PipelineMemCache *c,const A840PipelineKey *key)
{
    size_t at=c->buckets[bucket_for(c,key)];
    while(at!=NO_SLOT){if(!memcmp(c->slots[at].record.pipeline_key.sha256,key->sha256,32))return at;at=c->slots[at].hash_next;}
    return NO_SLOT;
}
static void lru_unlink(A840PipelineMemCache *c,size_t at)
{
    CacheSlot *s=&c->slots[at];
    if(s->prev!=NO_SLOT)c->slots[s->prev].next=s->next;else c->head=s->next;
    if(s->next!=NO_SLOT)c->slots[s->next].prev=s->prev;else c->tail=s->prev;
}
static void lru_front(A840PipelineMemCache *c,size_t at)
{
    CacheSlot *s=&c->slots[at];s->prev=NO_SLOT;s->next=c->head;
    if(c->head!=NO_SLOT)c->slots[c->head].prev=at;else c->tail=at;
    c->head=at;
}
static void cache_remove(A840PipelineMemCache *c,size_t at,int eviction)
{
    CacheSlot *s=&c->slots[at];size_t bucket=bucket_for(c,&s->record.pipeline_key);
    if(s->hash_prev!=NO_SLOT)c->slots[s->hash_prev].hash_next=s->hash_next;else c->buckets[bucket]=s->hash_next;
    if(s->hash_next!=NO_SLOT)c->slots[s->hash_next].hash_prev=s->hash_prev;
    lru_unlink(c,at);c->stats.bytes-=s->wire_bytes;--c->stats.entries;
    a840_record_free(&s->record);s->free_next=c->free_head;c->free_head=at;
    if(eviction)++c->stats.evictions;
}

A840RecordStatus a840_pipeline_mem_cache_create(size_t capacity,size_t budget,
    uint64_t stack,uint64_t abi,A840PipelineMemCache **out)
{
    A840PipelineMemCache *c;size_t buckets=1,i;
    if(!region_valid(out,sizeof(*out),_Alignof(A840PipelineMemCache *)) || !capacity || !budget || !stack || !abi)
        return A840_RECORD_INVALID_ARG;
    if(capacity>65536)return A840_RECORD_RANGE_ERROR;
    if(abi!=a840_compile_abi_fingerprint())return A840_RECORD_ABI_MISMATCH;
    while(buckets<capacity*2)buckets*=2;
    c=calloc(1,sizeof(*c));if(!c)return A840_RECORD_OOM;
    c->slots=calloc(capacity,sizeof(*c->slots));c->buckets=malloc(buckets*sizeof(*c->buckets));
    if(!c->slots || !c->buckets){free(c->slots);free(c->buckets);free(c);return A840_RECORD_OOM;}
    c->bucket_count=buckets;c->head=c->tail=NO_SLOT;c->free_head=0;c->stack_key=stack;c->abi_fingerprint=abi;
    c->stats.capacity=capacity;c->stats.byte_capacity=budget;
    for(i=0;i<buckets;++i)c->buckets[i]=NO_SLOT;
    for(i=0;i<capacity;++i)c->slots[i].free_next=i+1<capacity ? i+1 : NO_SLOT;
    *out=c;return A840_RECORD_OK;
}
void a840_pipeline_mem_cache_destroy(A840PipelineMemCache *c)
{
    if(c){size_t i;for(i=0;i<c->stats.capacity;++i)a840_record_free(&c->slots[i].record);
        free(c->slots);free(c->buckets);free(c);}
}
const A840PipelineRecord *a840_pipeline_mem_cache_get(A840PipelineMemCache *c,const A840PipelineKey *key)
{
    size_t at;if(!c || !region_valid(key,sizeof(*key),1))return NULL;
    at=cache_find(c,key);if(at==NO_SLOT){++c->stats.misses;return NULL;}
    ++c->stats.hits;lru_unlink(c,at);lru_front(c,at);return &c->slots[at].record;
}
A840RecordStatus a840_pipeline_mem_cache_put(A840PipelineMemCache *c,const A840PipelineRecord *r)
{
    uint8_t *data=NULL;size_t bytes=0,existing,at,bucket;A840PipelineRecord clone={0};A840RecordStatus st;
    if(!c || !region_valid(r,sizeof(*r),_Alignof(A840PipelineRecord)))return A840_RECORD_INVALID_ARG;
    if(r->stack_key!=c->stack_key)return A840_RECORD_STACK_MISMATCH;
    if(r->abi_fingerprint!=c->abi_fingerprint)return A840_RECORD_ABI_MISMATCH;
    st=record_size(r,&bytes);if(st!=A840_RECORD_OK)return st;
    if(bytes>c->stats.byte_capacity)return A840_RECORD_CAPACITY;
    st=a840_record_serialize(r,&data,&bytes);if(st!=A840_RECORD_OK)return st;
    st=a840_record_deserialize(data,bytes,c->stack_key,c->abi_fingerprint,&r->pipeline_key,&clone);
    free(data);if(st!=A840_RECORD_OK)return st;
    existing=cache_find(c,&clone.pipeline_key);
    if(existing!=NO_SLOT){cache_remove(c,existing,0);++c->stats.replacements;}
    while(c->stats.entries==c->stats.capacity || c->stats.bytes>c->stats.byte_capacity-bytes)
        cache_remove(c,c->tail,1);
    at=c->free_head;c->free_head=c->slots[at].free_next;
    c->slots[at].record=clone;c->slots[at].wire_bytes=bytes;
    bucket=bucket_for(c,&clone.pipeline_key);c->slots[at].hash_prev=NO_SLOT;c->slots[at].hash_next=c->buckets[bucket];
    if(c->buckets[bucket]!=NO_SLOT)c->slots[c->buckets[bucket]].hash_prev=at;
    c->buckets[bucket]=at;lru_front(c,at);++c->stats.entries;c->stats.bytes+=bytes;++c->stats.puts;
    return A840_RECORD_OK;
}
A840RecordStatus a840_pipeline_mem_cache_stats(const A840PipelineMemCache *c,A840PipelineCacheStats *out)
{
    if(!c || !region_valid(out,sizeof(*out),_Alignof(A840PipelineCacheStats)))return A840_RECORD_INVALID_ARG;
    if(overlap(c,sizeof(*c),out,sizeof(*out)))return A840_RECORD_ALIAS;
    *out=c->stats;return A840_RECORD_OK;
}

const char *a840_record_status_string(A840RecordStatus status)
{
    static const char *const names[]={"OK","INVALID_ARG","RANGE_ERROR","CAPACITY","MAGIC_ERROR","VERSION_ERROR",
        "LENGTH_ERROR","ABI_MISMATCH","STACK_MISMATCH","CHIP_MISMATCH","HBB_MISMATCH","HASH_MISMATCH",
        "FORMAT_ERROR","OOM","IO_ERROR","NOT_FOUND","ALIAS"};
    return (unsigned)status<sizeof(names)/sizeof(names[0]) ? names[status] : "UNKNOWN";
}
