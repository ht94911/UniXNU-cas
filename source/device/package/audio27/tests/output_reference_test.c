/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../hal/AudioReference.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t output[NA_AUDIO_RING_FRAMES], reference[NA_AUDIO_RING_FRAMES];
int main(void) {
    int16_t left, right, played_left, played_right;
    assert(na_reference_read(reference, 100, &left, &right) == NA_MISSING);
    assert(left == 0 && right == 0);
    assert(na_reference_mix(output, reference, 100, 12000, -8000) == NA_OK);
    assert(na_reference_mix(output, reference, 100, 24000, -28000) == NA_OK);
    assert(na_ring_take(output, 100, &played_left, &played_right) == NA_OK);
    assert(played_left == 32767 && played_right == -32768);
    assert(na_reference_read(reference, 100, &left, &right) == NA_OK);
    assert(left == played_left && right == played_right);
    assert(na_reference_mix(output, reference, 100, 2000, 3000) == NA_OWNERSHIP);
    assert(na_reference_read(reference, 100, &left, &right) == NA_OK);
    assert(left == played_left && right == played_right);

    /* The same slot in a later wrap must not expose an earlier period. */
    uint64_t future = 100 + NA_AUDIO_RING_FRAMES;
    assert(na_reference_mix(output, reference, future, -123, 456) == NA_OK);
    assert(na_reference_read(reference, 100, &left, &right) == NA_MISSING);
    assert(na_ring_take(output, future, &played_left, &played_right) == NA_OK);
    assert(na_reference_read(reference, future, &left, &right) == NA_OK);
    assert(left == played_left && right == played_right);
    assert(left == -123 && right == 456);
    assert(na_reference_mix(output, reference, 100, 1, 2) == NA_OWNERSHIP);

    /* Silence already consumed by the DSP remains silence in the reference. */
    assert(na_ring_take(output, 101, &played_left, &played_right) == NA_MISSING);
    assert(na_reference_mix(output, reference, 101, 3000, 4000) == NA_OWNERSHIP);
    assert(na_reference_read(reference, 101, &left, &right) == NA_MISSING);
    assert(left == 0 && right == 0);
    memset(output, 0, sizeof output); memset(reference, 0, sizeof reference);
    assert(na_reference_read(reference, future, &left, &right) == NA_MISSING);
    assert(na_reference_mix(output, reference, 100, 9, -9) == NA_OK);
    assert(na_reference_read(reference, 100, &left, &right) == NA_OK);
    assert(left == 9 && right == -9);
    /* A stopped pending frame must not accumulate the old source when the
     * next stream immediately reuses its valid future timestamp. */
    assert(!na_reference_mix(output,reference,200,24000,-24000));
    na_ring_silence(output);na_ring_silence(reference);
    assert(!na_reference_mix(output,reference,200,8000,-8000));
    assert(!na_ring_take(output,200,&played_left,&played_right));
    assert(played_left==8000 && played_right==-8000);
    assert(!na_reference_read(reference,200,&left,&right) && left==played_left && right==played_right);
    assert(na_reference_mix(output,reference,200,1,1)==NA_OWNERSHIP);
    puts("accepted output reference: mix/clip/late/wrap/epoch and StopIO restart without old PCM accumulation PASS");
    return 0;
}
