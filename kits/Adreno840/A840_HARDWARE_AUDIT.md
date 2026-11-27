# Adreno 840 / Kaanapali 硬件事实审计（W00-L1）

结论：本机三 slice Adreno 840 v2 的默认 `chip_id` 为 `0x44050a31`，可用 GMEM 总量为 **18 MiB = 18,874,368 B = 0x01200000**，GPU GMEM 基址为 `0x04000000`。UBWC 编码器版本 **6**、版本 tag **5**、HBB **16**、swizzle **6**、macrotiling **1**。Mesa 静态 HBB **15** 是本波明确保留的差异，默认值按本机配置采用 **16**。

本审计于 2026-11-26 只读核对指定源码；没有读取手机或产生新的实机测量。这里的内核结果指已存在的本机配置和 `GetDeviceInfo` 实现，Mesa feature 位属于源码声明。host 测试验证库的记录与更新规则，不提升为 `chip` 能力等级。

## 1. 固定来源与精确定位

下表的 G/U/A/M/P/D/XML 为后文行号缩写；源文件均只读。

| 缩写 | 绝对路径 | 身份 / SHA-256 |
| --- | --- | --- |
| G | `/home/hua/nezha-work/UniXNU-nezha/package/gpu/NezhaGpu.c` | 所属 HEAD `bd5371e823be3f7c19d6695395b7bdb8139eb546`；文件 `64c7835c944e55804ef30b306ad0e9982c3e887a705a85df3643781dd7a5caa1` |
| U | `/home/hua/nezha-work/UniXNU-nezha/package/gpu/NezhaAdrenoGpu.cpp` | 补读实际 `GetDeviceInfo` 返回端（原任务的 ABI 头仅定义字段）；文件 `15bcfb478e42d4f9e83c64ab0935f2289d3054d1bdcb3e1cc5cba8d05f72f55a` |
| A | `/home/hua/nezha-work/ios27/gpu27-astra/userland/drivers/Adreno740/NativeAdrenoABI.h` | 文件 `24b6d71df96a4849137ad6869c0bceea7e87602066df37b12c57f0cfa17c87a7` |
| M | `/home/hua/nezha-work/ios27/gpu27-astra/out/mesa-a840-004/freedreno_devices.h` | `__info29` 在 **45** 行，匹配项在 **120** 行；文件 `2074e3ac9878f2344eca2d4f202060c5c5d75bc11c383adaee33e52b9ea9d40d` |
| P | `/home/hua/nezha-work/ios27/gpu27-astra/private/metal-source-002/.cache/mesa-work/src/freedreno/common/freedreno_devices.py` | 实际 Mesa HEAD `85fda3b36405bcaea8bffc64aa4f2651dc90770d`；文件 `3a343e614b30509a01db4a22380eaa4604abafbb59552cd8b177c5912b29aef6` |
| D | 同一 Mesa 的 `src/freedreno/common/freedreno_dev_info.py` | **130–166** 行是 A6xxGPUInfo 默认值、SP/CCU 派生及 slice 字段来源 |
| XML | 同一 Mesa 的 `src/freedreno/registers/adreno/a6xx.xml` | **2886–2890** 行登记 A8XX NC/GMEM_PROTECT 名称，未定义该保护寄存器的位域 |

G:4–8 明确来源是 pandora `wsl/pandora_gpu.c` 的 `91f1f2468f30841c39123c7ba20ae520ecd82c84`，结合 nezha 原设备树配置；本波复用其事实，没有修改该实现或寄存器序列。M 与 P 的架构数字经交叉核对；P:1396–1414 的 A840 继承链为 `a7xx_base → a7xx_gen3 → a8xx_base → a8xx_gen2`，不能只看 `a8xx_base` 的旧默认。

## 2. 身份、GMEM、tiling 与 UBWC 对照

`未上报` 表示 A 的固定 64 B DeviceInfo 没有这个字段；`未列` 表示该来源没有相应数字，不把缺字段当数值冲突。M 列中 `M:45` 是生成表的实际值；P/D 是生成定义的进一步核对。

