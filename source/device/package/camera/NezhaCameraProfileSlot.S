/* SPDX-License-Identifier: MIT */
/* Same empty native-slot pattern as NezhaGpuFirmwareSlot.S. Local stock
 * inputs are inserted before strict prelink and never enter source-only CI. */
.section __DATA_CONST,__nezcam
.p2align 12
.globl _nezha_camera_profile_slot
_nezha_camera_profile_slot:
.space 131072
.section __DATA_CONST,__const
.p2align 3
.globl _nezha_camera_profile_slot_size
_nezha_camera_profile_slot_size:
.quad 131072
