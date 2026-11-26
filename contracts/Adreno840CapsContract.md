# Adreno 840 Hardware Capabilities & Facts Contract (`v1.0`)

本文件是 **Adreno 840（`gen80200` / SM8850 Kaanapali / 小米 17 Ultra `nezha`）** 的唯一硬件参数与能力表规范。
由统筹官冻结；任何泳道不得私自修改字段定义或新增未登记的寄存器魔法数字。

## 1. 硬件事实真源优先级（冲突裁决规则）

当 Mesa 静态表与本机内核/实机值存在差异时，按以下优先级裁决，并在 `kits/Adreno840/chip_facts_a840.h` 中显式记录差异原因：

1. **本机实测内核配置（最高优先级）**：
   - 源：`/home/hua/nezha-work/UniXNU-nezha/package/gpu/NezhaGpu.c`（以及 `NativeAdreno` 运行时 `GetDeviceInfo`）。
   - 已核实关键值：
     - `chip_id`: `0x44050a31`（含 enabled slice count；Mesa 匹配掩码 `0xffff44050a31`）
     - `gen`: `8`（Adreno 8xx / `gen80200`）
     - `gmem_range_min`: `0x04000000`（64 MiB）
     - `gmem_protect_end / gmem_size`: 需在 W00-L1 精确核对 `RB_GC_GMEM_PROTECT`（`0x12000000`）与 `NativeAdreno` 上报的可用 GMEM 字节数（3 slice 架构）
     - `ubwc_version`: `6.0`（Kaanapali `a8xx_set_ubwc_config`: `highest_bank_bit = 16`, swizzle levels 2+3, version tag 5, `yuvnotcomptofc`, `rgb565_predicator`, `amsbc`, `uavflagprd_inv = 2`, `ubwc_mode = 0`）
     - `uche_trap_base`: `0x1fffffffff000ULL`
     - `smmu`: 4 KiB stage-1 (`ACTLR = 0x32b`, stream 0 = `gfx3d_user`, CB 0)
     - `preemption_gmem_save_restore`: `RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE = 1`
2. **Pinned Mesa `85fda3b36405` 静态目录**：
   - 源：`/home/hua/nezha-work/ios27/gpu27-astra/out/mesa-a840-004/freedreno_devices.h`（`__info29`）与 `/home/hua/nezha-work/ios27/gpu27-astra/private/metal-source-002/.cache/mesa-work/src/freedreno/common/freedreno_devices.py`。
   - 已知差异（W00-L1 必须显式标出并提供运行时覆盖字段）：Mesa `__info29` 静态表写 `highest_bank_bit = 15`，而真实 Kaanapali `NezhaGpu.c` 硬件编程为 **`highest_bank_bit = 16`**。`chip_facts_a840.h` 必须把 `highest_bank_bit` 设为运行时可由内核 `GetDeviceInfo` 确认的值，并以 Kaanapali 实机值 `16` 作为本机默认值，严禁静默按 `15` 算 UBWC 布局！
   - `__info29` 其他关键架构参数：`num_slices = 3`, `num_ccu = 6`, `num_sp_cores = 6`, `gmem_align_w = 16`, `gmem_align_h = 4`, `tile_align_w = 96`, `tile_align_h = 32`, `tile_max_w = 16416`, `tile_max_h = 16384`, `num_vsc_pipes = 32`, `cs_shared_mem_size = 32768`, `fibers_per_sp = 4096`, `threadsize_base = 64`, `max_waves = 16`, `wave_granularity = 2`, `reg_size_vec4 = 128`, `instr_cache_size = 127`, `max_draw_states = 64`, `ubwc_swizzle = 6`, `macrotile_mode = 1`, `sysmem_per_ccu_color_cache_size = 131072`, `sysmem_per_ccu_depth_cache_size = 196608`, `gmem_per_ccu_color_cache_size = 16384`, `gmem_per_ccu_depth_cache_size = 262144`。

## 2. TSV 能力表 Schema（Phase 1 Lane 1 实现）

所有 TSV 使用 UTF-8、LF 行尾、Tab 分隔、`#` 注释行，由脚本生成 `kits/Adreno840/gen/Adreno840Caps.h`：

1. `kits/Adreno840/caps/formats.tsv`
   - 列：`mtl_pixel_format` | `mtl_name` | `pipe_format` | `a840_fmt_id` | `swap` | `bpp` | `ubwc_supported` | `renderable` | `filterable` | `blendable` | `msaa` | `grade` | `hwtest_id`
2. `kits/Adreno840/caps/features.tsv`
   - 列：`feature_id` | `mechanism` | `supported` | `grade` | `citation` | `hwtest_id`
   - `grade` 仅允许 `source`（源码/寄存器声明）、`host`（主机测试覆盖）、`chip`（小米 17 Ultra 实机验证通过）。
3. `kits/Adreno840/caps/limits.tsv`
   - 列：`limit_id` | `value` | `bound_kind`（`min`/`max`/`exact`） | `citation` | `notes`

## 3. 禁止手写魔法寄存器号准则

- 除 `kits/Adreno840/gen/Adreno840Regs.h` 与显式登记在 `kits/Adreno840/test/driver_constants.tsv` 的条目外，其余 C/C++/ObjC++ 源码不得出现裸 16 进制寄存器偏移或 PM4 opcode 常量。
- `tools/verify/driver_constants_test.py` 在每波门禁中强制扫描全部工作树。