| 字段 | 本机 G / U（精确行号） | A：GetDeviceInfo 字段 | Mesa M / P / D | 本库默认 / 裁决 |
| --- | --- | --- | --- | --- |
| `chip_id` | G:3217–3221：基值 `0x44050a01`，enabled-slice popcount 写 bits 4–7；U:419 返回实际组合值 | A:183，`uint32_t` | M:120、P:1397：catalog key `0xffff44050a31` | `0x44050a31`，零扩展存入 `uint64_t`；catalog fuse wildcard 不是 runtime ID |
| `chip_gen` | G:2、95：Adreno 840 v2 / gen80200 | 未上报 | M:45 `.chip=8`；P:1399 `A8XX` | `8`，一致 |
| `num_slices` | G:3217–3220：最多 3，按 enabled mask 计数 | 由 A:183 chip_id bits 4–7 携带 | M:45、P:1403：3 | `3`；本波只接受三 slice 的确切 chip_id |
| `num_ccu` | 未单独列为容量字段 | 未上报 | M:45、P:1402：6 | `6`，Mesa 来源 |
| `num_sp_cores` | 未单独列为容量字段 | 未上报 | M:45：6；D:155 由 num_ccu 派生 | `6`，不从 CP pipe mask 推导 |
| `gmem_range_min` | G:3121、3125–3126：`64ull << 20`；U:421 同值 | A:185 `gmem_base` | M:45 未列（由内核查询） | `0x04000000` = 64 MiB |
| `gmem_bytes` | U:414–420：**18u << 20**；G 本身未返回容量 | A:184 `uint32_t gmem_bytes`，无静态数值 | M:45 未列；D:41–45 说明 GMEM size 来自 kernel query | **18 MiB 总量**；3 slice，不再乘 3 |
| `RB_GC_GMEM_PROTECT` 原值 | G:3027、3184：**`0x12000000`**；G:3190、3195–3197 有读回代码 | 未上报 | XML:2889 仅寄存器名称，未给位域 | 单独审计原值；不是 gmem_bytes 或已知地址终点 |
| `gmem_align_w / h` | 未列 | 未上报 | M:45：16 / 4；D:148 | `16 / 4` |
| `tile_align_w / h` | 未列 | 未上报 | M:45、P:1404–1405：96 / 32 | `96 / 32` |
| `tile_max_w / h` | 未列 | 未上报 | M:45、P:1406–1407：16416 / 16384 | `16416 / 16384`，宽高均满足各自 tile alignment |
| `num_vsc_pipes` | 未列为该字段 | 未上报 | M:45、P:1408：32 | `32` |
| `ubwc_version` | G:2996、3135；U:427：6.0 / 6 | A:192 有字段；注释的 4.0 是泛用例子 | M:45 无 encoder-version 字段 | **6**，采用实际 A840 返回端；不沿用泛用注释的 4 |
| `highest_bank_bit` | G:3135：16−13=3；U:424：16 | A:189 字段 | M:45：**15**；P:1396–1414 未覆盖 D:138 的默认 15 | **16 默认**；保留 `A840_MESA_HIGHEST_BANK_BIT=15` 及显式 runtime 更新 |
| `ubwc_swizzle` | G:3135：levels 2+3；U:425：6 | A:190：bit0/1/2 对应 levels 1/2/3 | M:45：6；D:139 默认 `0x6` | `6`，一致 |
| `macrotile_mode` | U:417、426：8 channel / 1 | A:191：0 four channel，1 eight channel | M:45：1；D:139 | `1`，一致；不要把 1 当通道数量 |
| `ubwc_version_tag` | G:3135：5 | 未上报 | M:45 未列 | `5`，独立于 encoder version 6 |
| `ubwc_flags` | G:3135–3136：YUVNOTCOMPTOFC / RGB565_PREDICATOR / AMSBC；UAVFLAGPRD_INV=2，UBWC_MODE=0 | A:193 的 flags 是 DeviceInfo 能力标志，不是此配置 | M:45 未列等价 packed 配置 | 逻辑打包 **23 / 0x17**，下节给出位定义；不复制寄存器 image |

GMEM 精确边界按实际可用容量计算：`0x04000000 + 0x01200000 = 0x05200000`，即 `[0x04000000, 0x05200000)`。`0x12000000` 若当字节数是 288 MiB，若减去 64 MiB 基址是 224 MiB，两者均不能替代 U:420 的 **18 MiB**。指定 G 与 pinned XML 未解释该保护值的位域编码，因此本库不造 `gmem_protect_end` 数值，也不把保护寄存器原值写成可用容量。U 的字段返回实现提供了所需容量证据。

