/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* VirtualAudio24A446 source-spec constructor at1f2480. Keep every original
 * logical subport ID, mode, category and gain metadata. The board has one
 * measured PCM capture path; original built-in microphone source vectors
 * refer to that path. Other device source vectors retain their input.
 */
.text
.extern va_source_spec_resume
.global nezha_microphone_sources70
nezha_microphone_sources70:
    ldp x1, x2, [x2]
    cmp x1, x2
    b.eq 2f
    ldr w9, [x1]
    mov w10, #0x6963
    movk w10, #0x696d, lsl #16 /* imic */
    cmp w9, w10
    b.eq 1f
    movk w10, #0x736d, lsl #16 /* smic */
    cmp w9, w10
    b.eq 1f
    movk w10, #0x666d, lsl #16 /* fmic */
    cmp w9, w10
    b.eq 1f
    movk w10, #0x6c6d, lsl #16 /* lmic */
    cmp w9, w10
    b.ne 2f
1:
    adr x1, 3f
    add x2, x1, #4
2:
    b va_source_spec_resume
3:
    .word 0x696d6963
