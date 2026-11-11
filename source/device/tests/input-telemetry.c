// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../package/nezha27-input/NezhaInputTelemetry.h"
#include <pthread.h>
#include <assert.h>
#include <string.h>
static struct nzi_frame state;
static unsigned finished;
static void *writer(void *arg){(void)arg;for(uint32_t n=1;n<=100000;++n){uint32_t stamp=nzi_begin(&state);for(unsigned i=0;i<NZI_WORDS;++i)nzi_put(&state,i,n+i);nzi_end(&state,stamp);}__atomic_store_n(&finished,1,__ATOMIC_RELEASE);return 0;}
int main(void){uint32_t out[NZI_WORDS],prior[NZI_WORDS];memset(out,0x5a,sizeof(out));memcpy(prior,out,sizeof(out));
assert(!nzi_copy(0,out)&&!nzi_copy(&state,0));uint32_t stamp=nzi_begin(&state);assert(!nzi_copy(&state,out)&&!memcmp(out,prior,sizeof(out)));for(unsigned i=0;i<NZI_WORDS;++i)nzi_put(&state,i,100+i);nzi_end(&state,stamp);assert(nzi_copy(&state,out));for(unsigned i=0;i<NZI_WORDS;++i)assert(out[i]==100+i);
stamp=nzi_begin(&state);nzi_time(&state,10,UINT64_C(0xfedcba9876543210));nzi_end(&state,stamp);assert(nzi_copy(&state,out)&&nzi_u64(out,10)==UINT64_C(0xfedcba9876543210));
memset(&state,0,sizeof(state));pthread_t t;assert(!pthread_create(&t,0,writer,0));unsigned samples=0;do{if(nzi_copy(&state,out)&&out[0]){for(unsigned i=0;i<NZI_WORDS;++i)assert(out[i]==out[0]+i);++samples;}}while(!__atomic_load_n(&finished,__ATOMIC_ACQUIRE));assert(!pthread_join(t,0));assert(nzi_copy(&state,out)&&out[0]==100000);assert(samples);return 0;}