M catalog key 的高位不是新芯片型号：同一 Mesa 的 `src/freedreno/common/freedreno_dev_info.c`:45–54 明确把 bits 47–32 的 `0xffff` 当 fuse-ID wildcard。runtime 接口来自 A:183 的 32-bit chip_id；因此本库拒绝 `0xffff44050a31` 或其他高位扩展，防止截断后误接受。

## 3. Shader / compute / CCU / VPC 全字段对照

这些数字由 M:45 与 P/D 确认，G/U/A 未逐字段声明或上报它们，不能称三方都实测一致。

| 字段 | G / U | A | M:45 实际值 | P / D 精确来源与裁决 |
| --- | --- | --- | --- | --- |
| `cs_shared_mem_size` | 未列 | 未上报 | `32768` B | P:1409，32 KiB |
| `fibers_per_sp` | 未列 | 未上报 | `4096` | P:1411，128×2×16 |
| `threadsize_base` | 未列 | 未上报 | `64` | D:140、161，默认 64 |
| `max_waves` | 未列 | 未上报 | `16` | D:140、162 |
| `wave_granularity` | 未列 | 未上报 | `2` | P:1410 |
| `reg_size_vec4` | 未列 | 未上报 | `128` | P:1307 的 gen2 覆盖 a8xx_base:1245 的 96 |
| `instr_cache_size` | 未列 | 未上报 | `127` | P:765–766；源码记录 128 会 hang，保留 127 |
| `max_draw_states` | 未列 | 未上报 | `64` | P:862 |
| `vs_max_inputs_count` | 未列 | 未上报 | `32` | P:761；与 frozen compiler MAX_VARYINGS 数字一致，语义仍分别保留 |
| `max_sets` | 未列 | 未上报 | `8` | P:762；补入 facts 以直接核对 `ADRENO_COMPILE_MAX_BIND_SETS` |
| `sysmem_per_ccu_color_cache_size` | 未列 | 未上报 | `131072` B | P:1312，128 KiB **per CCU** |
| `sysmem_per_ccu_depth_cache_size` | 未列 | 未上报 | `196608` B | P:1313–1314，192 KiB，gen2 THREE_QUARTER；不是旧 gen1 256 KiB |
| `gmem_per_ccu_color_cache_size` | 未列 | 未上报 | `16384` B | P:1318–1319，16 KiB，EIGHTH |
| `gmem_per_ccu_depth_cache_size` | 未列 | 未上报 | `262144` B | P:1320–1321，256 KiB，FULL |
| `sysmem_vpc_attr_buf_size` | 未列 | 未上报 | `131072` B | P:1308，128 KiB，不标成 per CCU |
| `gmem_vpc_attr_buf_size` | 未列 | 未上报 | `49152` B | P:1315，48 KiB，不标成 per CCU |

前四个 cache 字段是 **每 CCU** 容量；六 CCU 的对应总和分别是 786432 / 1179648 / 98304 / 1572864 B。VPC 字段没有 per-CCU 标识，不能按同样规则乘六。M:45 与 P 的 gen2 数字一致，没有把 a7xx/gen1 的旧数值继承到 A840。

## 4. Quirks、features 与内核基础

`features` 使用固定宽度 `uint32_t` 掩码，避免 C/C++ 位域布局差异。以下十位默认全设，数值 `0x3ff`；每位在测试中独立检查并验证不得静默移除。它们没有与 Metal family、编译参数或 Native ABI capability flags 共用编码。

