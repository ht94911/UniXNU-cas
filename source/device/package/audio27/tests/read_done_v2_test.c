/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Independent replies from the published AudioReach V2 field order. */
#include "../core/AudioStream.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t data_status, data_buf_addr_lsw, data_buf_addr_msw, data_mem_map_handle;
    uint32_t data_size, timestamp_lsw, timestamp_msw, flags, num_frames;
    uint32_t md_status, md_buf_addr_lsw, md_buf_addr_msw, md_mem_map_handle, md_size;
} ReadDoneV2;
typedef struct {
    uint32_t data_status, data_buf_addr_lsw, data_buf_addr_msw, data_mem_map_handle;
    uint32_t data_size, offset, timestamp_lsw, timestamp_msw, flags, num_frames;
    uint32_t md_status, md_buf_addr_lsw, md_buf_addr_msw, md_mem_map_handle, md_size;
} CachedReadDoneV2;
_Static_assert(sizeof(ReadDoneV2)==56, "published14-word reply");
_Static_assert(offsetof(ReadDoneV2,timestamp_lsw)==20, "timestamp is not an offset");
_Static_assert(offsetof(ReadDoneV2,md_status)==36, "published metadata fields");
_Static_assert(sizeof(CachedReadDoneV2)==60, "existing15-word reply");
_Static_assert(offsetof(CachedReadDoneV2,md_status)==40, "cached metadata fields");

static _Alignas(64) uint8_t memory[32768];
static NaSession session;
static NaStream stream;
static uint8_t packet[GPR_MAX_PACKET];
static unsigned first_slot, cases;

static unsigned prepare(void) {
    unsigned slot;uint32_t bytes=0;
    assert(!na_stream_prepare(&stream,&session,1000,2000,&slot,packet,&bytes));
    assert(bytes && !na_stream_commit(&stream,slot));
    return slot;
}
static void setup(int capture) {
    na_session_init(&session);assert(!na_session_link(&session,1));
    memset(memory,0,sizeof memory);
    assert(!na_stream_init(&stream,memory,0x100040000ULL,sizeof memory,
                          capture ? 960:1920,capture ? 2:4,17,29,capture));
    assert(!na_stream_mapped(&stream,0x77,session.generation));
    assert(!na_stream_start(&stream));first_slot=prepare();
}
static ReadDoneV2 reply(unsigned slot) {
    uint64_t address=stream.dsp_base+(uint64_t)slot*stream.stride;
    return (ReadDoneV2){.data_buf_addr_lsw=(uint32_t)address,
        .data_buf_addr_msw=(uint32_t)(address>>32),.data_mem_map_handle=stream.handle,
        .data_size=960,.timestamp_lsw=0xfedcba98,.timestamp_msw=0x12345678,
        .flags=0x80000000,.num_frames=1};
}
static uint32_t encode(uint32_t opcode,uint32_t token,uint32_t peer,
                       const void *payload,uint32_t bytes) {
    GprPacket p={.dst_domain=GPR_DOMAIN_APPS,.src_domain=GPR_DOMAIN_ADSP,
        .src_port=peer,.dst_port=17,.token=token,.opcode=opcode};
    uint32_t length=gpr_encode(packet,sizeof packet,&p,payload,bytes);
    assert(length);return length;
}
static uint32_t owned(const ReadDoneV2 *r) {
    return encode(AR_OP_READ_DONE,stream.slot[first_slot].token,29,r,sizeof *r);
}
static void wire_failure(const ReadDoneV2 *r,unsigned reason,unsigned diagnostic_slot) {
    assert(na_stream_done(&stream,packet,owned(r))==NA_WIRE);
    assert(stream.quarantine && !stream.running && stream.in_flight==1);
    assert(!stream.completions && stream.first_bad_done.reason==reason);
    assert(stream.first_bad_done.slot==diagnostic_slot);
    assert(stream.first_bad_done.expected_token==stream.slot[diagnostic_slot].token);
    assert(na_stream_reclaim(&stream,1,0,0)==NA_OWNERSHIP);
    assert(!na_stream_reclaim(&stream,1,1,0));++cases;
}

