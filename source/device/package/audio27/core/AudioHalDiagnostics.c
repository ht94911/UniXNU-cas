/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioHalDiagnostics.h"
#include "AudioProtocol.h"
#include <string.h>
_Static_assert(sizeof(NaHalObservation)==32,"HAL observation ABI");
int na_hal_history_add(NaHalHistory *history,const NaHalObservation *event) {
    if (!history || !event || event->version!=NA_HAL_OBSERVATION_VERSION ||
        event->kind<NaHalConnected || event->kind>NaHalPublication || event->object>5 ||
        history->count>NA_HAL_HISTORY_CAPACITY) return NA_ARGUMENT;
    for (uint32_t i=0;i<history->count;++i)
        if (!memcmp(&history->records[i],event,sizeof *event)) return NA_OK;
    if (history->count==NA_HAL_HISTORY_CAPACITY) return NA_BUSY;
    history->records[history->count++]=*event;
    return NA_OK;
}
uint32_t na_hal_history_copy(const NaHalHistory *history,uint32_t first,uint32_t count,NaHalObservation *out) {
    if (!history || !out || !count || count>8 || history->count>NA_HAL_HISTORY_CAPACITY || first>=history->count) return 0;
    uint32_t available=history->count-first;if (count>available) count=available;
    memcpy(out,history->records+first,count*sizeof *out);return count;
}