| bit / 字段 | G / U | A | M:45 / P | 默认与 Wave 1 注意项 |
| --- | --- | --- | --- | --- |
| 0 `has_early_preamble` | 未独立列 | 未上报 | True；P:791 | 1；compiler 请求须用自身 `A840_COMPILE_FLAG_EARLY_PREAMBLE` bit1 显式映射 |
| 1 `has_gmem_fast_clear` | G:3025 编程 RB_CCU_CNTL；不据原值重建此布尔量 | 未上报 | True；P:748 | 1；需同时保留具体格式 fast-clear quirk |
| 2 `has_coherent_ubwc_flag_caches` | 未独立列 | 未上报 | True；P:788 | 1；不能覆盖 bit4 的 coherency quirk 或当成全部 BO 免维护 |
| 3 `supports_uav_ubwc` | 未独立列 | 未上报 | True；P:844 | 1；formats.tsv 仍须按格式/usage 采用现成 Mesa 资格 |
| 4 `ubwc_coherency_quirk` | 未独立列 | 未上报 | True；P:850 | 1；保留相应上下文/资源同步路径 |
| 5 `r8g8_faulty_fast_clear_quirk` | 未独立列 | 未上报 | True；P:846 | 1；formats.tsv 不可因 general fast-clear=true 就忽略 R8G8 路径限制 |
| 6 `prede_nop_quirk` | 未独立列 | 未上报 | True；P:795 | 1；沿 Mesa compiler 的既有 NOP quirk 处理 |
| 7 `predtf_nop_quirk` | 未独立列 | 未上报 | True；P:796 | 1；独立保留对应 NOP quirk |
| 8 `has_salu_int_narrowing_quirk` | 未独立列 | 未上报 | True；P:1324 | 1；gen2 SALU narrowing 不能套用无 quirk 的 lowering |
| 9 `has_gmem_save_restore` | **G:3174**：RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE=1 | 未上报 | M:45 无此字段 | 1；这是既有 kernel enable，不代表 v3 PreemptRings 已实现 |
| `smmu_page_shift` | **G:55–57、94–98、567、572**：ArmSmmu4K、ACTLR=0x32b、GPU SID0/CB0；G:1867 用 `iova >> 12` | A:52、63–68 规定 4 KiB BO/map 粒度 | M:45 未列平台 SMMU | **12 = 4 KiB**；不要与 v3 64 KiB zero-page 大小混同 |
| `uche_trap_base` | **G:3121、3127–3130**；**U:422**：`0x1fffffffff000` | **A:186**，uint64_t | M:45 未列 | 相同 64-bit 地址；trap/write-through 各写入路径已定位 |

UBWC `ubwc_flags` 的逻辑编码如下。它是事实记录的自有掩码，**不是** G:3137–3143 的 GRAS/RB/SP/TPL1 寄存器值。

| 逻辑位 | 含义 | 默认 |
| --- | --- | --- |
| bit0 | YUVNOTCOMPTOFC | 1 |
| bit1 | RGB565_PREDICATOR | 1 |
| bit2 | AMSBC | 1 |
| bits3–4 | UAVFLAGPRD_INV 数值 | 2 |
| bits5–6 | UBWC_MODE 数值 | 0 |

因此 `ubwc_flags = 1 + 2 + 4 + (2 << 3) = 23`。HBB、swizzle、macrotile、encoder version、tag 仍为独立字段；runtime 改 HBB 不改变其他逻辑位。

## 5. HFI 频表与 context_init 后续输入

G:1051–1058 的原设备树频表保留全部 **18** 档，以下数值为源码 Hz / qcom level；M/A 不上报此表。

| Hz | level | Hz | level |
| ---: | --- | ---: | --- |
| 160000000 | 0x32 | 578000000 | 0x80 |
| 191000000 | 0x33 | 646000000 | 0x90 |
| 222000000 | 0x34 | 726000000 | 0xc0 |
| 282000000 | 0x36 | 826000000 | 0xe0 |
| 342000000 | 0x38 | 902000000 | 0x100 |
| 382000000 | 0x3c | 1000000000 | 0x180 |
| 422000000 | 0x40 | 1050000000 | 0x1a0 |
| 461000000 | 0x4c | 1100000000 | 0x1b0 |
| 500000000 | 0x50 | 1200000000 | 0x1c0 |

G:1062 `NGPU=1+18` 包含 off level，G:1225–1228 按该 19 项 HFI 表发送。不得把 off 档计入“18 档可运行频率”，也不能以表/软件票推断真实 PLL 频率或 120 Hz 画面。

Wave 1 的 `context_init_a840.tsv` 应沿 G:2994–3002 的平台初始化分工与 G:3117–3197 的已存在序列：GMEM/trap、UBWC 6.0 + HBB16、non-context/protect、GMEM save/restore、fault/interrupt masks，GMEM_PROTECT 在 zap 后。P:1254–1256 明确 a8xx 的多数 non-context/chicken programming 移到 kernel；M 的剩余 magic/raw-magic 不能覆盖本机 kernel 配置。这里只提供输入事实，不新写 TSV、寄存器生成器或任何 init 路径。

## 6. 本库布局、runtime 规则与冻结契约