int main(void) {
    setup(1);ReadDoneV2 r=reply(first_slot);
    for(unsigned i=0;i<960;++i)memory[i]=(uint8_t)(i*17u+3u);
    uint32_t n=owned(&r);assert(!na_stream_done(&stream,packet,n));
    assert(stream.slot[first_slot].offset==0 && stream.completions==1 && !stream.in_flight);
    uint8_t pcm[960];uint32_t bytes=0;
    assert(!na_stream_take(&stream,pcm,sizeof pcm,&bytes) && bytes==960);
    assert(!memcmp(pcm,memory,bytes) && stream.frames==480);++cases;

    setup(1);r=reply(first_slot);r.data_size=0;r.num_frames=0;
    assert(!na_stream_done(&stream,packet,owned(&r)) && stream.completions==1);
    assert(!na_stream_take(&stream,pcm,sizeof pcm,&bytes) && bytes==0 && !stream.frames);++cases;

    setup(1);r=reply(first_slot);r.data_status=9;r.data_size=0;
    assert(na_stream_done(&stream,packet,owned(&r))==NA_REMOTE);
    assert(stream.last_remote_status==9 && stream.remote_errors==1 && !stream.bad_done);
    assert(stream.quarantine && stream.in_flight==1 && !stream.completions);++cases;

    setup(1);r=reply(first_slot);r.data_mem_map_handle++;
    wire_failure(&r,NaDoneAddress,first_slot);
    setup(1);r=reply(first_slot);r.data_buf_addr_lsw+=4096;
    wire_failure(&r,NaDoneAddress,first_slot+1);

    for(unsigned field=0;field<5;++field) {
        setup(1);r=reply(first_slot);
        switch(field) {
        case 0:r.md_status=1;break;
        case 1:r.md_buf_addr_lsw=0x1000;break;
        case 2:r.md_buf_addr_msw=1;break;
        case 3:r.md_mem_map_handle=0x77;break;
        default:r.md_size=16;break;
        }
        wire_failure(&r,NaDoneMetadata,first_slot);
    }
    setup(1);r=reply(first_slot);r.data_size=961;wire_failure(&r,NaDoneBounds,first_slot);
    setup(1);r=reply(first_slot);r.data_size=962;wire_failure(&r,NaDoneBounds,first_slot);
    for(unsigned size=48;size<=64;size+=4) {
        if(size==56 || size==60)continue;
        setup(1);r=reply(first_slot);uint8_t data[64]={0};memcpy(data,&r,sizeof r);
        n=encode(AR_OP_READ_DONE,stream.slot[first_slot].token,29,data,size);
        assert(na_stream_done(&stream,packet,n)==NA_WIRE && stream.first_bad_done.reason==NaDoneSize);++cases;
    }

    setup(1);r=reply(first_slot);uint32_t token=stream.slot[first_slot].token;
    n=encode(AR_OP_READ_DONE,token+100,29,&r,sizeof r);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && !stream.quarantine);
    assert(stream.first_bad_done.reason==NaDoneNoToken && stream.first_bad_done.slot==first_slot);
    assert(stream.first_bad_done.expected_token==token && stream.first_bad_done.payload[14]==0);++cases;
    setup(1);r=reply(first_slot);stream.slot[first_slot].state=NaBufferCpu;
    assert(na_stream_done(&stream,packet,owned(&r))==NA_IGNORED && !stream.quarantine);
    assert(stream.first_bad_done.reason==NaDoneState);++cases;
    setup(1);r=reply(first_slot);++stream.generation;
    assert(na_stream_done(&stream,packet,owned(&r))==NA_IGNORED && !stream.quarantine);
    assert(stream.first_bad_done.reason==NaDoneGeneration);++cases;
    setup(1);r=reply(first_slot);n=encode(AR_OP_READ_DONE,stream.slot[first_slot].token,30,&r,sizeof r);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && !stream.bad_done && !stream.completions);++cases;

    setup(1);r=reply(first_slot);CachedReadDoneV2 legacy={
        .data_buf_addr_lsw=r.data_buf_addr_lsw,.data_buf_addr_msw=r.data_buf_addr_msw,
        .data_mem_map_handle=r.data_mem_map_handle,.data_size=956,.offset=4,
        .timestamp_lsw=r.timestamp_lsw,.timestamp_msw=r.timestamp_msw,.flags=r.flags,.num_frames=1};
    for(unsigned i=0;i<960;++i)memory[i]=(uint8_t)(i+9u);
    n=encode(AR_OP_READ_DONE,stream.slot[first_slot].token,29,&legacy,sizeof legacy);
    assert(!na_stream_done(&stream,packet,n));
    assert(!na_stream_take(&stream,pcm,sizeof pcm,&bytes) && bytes==956);
    assert(!memcmp(pcm,memory+4,bytes) && stream.frames==478);++cases;

    setup(1);unsigned slots[4]={first_slot};
    for(unsigned i=1;i<4;++i)slots[i]=prepare();
    for(unsigned i=0;i<4;++i)memset(memory+slots[i]*stream.stride,(int)(i+1),960);
    unsigned order[4]={2,0,1,3};
    for(unsigned i=0;i<4;++i) {
        unsigned slot=slots[order[i]];r=reply(slot);
        n=encode(AR_OP_READ_DONE,stream.slot[slot].token,29,&r,sizeof r);
        assert(!na_stream_done(&stream,packet,n));
        if(i==0)assert(na_stream_take(&stream,pcm,sizeof pcm,&bytes)==NA_PENDING);
    }
    assert(stream.completions==4 && !stream.in_flight);
    for(unsigned i=0;i<4;++i) {
        assert(!na_stream_take(&stream,pcm,sizeof pcm,&bytes) && bytes==960);
        for(unsigned j=0;j<bytes;++j)assert(pcm[j]==i+1);
    }
    assert(stream.frames==1920);++cases;

    setup(0);uint32_t output[8]={(uint32_t)stream.dsp_base,(uint32_t)(stream.dsp_base>>32),stream.handle};
    n=encode(AR_OP_WRITE_DONE,stream.slot[first_slot].token,29,output,sizeof output);
    assert(!na_stream_done(&stream,packet,n) && stream.completions==1);
    assert(!na_stream_take(&stream,NULL,0,&bytes) && bytes==1920);++cases;
    printf("%u published56/cached60/ownership/metadata/bounds/ordered PCM scenarios pass; DSP replies modeled\n",cases);
    return 0;
}
