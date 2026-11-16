/* SPDX-License-Identifier: MIT */
#include "NezhaCameraAfParking.h"
#include <stddef.h>
#include <math.h>

/* Original GenerateParkLens@5bb0 uses FMADD with the double literal0.6,
 * then truncates to an unsigned DAC. Each produced register00/DAC<<4
 * command carries10000us. Keep endpoint order supplied by the caller. */
bool nezha_camera_af_parking(uint16_t current, uint16_t first, uint16_t last,
    struct NezhaCameraAfParking *out)
{
    if (!out) return false;
    *out=(struct NezhaCameraAfParking){0};
    if (current>4095 || first>4095 || last>4095) return false;
    unsigned at=current;
    bool finished=false;
    while (out->count<NezhaCameraAfParkingMoves) {
        unsigned next;
        if (first<last) {
            if (at>last) return false;
            if (at<first) next=first;
            else if (last-at<=10) { next=last;finished=true; }
            else {
                next=(unsigned)fma((double)(last-at),-0.6,(double)last);
                if (next-at<10) next=at+10;
            }
        } else {
            if (at<last) return false;
            if (at>first) next=first;
            else if (at-last<=10) { next=last;finished=true; }
            else {
                next=(unsigned)fma((double)(at-last),0.6,(double)last);
                if (at-next<10) next=at-10;
            }
        }
        if (next>4095) return false;
        out->dac[out->count]=(uint16_t)next;
        out->delay_ms[out->count]=10;
        ++out->count;
        at=next;
        if (finished) return true;
    }
    return false;
}
