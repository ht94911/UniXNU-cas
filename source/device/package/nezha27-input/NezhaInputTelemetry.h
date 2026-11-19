// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only observation of the owned producer and HID submission paths.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define NZI_WORDS 32
struct nzi_frame {uint32_t sequence,word[NZI_WORDS];};
enum {R_VERSION,R_PACKETS,R_IC_REPORTS,R_ERRORS,R_FRAME,R_MASK,R_DOWN,R_UP,R_PRODUCER,R_CONSUMER,R_DROP,R_ID,R_X,R_Y,R_TICKLO,R_TICKHI,R_STAGE,R_THRESHOLD};
enum {H_VERSION,H_ATTEMPT,H_RETURN,H_OK,H_ERROR,H_IN_CALL,H_FRAME,H_MASK,H_ACK_MASK,H_DOWN,H_UP,H_ID,H_X,H_Y,H_WIRE_X,H_WIRE_Y,H_CONTACTS,H_RESULT,H_STARTLO,H_STARTHI,H_ENDLO,H_ENDHI,H_COALLO,H_COALHI,H_READY,H_TICKS,H_RESERVED,H_STOP,H_SEEN,H_TICKLO,H_TICKHI};
static inline uint32_t nzi_get(const volatile struct nzi_frame *p,unsigned i) {return __atomic_load_n(&p->word[i],__ATOMIC_RELAXED);}
static inline void nzi_put(volatile struct nzi_frame *p,unsigned i,uint32_t v) {__atomic_store_n(&p->word[i],v,__ATOMIC_RELAXED);}
static inline uint32_t nzi_begin(volatile struct nzi_frame *p) {
    uint32_t before=__atomic_load_n(&p->sequence,__ATOMIC_RELAXED);
    __atomic_store_n(&p->sequence,before+1,__ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);return before;
}
static inline void nzi_end(volatile struct nzi_frame *p,uint32_t before) {__atomic_store_n(&p->sequence,before+2,__ATOMIC_RELEASE);}
static inline bool nzi_copy(const volatile struct nzi_frame *p,uint32_t *out) {
    if(!p||!out)return false;
    uint32_t before=__atomic_load_n(&p->sequence,__ATOMIC_ACQUIRE),temporary[NZI_WORDS];
    if(before&1)return false;
    for(unsigned i=0;i<NZI_WORDS;++i)temporary[i]=nzi_get(p,i);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(before!=__atomic_load_n(&p->sequence,__ATOMIC_ACQUIRE))return false;
    for(unsigned i=0;i<NZI_WORDS;++i){out[i]=temporary[i];}
    return true;
}
static inline void nzi_time(volatile struct nzi_frame *p,unsigned lo,uint64_t t) {nzi_put(p,lo,(uint32_t)t);nzi_put(p,lo+1,(uint32_t)(t>>32));}
static inline uint64_t nzi_u64(const uint32_t *v,unsigned lo) {return (uint64_t)v[lo]|((uint64_t)v[lo+1]<<32);}
#ifdef __cplusplus
extern "C" {
#endif
extern volatile struct nzi_frame nezha_raw_input_diag,nezha_hid_input_diag;
#ifdef __cplusplus
}
#endif
