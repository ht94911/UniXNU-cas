/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* iPhone14,4 / 24A446 VirtualAudio, UUID 6776a3c82bff39188faa0a284018eb88.
 * The original N71 constructor installs its speaker definitions directly.
 * Read the board's VAD configuration with the same system path/parser and
 * move its owned map into the existing optional VAD map before construction.
 * The original tuning folders, HAL protection, error paths and caller remain.
 * Installed only by the existing original-byte-checked signed RAM page path.
 */
.text
.extern va_vad_path, va_read_config, va_move_map, va_destroy_config
.extern va_delete, va_resume, va_config_filename
.global nezha_receiver_graph_config67
nezha_receiver_graph_config67:
    sub sp, sp, #0xb0
    /* Const filename argument: the existing path builder copies it. */
    adrp x9, va_config_filename
    add x9, x9, :lo12:va_config_filename
    mov x10, #26
    mov x11, #0x20
    movk x11, #0x8000, lsl #48
    stp x9, x10, [sp, #0x88]
    str x11, [sp, #0x98]
    add x0, sp, #0x70
    add x1, sp, #0x88
    bl va_vad_path
    mov x0, sp
    add x1, sp, #0x70
    bl va_read_config
    /* x20 is the original wrapper's live RoutingSettings config. */
    add x0, x20, #0x1f8
    mov x1, sp
    bl va_move_map
    mov x0, sp
    bl va_destroy_config
    ldrsb w8, [sp, #0x87]
    tbz w8, #31, 1f
    ldr x0, [sp, #0x70]
    bl va_delete
1:
    add sp, sp, #0xb0
    add x1, sp, #8
    b va_resume
