/* SPDX-License-Identifier: MIT */
#ifdef A840_BUILD_CORPUS_DRIVER
#define _POSIX_C_SOURCE 200809L
#include "a840_record_io.h"
#include "NativeMetalTranslate.h"
#include "a840_binding_plan.h"
#include "common/disasm.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_string(const char *text)
{
    const unsigned char *p=(const unsigned char *)(text?text:"");
    putchar('"');
    for(;*p;++p) {
        if(*p=='"' || *p=='\\'){putchar('\\');putchar(*p);}
        else if(*p<32)printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}
static void hex_digest(const uint8_t digest[32],char text[65])
{
    static const char hex[]="0123456789abcdef";size_t i;
    for(i=0;i<32;++i){text[2*i]=hex[digest[i]>>4];text[2*i+1]=hex[digest[i]&15];}text[64]=0;
}
static void failure(const char *category,const char *message)
{
    printf("{\"status\":");json_string(category);printf(",\"error\":");json_string(message);puts("}");
}
static int record_failure(const char *message)
{
    printf("{\"status\":\"RECORD_ERR\",\"compiled_ok\":true,\"error\":");
    json_string(message);
    puts("}");
    return 1;
}
static NmbLibrary *load_library(NmbDevice *dev,const char *path)
{
    FILE *f=fopen(path,"rb");long size;uint8_t *bytes;size_t got;NmbLibrary *library;
    if(!f)return NULL;
    if(fseek(f,0,SEEK_END) || (size=ftell(f))<=0 || size>64*1024*1024 || fseek(f,0,SEEK_SET)) {
        fclose(f);return NULL;
    }
    bytes=malloc((size_t)size);if(!bytes){fclose(f);return NULL;}
    got=fread(bytes,1,(size_t)size,f);fclose(f);
    library=got==(size_t)size ? nmb_library_parse(dev,bytes,got) : NULL;
    free(bytes);return library;
}
static const char *translation_category(const char *error)
{
    return strstr(error,"Unsupported") || strstr(error,"unsupported") || strstr(error,"not supported") ? "UNSUPPORTED" : "SPIRV_ERR";
}
static int encode_spirv_digest(const uint32_t *words,size_t bytes,uint8_t digest[32])
{
    uint8_t *canonical=malloc(bytes);size_t i;
    A840RecordStatus status;
    if(!canonical)return 0;
    for(i=0;i<bytes;++i)canonical[i]=(uint8_t)(words[i/4]>>((i%4)*8));
    status=a840_record_sha256(canonical,bytes,digest);free(canonical);
    return status==A840_RECORD_OK;
}
static int stage_equal(const A840CompiledStage *a,const A840CompiledStage *b)
{
    if(a->abi_version!=b->abi_version || a->stage!=b->stage || a->isa_dwords!=b->isa_dwords ||
        a->instruction_groups!=b->instruction_groups || a->max_full_reg!=b->max_full_reg ||
        a->max_half_reg!=b->max_half_reg || a->branchstack!=b->branchstack || a->constlen_vec4!=b->constlen_vec4 ||
        a->const_data_dwords!=b->const_data_dwords || a->private_mem_bytes!=b->private_mem_bytes ||
        a->shared_mem_bytes!=b->shared_mem_bytes || a->flags_out!=b->flags_out ||
        a->ubo_push_count!=b->ubo_push_count || a->vpc_varying_count!=b->vpc_varying_count ||
        a->sds_state_dwords!=b->sds_state_dwords || a->reg_spills!=b->reg_spills ||
        a->reg_fills!=b->reg_fills || a->max_waves!=b->max_waves || b->owner)return 0;
    if(memcmp(a->local_size,b->local_size,sizeof(a->local_size)) ||
        memcmp(a->ubo_push,b->ubo_push,sizeof(a->ubo_push)) ||
        memcmp(a->vpc_varyings,b->vpc_varyings,sizeof(a->vpc_varyings)))return 0;
    if(memcmp(a->isa_words,b->isa_words,(size_t)a->isa_dwords*4))return 0;
    if(a->const_data_dwords && memcmp(a->const_data_words,b->const_data_words,(size_t)a->const_data_dwords*4))return 0;
    if(a->sds_state_dwords && memcmp(a->sds_state_words,b->sds_state_words,(size_t)a->sds_state_dwords*4))return 0;
    return 1;
}

static int compile_pipeline(NmbDevice *dev,NmbLibrary *lib,const char *mode,const char *entry,
    const char *other,const char *directory,uint64_t stack_key)
{
    const A840CompileTarget target={.abi_version=1,.chip_id=A840_RECORD_CHIP_ID,.num_slices=3,.num_ccu=6,
        .highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1};
    const bool compute = !strcmp(mode, "compute");
    const NmbComputePipelineDesc compute_desc = {{8, 8, 1}, 1};
    NmbRenderPipelineDesc render_desc = {0};
    NmbSpirvBinding bindings[A840_RECORD_MAX_BINDINGS];
    NmbSpirvPushConstantRange push_ranges[A840_RECORD_MAX_STAGES];
    A840BindingPlan plan;
    uint32_t color_mask = 0, half_mask = 0;
    NmbSpirvResult *spv=NULL;A840PipelineRecord record={0},loaded={0};
    A840RecordKeyStage keys[2]={{0}};struct shader_stats stats[2];uint32_t i,compiled=0;
    char error[1024]={0},digest_text[65];A840RecordStatus record_status;
    uint8_t *serialized=NULL,*serialized_loaded=NULL;size_t bytes=0,loaded_bytes=0;
    int result=0;
    if (compute) {
        spv = nmb_compile_compute_spirv_ex(dev, lib, entry, &compute_desc);
    } else {
        if (!nmb_library_default_render_desc(dev, lib, entry, other, &render_desc)) {
            failure(translation_category(nmb_last_error()), nmb_last_error());
            return 0;
        }
        for (uint32_t slot = 0; slot < render_desc.color_attachment_count; ++slot) {
            uint32_t format = render_desc.color_mtl_formats[slot];
            if (format) color_mask |= 1u << slot;
            if (format == 25 || format == 65 || format == 115) half_mask |= 1u << slot;
        }
        spv = nmb_compile_render_spirv_ex(dev, lib, entry, other, &render_desc);
    }
    if(!spv){failure(translation_category(nmb_last_error()),nmb_last_error());return 0;}
    record.stage_count=nmb_spirv_stage_count(spv);
    if(record.stage_count!=(compute?1u:2u)) {
        failure("SPIRV_ERR","unexpected translator stage count");goto cleanup;
    }
    record.abi_fingerprint=a840_compile_abi_fingerprint();record.stack_key=stack_key;
    record.chip_id=A840_RECORD_CHIP_ID;record.highest_bank_bit=16;
    memset(stats,0,sizeof(stats));
    for(i=0;i<record.stage_count;++i) {
        A840StageInput input={0};size_t spirv_bytes=0;FILE *disasm;
        char *disasm_text=NULL;size_t disasm_bytes=0;
        A840CompileStatus status;
        input.abi_version=1;input.stage=record.stage_count==1?A840_STAGE_COMPUTE:i;
        input.spirv_words=nmb_spirv_words(spv,i,&spirv_bytes);input.spirv_bytes=spirv_bytes;
        input.entry_point=nmb_spirv_entry_point(spv,i);
        input.sample_count = compute ? 1u : render_desc.sample_count;
        if (input.stage == A840_STAGE_FRAGMENT) {
            input.color_rt_mask = color_mask;
            input.half_rt_mask = half_mask;
        }
        input.flags=A840_COMPILE_FLAG_EARLY_PREAMBLE;
        if (compute) memcpy(input.local_size, compute_desc.local_size, sizeof(input.local_size));
        if(!input.spirv_words || spirv_bytes<20 || !input.entry_point ||
           !nmb_spirv_push_constant_range(spv, i, &push_ranges[i])) {
            failure("SPIRV_ERR","translator returned no stage words/entry");goto cleanup;
        }
        keys[i].entry_point=input.entry_point;keys[i].stage=input.stage;
        keys[i].sample_count = input.sample_count;
        keys[i].color_rt_mask = input.color_rt_mask;
        keys[i].half_rt_mask = input.half_rt_mask;
        keys[i].flags = input.flags;
        memcpy(keys[i].local_size,input.local_size,sizeof(input.local_size));
        if(!encode_spirv_digest(input.spirv_words,spirv_bytes,keys[i].spirv_sha256)) {
            failure("RECORD_ERR","SPIR-V digest allocation failed");result=1;goto cleanup;
        }
        status=a840_compile_stage(&target,&input,&record.stages[i],error,sizeof(error));
        if(status!=A840_COMPILE_OK) {
            const char *category=status==A840_COMPILE_UNSUPPORTED_FEATURE?"UNSUPPORTED":
                status==A840_COMPILE_SPIRV_ERROR?"SPIRV_ERR":"IR3_ERR";
            printf("{\"status\":");json_string(category);printf(",\"compile_status\":%u,\"failed_stage\":%u,\"error\":",status,input.stage);
            json_string(error);puts("}");goto cleanup;
        }
        ++compiled;
        disasm=open_memstream(&disasm_text,&disasm_bytes);
        if(!disasm || disasm_a3xx_stat(record.stages[i].isa_words,(int)record.stages[i].isa_dwords,
            0,disasm,800,&stats[i])) {
            if(disasm)fclose(disasm);
            free(disasm_text);
            failure("IR3_ERR","Gen8 ISA disassembly/statistics failed");goto cleanup;
        }
        fclose(disasm);free(disasm_text);
    }
    {
        size_t binding_count=nmb_spirv_binding_count(spv);
        if(binding_count>A840_RECORD_MAX_BINDINGS){result=record_failure("binding count exceeds record capacity");goto cleanup;}
        record.binding_count=(uint32_t)binding_count;
    }
    for(i=0;i<record.binding_count;++i) {
        if(!nmb_spirv_binding(spv,i,&bindings[i])){result=record_failure(nmb_last_error());goto cleanup;}
        const NmbSpirvBinding *b = &bindings[i];
        record.bindings[i]=(A840RecordBinding){b->descriptor_set,b->binding,b->descriptor_type,b->descriptor_count,
            b->stage_mask,b->buffer_index,b->texture_index,b->sampler_index};
    }
    record_status=a840_record_compute_key(keys,record.stage_count,&record.pipeline_key);
    if(record_status==A840_RECORD_OK)record_status=a840_record_save_file(directory,&record);
    if(record_status==A840_RECORD_OK)record_status=a840_record_load_file(directory,&record.pipeline_key,
        record.stack_key,record.abi_fingerprint,&loaded);
    if(record_status==A840_RECORD_OK)record_status=a840_record_serialize(&record,&serialized,&bytes);
    if(record_status==A840_RECORD_OK)record_status=a840_record_serialize(&loaded,&serialized_loaded,&loaded_bytes);
    if(record_status!=A840_RECORD_OK || bytes!=loaded_bytes || memcmp(serialized,serialized_loaded,bytes)) {
        result=record_failure(record_status==A840_RECORD_OK?"wire roundtrip bytes differ":a840_record_status_string(record_status));goto cleanup;
    }
    for(i=0;i<compiled;++i)if(!stage_equal(&record.stages[i],&loaded.stages[i])) {
        result=record_failure("compiled stage metadata/payload mismatch");goto cleanup;
    }
    if(memcmp(record.bindings,loaded.bindings,(size_t)record.binding_count*sizeof(A840RecordBinding))) {
        result=record_failure("reflection binding mismatch");goto cleanup;
    }
    A840BindingPlanStatus plan_status = a840_binding_plan_build(bindings, record.binding_count,
        record.stages, record.stage_count, push_ranges, &plan);
    const char *plan_error = plan_status == A840_BINDING_PLAN_OK ? NULL :
        a840_binding_plan_status_string(plan_status);
    if (!plan_error) {
        // Frozen compiler metadata does not tag bindful ranges. Check their
        // source namespace against the pinned Mesa ISA decoder as well.
        for (i = 0; i < compiled; ++i) {
            const A840StageBindingPlan *stage_plan = &plan.stages[i];
            for (uint32_t ubo = 0; ubo < stage_plan->inline_ubo_count; ++ubo) {
                const A840InlineUbo *copy = &stage_plan->inline_ubos[ubo];
                if (copy->source == A840_INLINE_UBO_IR3_BINDFUL &&
                    !(stats[i].desc[0].ubo & (UINT64_C(1) << copy->binding)))
                    plan_error = "IR3 bindful UBO slot absent from decoded ISA";
            }
        }
    }
    hex_digest(record.pipeline_key.sha256,digest_text);
    printf("{\"status\":\"%s\",\"compiled_ok\":true,\"record_roundtrip_ok\":true,"
           "\"stage_count\":%u,\"binding_count\":%u,\"record_bytes\":%zu,\"pipeline_key_sha256\":",
        plan_error ? "BINDING_ERR" : "PASS",
        record.stage_count,record.binding_count,bytes);json_string(digest_text);
    printf(",\"binding_plan_ok\":%s", plan_error ? "false" : "true");
    if (!plan_error) {
        printf(",\"binding_table_bytes\":%u,\"metal_lookup_count\":%u,\"set_table_bytes\":[",
            plan.total_table_bytes, plan.metal_lookup_count);
        for (i = 0; i < ADRENO_COMPILE_MAX_BIND_SETS; ++i)
            printf("%s%u", i ? "," : "", plan.sets[i].table_bytes);
        putchar(']');
    } else {
        printf(",\"error\":");
        json_string(plan_error);
    }
    if (compute) {
        printf(",\"compute_desc\":{\"local_size\":[%u,%u,%u],\"zeroed_threadgroup_memory\":true}",
            compute_desc.local_size[0], compute_desc.local_size[1], compute_desc.local_size[2]);
    } else {
        printf(",\"render_desc\":{\"sample_count\":%u,\"color_mtl_formats\":[", render_desc.sample_count);
        for (i = 0; i < render_desc.color_attachment_count; ++i)
            printf("%s%u", i ? "," : "", render_desc.color_mtl_formats[i]);
        printf("],\"depth_mtl_format\":%u,\"stencil_mtl_format\":%u,"
               "\"vertex_attribute_count\":%u,\"vertex_layout_count\":%u}",
            render_desc.depth_mtl_format, render_desc.stencil_mtl_format,
            render_desc.vertex_attribute_count, render_desc.vertex_layout_count);
    }
    printf(",\"abi_fingerprint\":\"0x%016" PRIx64 "\",\"stack_key\":\"0x%016" PRIx64 "\",\"stages\":[",
        record.abi_fingerprint,record.stack_key);
    for(i=0;i<compiled;++i) {
        const A840CompiledStage *s=&record.stages[i];unsigned n,c;
        n=0;for(c=0;c<8;++c)n+=stats[i].instrs_per_cat[c];
        if(i)putchar(',');
        printf("{\"stage\":%u,\"isa_dwords\":%u,\"instruction_groups\":%u,\"instructions\":%u,\"max_full_reg\":%d,\"max_half_reg\":%d,"
            "\"spills\":%u,\"fills\":%u,\"constlen_vec4\":%u,\"const_data_dwords\":%u,\"shared_mem_bytes\":%u,\"private_mem_bytes\":%u,"
            "\"branchstack\":%u,\"max_waves\":%u,\"flags_out\":%u,\"ubo_push_count\":%u,\"vpc_varying_count\":%u,\"sds_state_dwords\":%u,"
            "\"local_size\":[%u,%u,%u],\"reflected_push_range\":[%u,%u],\"isa_bindful_ubo_mask\":\"0x%" PRIx64 "\"",
            s->stage,s->isa_dwords,s->instruction_groups,n,s->max_full_reg,s->max_half_reg,s->reg_spills,s->reg_fills,
            s->constlen_vec4,s->const_data_dwords,s->shared_mem_bytes,s->private_mem_bytes,
            s->branchstack,s->max_waves,s->flags_out,s->ubo_push_count,s->vpc_varying_count,s->sds_state_dwords,
            s->local_size[0],s->local_size[1],s->local_size[2],push_ranges[i].offset_bytes,
            push_ranges[i].size_bytes,stats[i].desc[0].ubo);
        if (!plan_error) {
            const A840StageBindingPlan *stage_plan = &plan.stages[i];
            uint32_t bindful_ubos = 0;
            for (uint32_t ubo = 0; ubo < stage_plan->inline_ubo_count; ++ubo)
                bindful_ubos += stage_plan->inline_ubos[ubo].source == A840_INLINE_UBO_IR3_BINDFUL;
            printf(",\"push_upload\":[%u,%u,%u],\"inactive_push_constants\":%s,"
                   "\"inline_ubo_count\":%u,\"ir3_bindful_ubo_count\":%u,\"inactive_ubo_count\":%u",
                stage_plan->push_constants.src_offset_bytes, stage_plan->push_constants.dst_offset_bytes,
                stage_plan->push_constants.size_bytes, stage_plan->inactive_push_constants ? "true" : "false",
                stage_plan->inline_ubo_count, bindful_ubos, stage_plan->inactive_ubo_count);
        }
        putchar('}');
    }
    puts("]}");
cleanup:
    free(serialized);free(serialized_loaded);a840_record_free(&loaded);
    for(i=0;i<compiled;++i)a840_compile_stage_free(&record.stages[i]);
    nmb_spirv_result_free(spv);return result;
}

int main(int argc,char **argv)
{
    NmbDevice *device;NmbLibrary *library;int result=0;
    if(argc<3){fprintf(stderr,"usage: corpus_driver list <metallib> | compile <metallib> <compute|render> <entry> <other|-> <dir> <stack>\n");return 2;}
    device=nmb_device_create_a840(0);
    if(!device){failure("SPIRV_ERR",nmb_last_error());return 0;}
    library=load_library(device,argv[2]);
    if(!library){failure("SPIRV_ERR",*nmb_last_error()?nmb_last_error():"cannot read metallib");nmb_device_destroy(device);return 0;}
    if(argc==3 && !strcmp(argv[1],"list")) {
        size_t i,count=nmb_library_function_count(library);char name[4096];
        printf("{\"status\":\"PASS\",\"function_count\":%zu,\"names\":[",count);
        for(i=0;i<count;++i) {
            if(!nmb_library_function_name(library,i,name,sizeof(name))){result=1;break;}
            if(i)putchar(',');
            json_string(name);
        }
        printf("],\"stage_kinds\":[");
        for (i = 0; !result && i < count; ++i) {
            uint32_t kind;
            if (!nmb_library_function_stage_kind(library, i, &kind)) { result = 1; break; }
            printf("%s%u", i ? "," : "", kind);
        }
        puts("]}");
    } else if(argc==8 && !strcmp(argv[1],"compile")) {
        char *end=NULL;uint64_t key;
        errno=0;key=strtoull(argv[7],&end,0);
        if(errno || !end || *end || !key || (strcmp(argv[3],"compute") && strcmp(argv[3],"render")))result=2;
        else result=compile_pipeline(device,library,argv[3],argv[4],argv[5],argv[6],key);
    } else result=2;
    nmb_library_destroy(library);nmb_device_destroy(device);return result;
}
#else
typedef int a840_corpus_driver_guard_t;
#endif /* A840_BUILD_CORPUS_DRIVER */
