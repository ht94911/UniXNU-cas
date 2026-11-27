/* SPDX-License-Identifier: MIT */
#ifdef A840_BUILD_PACK_DRIVER
#define _POSIX_C_SOURCE 200809L
#include "a840_pack_v1.h"
#include "NativeMetalTranslate.h"
#include "a840_binding_plan.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void json_string(const char *s)
{
    const unsigned char *p=(const unsigned char *)(s ? s : "");
    putchar('"');
    for(;*p;++p) {
        if(*p=='"' || *p=='\\'){putchar('\\');putchar(*p);}
        else if(*p<32)printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
static int error(const char *where,const char *why)
{
    printf("{\"status\":\"FAIL\",\"where\":");json_string(where);
    printf(",\"error\":");json_string(why);puts("}");return 1;
}
static NmbLibrary *library(NmbDevice *dev,const char *path)
{
    FILE *f=fopen(path,"rb");long size;uint8_t *bytes;NmbLibrary *result=NULL;
    if(!f)return NULL;
    if(fseek(f,0,SEEK_END) || (size=ftell(f))<88 || size>64*1024*1024 || fseek(f,0,SEEK_SET)) {
        fclose(f);return NULL;
    }
    bytes=malloc((size_t)size);
    if(bytes && fread(bytes,1,(size_t)size,f)==(size_t)size)
        result=nmb_library_parse(dev,bytes,(size_t)size);
    free(bytes);fclose(f);return result;
}
static int spirv_digest(const uint32_t *words,size_t bytes,uint8_t hash[32])
{
    uint8_t *canonical=malloc(bytes);size_t i;A840RecordStatus status;
    if(!canonical)return 0;
    for(i=0;i<bytes;++i)canonical[i]=(uint8_t)(words[i/4]>>((i%4)*8));
    status=a840_record_sha256(canonical,bytes,hash);free(canonical);
    return status==A840_RECORD_OK;
}
static int compile_one(const char *path,const char *mode,const char *entry,
    const char *other,const char *output,uint64_t stack)
{
    const int compute=!strcmp(mode,"compute");
    const A840CompileTarget target={.abi_version=1,.chip_id=A840_PACK_V1_CHIP_ID,
        .num_slices=3,.num_ccu=6,.highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1};
    NmbDevice *dev=NULL;NmbLibrary *lib=NULL;NmbSpirvResult *spv=NULL;
    A840PipelineRecord record={0};A840BindingPlan plan;
    A840RecordKeyStage keys[2]={{0}};NmbSpirvBinding bindings[128];
    NmbSpirvPushConstantRange push[2];NmbComputePipelineDesc compute_desc={{8,8,1},1};
    NmbRenderPipelineDesc render_desc={0};uint32_t color=0,half=0,compiled=0,i;
    A840PackV1Builder builder;A840PackV1BuilderItem items[1];uint32_t buckets[2];
    size_t bytes=0;void *memory=NULL;A840PackV1Status status;char message[1024]={0};
    int result=1;
    dev=nmb_device_create_a840(0);
    if(!dev){error("device",nmb_last_error());goto cleanup;}
    lib=library(dev,path);
    if(!lib){error("library",nmb_last_error());goto cleanup;}
    if(compute)spv=nmb_compile_compute_spirv_ex(dev,lib,entry,&compute_desc);
    else {
        if(!nmb_library_default_render_desc(dev,lib,entry,other,&render_desc)) {
            error("render_desc",nmb_last_error());goto cleanup;
        }
        for(i=0;i<render_desc.color_attachment_count;++i) {
            uint32_t format=render_desc.color_mtl_formats[i];
            if(format)color|=1u<<i;
            if(format==25 || format==65 || format==115)half|=1u<<i;
        }
        spv=nmb_compile_render_spirv_ex(dev,lib,entry,other,&render_desc);
    }
    if(!spv){error("translate",nmb_last_error());goto cleanup;}
    record.stage_count=nmb_spirv_stage_count(spv);
    if(record.stage_count!=(compute ? 1u : 2u)){error("stage_count","translator count mismatch");goto cleanup;}
    record.abi_fingerprint=a840_compile_abi_fingerprint();record.stack_key=stack;
    record.chip_id=A840_PACK_V1_CHIP_ID;record.highest_bank_bit=16;
    for(i=0;i<record.stage_count;++i) {
        A840StageInput input={0};size_t spirv_bytes=0;A840CompileStatus compile_status;
        input.abi_version=1;input.stage=compute ? A840_STAGE_COMPUTE : i;
        input.spirv_words=nmb_spirv_words(spv,i,&spirv_bytes);input.spirv_bytes=spirv_bytes;
        input.entry_point=nmb_spirv_entry_point(spv,i);input.sample_count=compute ? 1 : render_desc.sample_count;
        input.flags=A840_COMPILE_FLAG_EARLY_PREAMBLE;
        if(compute)for(uint32_t j=0;j<3;++j)input.local_size[j]=compute_desc.local_size[j];
        if(input.stage==A840_STAGE_FRAGMENT){input.color_rt_mask=color;input.half_rt_mask=half;}
        if(!input.spirv_words || !input.entry_point || !nmb_spirv_push_constant_range(spv,i,&push[i])) {
            error("stage","missing words/reflection");goto cleanup;
        }
        keys[i].entry_point=input.entry_point;keys[i].stage=input.stage;
        keys[i].sample_count=input.sample_count;keys[i].color_rt_mask=input.color_rt_mask;
        keys[i].half_rt_mask=input.half_rt_mask;keys[i].flags=input.flags;
        for(uint32_t j=0;j<3;++j)keys[i].local_size[j]=input.local_size[j];
        if(!spirv_digest(input.spirv_words,spirv_bytes,keys[i].spirv_sha256)) {
            error("key","SPIR-V digest failed");goto cleanup;
        }
        compile_status=a840_compile_stage(&target,&input,&record.stages[i],message,sizeof(message));
        if(compile_status!=A840_COMPILE_OK){error("ir3",message);goto cleanup;}
        ++compiled;
    }
    record.binding_count=(uint32_t)nmb_spirv_binding_count(spv);
    if(record.binding_count>128){error("reflection","binding count");goto cleanup;}
    for(i=0;i<record.binding_count;++i) {
        const NmbSpirvBinding *b=&bindings[i];
        if(!nmb_spirv_binding(spv,i,&bindings[i])){error("reflection",nmb_last_error());goto cleanup;}
        record.bindings[i]=(A840RecordBinding){b->descriptor_set,b->binding,b->descriptor_type,b->descriptor_count,
            b->stage_mask,b->buffer_index,b->texture_index,b->sampler_index};
    }
    if(a840_record_compute_key(keys,record.stage_count,&record.pipeline_key)!=A840_RECORD_OK) {
        error("key","pipeline key failed");goto cleanup;
    }
    {
        A840BindingPlanStatus s=a840_binding_plan_build(bindings,record.binding_count,record.stages,
            record.stage_count,push,&plan);
        if(s!=A840_BINDING_PLAN_OK){error("binding_plan",a840_binding_plan_status_string(s));goto cleanup;}
    }
    status=a840_pack_v1_builder_init(&builder,0,stack,record.abi_fingerprint,items,1,buckets,2);
    if(status==A840_PACK_OK)status=a840_pack_v1_builder_add(&builder,&record,&plan);
    if(status==A840_PACK_OK)status=a840_pack_v1_builder_measure(&builder,&bytes);
    if(status!=A840_PACK_OK){error("builder",a840_pack_v1_status_string(status));goto cleanup;}
    if(posix_memalign(&memory,128,bytes)){error("memory","aligned allocation failed");goto cleanup;}
    status=a840_pack_v1_builder_build(&builder,memory,bytes,&bytes);
    if(status==A840_PACK_OK)status=a840_pack_v1_save_file(output,memory,bytes,stack,record.abi_fingerprint);
    if(status!=A840_PACK_OK){error("part_pack",a840_pack_v1_status_string(status));goto cleanup;}
    printf("{\"status\":\"PASS\",\"compiled_ok\":true,\"binding_plan_ok\":true,\"stages\":%u,"
           "\"bindings\":%u,\"pack_bytes\":%zu,\"pipeline_key_sha256\":\"",record.stage_count,record.binding_count,bytes);
    for(i=0;i<32;++i)printf("%02x",record.pipeline_key.sha256[i]);
    printf("\",\"abi_fingerprint\":\"0x%016" PRIx64 "\",\"stack_key\":\"0x%016" PRIx64 "\"}\n",
        record.abi_fingerprint,stack);
    result=0;
cleanup:
    free(memory);
    for(i=0;i<compiled;++i)a840_compile_stage_free(&record.stages[i]);
    if(spv)nmb_spirv_result_free(spv);
    if(lib)nmb_library_destroy(lib);
    if(dev)nmb_device_destroy(dev);
    return result;
}
typedef struct Mapping {
    int fd;
    void *base;
    size_t bytes;
    A840PackV1View view;
    A840PackV1Entry entry;
    A840PipelineRecord record;
} Mapping;
static int map_file(const char *path,uint64_t stack,Mapping *m)
{
    struct stat st;A840PackV1Status status;
    m->fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(m->fd<0 || fstat(m->fd,&st)<0 || !S_ISREG(st.st_mode) || st.st_size<128 ||
       (uint64_t)st.st_size>UINT32_MAX)return error("mmap","input regular-file size");
    m->bytes=(size_t)st.st_size;
    m->base=mmap(NULL,m->bytes,PROT_READ,MAP_PRIVATE,m->fd,0);
    if(m->base==MAP_FAILED){m->base=NULL;return error("mmap","mapping failed");}
    status=a840_pack_v1_open_memory(m->base,m->bytes,stack,a840_compile_abi_fingerprint(),&m->view);
    if(status!=A840_PACK_OK)return error("open_memory",a840_pack_v1_status_string(status));
    return 0;
}
static void unmap(Mapping *m)
{
    if(m->base)munmap(m->base,m->bytes);
    if(m->fd>=0)close(m->fd);
}
static int merge(const char *output,uint64_t stack,int count,char **paths)
{
    Mapping *maps=NULL,final={.fd=-1};A840PackV1BuilderItem *items=NULL;
    uint32_t *buckets=NULL,bucket_capacity=2;A840PackV1Builder b;
    void *memory=NULL;size_t bytes=0;int result=1,i;uint32_t s;
    A840PackV1Status status;struct timespec start,end;uint64_t iterations=0;
    if(count<1 || count>32768)return error("merge","part count");
    maps=calloc((size_t)count,sizeof(*maps));items=calloc((size_t)count,sizeof(*items));
    while(bucket_capacity<(uint32_t)count*4 && bucket_capacity<65536)bucket_capacity*=2;
    buckets=malloc((size_t)bucket_capacity*4);
    if(maps)for(i=0;i<count;++i)maps[i].fd=-1;
    if(!maps || !items || !buckets){error("merge","workspace allocation");goto cleanup;}
    status=a840_pack_v1_builder_init(&b,0,stack,a840_compile_abi_fingerprint(),items,(size_t)count,buckets,bucket_capacity);
    if(status!=A840_PACK_OK){error("merge_init",a840_pack_v1_status_string(status));goto cleanup;}
    for(i=0;i<count;++i) {
        if(map_file(paths[i],stack,&maps[i]))goto cleanup;
        if(maps[i].view.entry_count!=1){error("merge","part must contain one pipeline");goto cleanup;}
        status=a840_pack_v1_entry_at(&maps[i].view,0,&maps[i].entry);
        if(status==A840_PACK_OK)status=a840_pack_v1_entry_record(&maps[i].view,&maps[i].entry,&maps[i].record);
        if(status==A840_PACK_OK)status=a840_pack_v1_builder_add(&b,&maps[i].record,maps[i].entry.binding_plan);
        if(status!=A840_PACK_OK){error("merge_add",a840_pack_v1_status_string(status));goto cleanup;}
    }
    status=a840_pack_v1_builder_measure(&b,&bytes);
    if(status!=A840_PACK_OK || posix_memalign(&memory,128,bytes)){error("merge_measure","size/allocation");goto cleanup;}
    status=a840_pack_v1_builder_build(&b,memory,bytes,&bytes);
    if(status==A840_PACK_OK)status=a840_pack_v1_save_file(output,memory,bytes,stack,a840_compile_abi_fingerprint());
    if(status!=A840_PACK_OK){error("merge_save",a840_pack_v1_status_string(status));goto cleanup;}
    if(map_file(output,stack,&final))goto cleanup;
    for(i=0;i<count;++i) {
        A840PackV1Entry e;
        status=a840_pack_v1_lookup(&final.view,&maps[i].record.pipeline_key,&e);
        if(status!=A840_PACK_OK || e.stage_count!=maps[i].entry.stage_count ||
           memcmp(e.binding_plan,maps[i].entry.binding_plan,sizeof(A840BindingPlan))) {
            error("verify_lookup","plan/key/count mismatch");goto cleanup;
        }
        for(s=0;s<e.stage_count;++s) {
            const A840PackV1StageView *a=&e.stages[s],*c=&maps[i].entry.stages[s];
            if((uintptr_t)a->isa_words%128 || a->isa_dwords!=c->isa_dwords ||
               a->const_data_dwords!=c->const_data_dwords || a->sds_state_dwords!=c->sds_state_dwords ||
               memcmp(a->isa_words,c->isa_words,(size_t)a->isa_dwords*4) ||
               (a->const_data_dwords && memcmp(a->const_data_words,c->const_data_words,(size_t)a->const_data_dwords*4)) ||
               (a->sds_state_dwords && memcmp(a->sds_state_words,c->sds_state_words,(size_t)a->sds_state_dwords*4))) {
                error("verify_lookup","zero-copy stage payload mismatch");goto cleanup;
            }
        }
    }
    if(clock_gettime(CLOCK_MONOTONIC,&start)){error("clock","start");goto cleanup;}
    for(unsigned repeat=0;repeat<10000;++repeat)
        for(i=0;i<count;++i) {
            A840PackV1Entry e;
            if(a840_pack_v1_lookup(&final.view,&maps[i].record.pipeline_key,&e)!=A840_PACK_OK) {
                error("lookup_timing","miss");goto cleanup;
            }
            ++iterations;
        }
    if(clock_gettime(CLOCK_MONOTONIC,&end)){error("clock","end");goto cleanup;}
    {
        uint64_t ns=(uint64_t)(end.tv_sec-start.tv_sec)*UINT64_C(1000000000)+(uint64_t)end.tv_nsec-(uint64_t)start.tv_nsec;
        printf("{\"status\":\"PASS\",\"attempted_pipelines\":%d,\"lookup_verified\":%d,\"unique_entries\":%u,"
               "\"pack_bytes\":%zu,\"mmap_readonly\":true,\"all_isa_128b_aligned\":true,\"sha256_verified\":true,"
               "\"lookup_iterations\":%" PRIu64 ",\"lookup_mean_ns\":%.3f,\"tier\":%u,\"stack_key\":\"0x%016" PRIx64 "\"}\n",
            count,count,final.view.entry_count,bytes,iterations,(double)ns/(double)iterations,final.view.tier,stack);
    }
    result=0;
cleanup:
    unmap(&final);
    if(maps){for(i=0;i<count;++i)unmap(&maps[i]);}
    free(memory);free(maps);free(items);free(buckets);return result;
}
int main(int argc,char **argv)
{
    char *end=NULL;uint64_t stack;
    if(argc<5){fprintf(stderr,"usage: pack_builder_driver compile <lib> <compute|render> <entry> <other> <out> <stack> | merge <out> <stack> <parts...>\n");return 2;}
    if(!strcmp(argv[1],"compile") && argc==8) {
        errno=0;stack=strtoull(argv[7],&end,0);
        if(errno || !end || *end || !stack || (strcmp(argv[3],"compute") && strcmp(argv[3],"render")))return 2;
        return compile_one(argv[2],argv[3],argv[4],argv[5],argv[6],stack);
    }
    if(!strcmp(argv[1],"merge")) {
        errno=0;stack=strtoull(argv[3],&end,0);
        if(errno || !end || *end || !stack)return 2;
        return merge(argv[2],stack,argc-4,argv+4);
    }
    return 2;
}
#else
typedef int a840_pack_builder_driver_disabled_t;
#endif /* A840_BUILD_PACK_DRIVER */
