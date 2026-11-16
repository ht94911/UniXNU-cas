/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../core/AudioHalDiagnostics.h"
#include "../core/AudioProtocol.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    NaHalHistory history={0};
    NaHalObservation event={NA_HAL_OBSERVATION_VERSION,NaHalClockResult,1,0,0,0,INT32_MIN,UINT32_MAX};
    assert(na_hal_history_add(&history,&event)==NA_OK && history.count==1);
    assert(na_hal_history_add(&history,&event)==NA_OK && history.count==1);
    NaHalObservation invalid=event;invalid.version++;
    assert(na_hal_history_add(&history,&invalid)==NA_ARGUMENT && history.count==1);
    invalid=event;invalid.kind=0;
    assert(na_hal_history_add(&history,&invalid)==NA_ARGUMENT);
    invalid.kind=NaHalClockResult+1;
    assert(na_hal_history_add(&history,&invalid)==NA_ARGUMENT);
    invalid=event;invalid.object=6;
    assert(na_hal_history_add(&history,&invalid)==NA_ARGUMENT);
    for (uint32_t i=1;i<NA_HAL_HISTORY_CAPACITY;++i) {
        event.kind=NaHalPropertyError;event.selector=i;
        assert(na_hal_history_add(&history,&event)==NA_OK);
    }
    assert(history.count==NA_HAL_HISTORY_CAPACITY);
    assert(na_hal_history_add(&history,&event)==NA_OK); /* duplicate at capacity */
    event.selector=UINT32_MAX;
    assert(na_hal_history_add(&history,&event)==NA_BUSY && history.count==NA_HAL_HISTORY_CAPACITY);
    struct {uint64_t first;NaHalObservation records[8];uint64_t last;} output={0};
    output.first=output.last=0x123456789abcdef0ULL;
    assert(na_hal_history_copy(&history,0,8,output.records)==8);
    assert(output.records[0].result==INT32_MIN && output.records[0].value==UINT32_MAX);
    assert(output.records[7].selector==7 && output.first==output.last);
    assert(na_hal_history_copy(&history,63,8,output.records)==1 && output.records[0].selector==63);
    assert(na_hal_history_copy(&history,64,8,output.records)==0);
    assert(na_hal_history_copy(&history,0,9,output.records)==0);
    assert(na_hal_history_copy(&history,0,0,output.records)==0);
    assert(output.first==0x123456789abcdef0ULL && output.last==output.first);
    puts("HAL numeric observations: identity, duplicate/capacity and bounded pages passed");
}