- `A840ChipFacts` 为 **168 B**：3 个 uint64_t + 36 个 uint32_t；sizeof、首 scalar offset 24、末字段 offset 164 由 C11 `_Static_assert` / C++17 `static_assert` 锁定。39 个字段含本任务全部参数及用于 compiler 契约核对的 `max_sets`。
- 默认值静态只读；对运行时容量/HBB 必须复制后调用 `a840_chip_facts_apply_runtime`。仅确切 `0x44050a31` 可接受，高位 fuse/catalog wildcard、其他 revision/slice ID 拒绝。
- runtime GMEM 是显式可用容量输入。本波 API 采用 **正数、4 KiB 整倍数、≤18 MiB** 的范围/粒度检查；这是本库校验规则，未据此声称较小容量是该手机的新实测或解出 GMEM_PROTECT 位域。
- runtime HBB 本波仅接受两份证据中的 **15/16**；默认一直是 **16**。其他 HBB profile 尚不在本波支持范围，不声称其他 GPU 的 HBB 硬件取值非法。
- 全部非 runtime 字段必须与固定 A840 profile 相同，包括所有 quirks。完整候选校验成功后才赋值；失败保持原结构每个字节不变。
- `ubwc_flags` / `features` 为显式 uint32_t 掩码，不采用实现相关 bit-field；库仅依赖 C 标准 `<stddef.h>` / `<stdint.h>`，没有 Vulkan、Mesa、XNU 或外部运行库调用。
- 冻结 `contracts/AdrenoCompileABI.h` 的 ABI version 1、MAX_VARYINGS 32、MAX_BIND_SETS 8 与测试一致；`A840CompileTarget` 40 B，chip/slices/CCU/HBB/swizzle/macrotile 全字段映射独立验证。
- 冻结 `contracts/NativeAdrenoABI_v3.h` 的 v2/v3 version 2/3、3104 B envelope、SurfaceMeta 48 B、UBWC 6/HBB16/swizzle6/macrotile1 fixture、method25/26 与测试一致。硬件默认在冻结头中是注释，未虚构不存在的硬件数值常量；本波不实现 v3 方法。
- 冻结 caps 合同 SHA-256：`83c375010d90b56f7b565863e00adedddfc9d7baa14a81460d5e385b0dc1f13a`；冻结 v3 合同：`468c478b5c70ece0f9c712545d51b0ebf7b24ba3dfc14cd16b5ecd05f141dc3a`；compiler 合同：`c9dcb74ef4a3d35d284bdea70139b1d425165adca2079cdfcae89f5eff1a1528`。全程只读。

## 7. 主机验证与交付范围

测试程序将所有 39 个默认字段与独立预期对照，并逐位检查十个 feature/quirk。4 组合法 runtime 覆盖（含 HBB15/16 与容量边界），非法 chip/HBB/未对齐/零/超容量值在默认记录与已覆盖记录上检查；48 组固定字段/标志拒绝，合计 **101** 次 runtime 失败均字节保持。覆盖 NULL、零长度、1/2 B 错误缓冲区与 guard byte、成功清空理由、只读默认保留和冻结契约，总 **628** 项检查。

WSL Ubuntu GCC/G++ **13.3.0** 与 Clang/Clang++ **18.1.3** 均以 C11 / C++17、`-Wall -Wextra -Werror -pedantic -fsanitize=address,undefined` 编译、链接并运行通过；ASan leak detection 与 UBSan halt-on-error 已开启。系统 Clang 缺 compiler-rt，匹配 `libclang-rt-18-dev=1:18.1.3-1ubuntu1` 仅下载并解包到本任务 cache；Clang 命令显式增加 `-resource-dir` 指向 cache，并 `-isystem` 使用原系统 Clang 18 builtin headers。未删除任何告警或 sanitizer 参数。

编译/日志/运行库/产物全部在 `/home/hua/nezha-work/ios27/adreno840-native/.cache/w00-l1`；Ubuntu VHDX 已核位于 `I:\Code\UniXNU-nezha\.wsl\ext4.vhdx`。首轮 GCC 的文件传递转义错误输出保存为 `gcc-results-initial-failed.json`，改用逐字节写入核同后通过。最终真实命令与输出见同 cache 的 `gcc-results.json` / `clang-results.json`，以及指定 `receipts/W00-L1.md`。

本波只交付 Allowed Write Paths 的四个文件与指定回执；没有手机、ADB、fastboot、push、contracts/BOARD/其他泳道或 integration088 修改。Wave 1 按本审计的 HBB 差异、容量单位、quirk 与 kernel 分工继续；无需本波手机验证。
