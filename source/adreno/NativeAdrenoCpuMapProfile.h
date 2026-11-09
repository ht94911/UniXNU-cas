/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_CPU_MAP_PROFILE_H
#define NATIVE_ADRENO_CPU_MAP_PROFILE_H
#include <stdint.h>

/* All access is under the existing driver lock. No allocation, extra IPC or
 * page ownership is involved. Token requests let a fast mapping wrapper retain
 * evidence of a long wait before its entry, including the IOKit IPC gate.
 * A missing/overwritten request or descriptor remains explicitly unpaired. */
struct NativeAdrenoCpuMapProfile {
    enum { Slots=16, SlowUs=8000 };
    struct Request {uint64_t thread=0,when=0,bytes=0;uint32_t token=0;};
    struct Descriptor {
        uint64_t thread=0,entered=0,locked=0,finished=0;
        uint32_t token=0,options=0,status=0,kind=0;
    };
    struct Sample {uint64_t value[12]{};bool emit=false;};
    Request requests[Slots]{};
    Descriptor descriptors[Slots]{};
    uint32_t next_request=0,next_descriptor=0;

    void requested(uint32_t token,uint64_t thread,uint64_t when,uint64_t bytes) {
        requests[next_request++%Slots]={thread,when,bytes,token};
    }
    void descriptor(uint32_t token,uint64_t thread,uint64_t entered,uint64_t locked,
                    uint64_t finished,uint32_t options,uint32_t status,uint32_t kind) {
        descriptors[next_descriptor++%Slots]={thread,entered,locked,finished,token,options,status,kind};
    }
    Sample finish(uint32_t token,uint64_t thread,uint64_t began,uint64_t ended,
                  uint32_t options,bool mapped) {
        Sample out{};
        out.value[0]=thread;out.value[1]=token;out.value[3]=began;
        out.value[7]=ended;out.value[10]=options;
        uint64_t flags=mapped?1u:0u;
        if(began && ended>=began)flags|=8u;
        Request *request=nullptr;
        for(auto &row:requests)
            if(row.token==token && row.thread==thread && row.when && row.when<=began &&
               (!request || row.when>request->when))request=&row;
        if(request) {
            out.value[2]=request->when;out.value[8]=request->bytes;flags|=2u;
            *request={};
        }
        Descriptor *callback=nullptr;unsigned matches=0;
        for(auto &row:descriptors)
            if(row.token==token && row.thread==thread && row.entered>=began &&
               row.locked>=row.entered && row.finished>=row.locked && row.finished<=ended) {
                callback=&row;++matches;
            }
        if(matches==1) {
            out.value[4]=callback->entered;out.value[5]=callback->locked;
            out.value[6]=callback->finished;
            out.value[11]=(uint64_t(callback->options)<<32)|callback->status;
            flags|=4u|(uint64_t(callback->kind)<<8);
            *callback={};
        } else if(matches>1)flags|=16u;
        out.value[9]=flags;
        const uint64_t first=out.value[2]?out.value[2]:began;
        out.emit=!mapped || !(flags&8u) || !out.value[2] ||
                 (ended>=first && ended-first>=SlowUs);
        return out;
    }
};
#endif
