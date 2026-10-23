# cas RAM 内存与日志诊断

[English](cas-diagnostics.md)

独立 `cas-diag-002` profile 构建只在 RAM 中运行的 EFI 诊断程序，用于收集当前内存依据、拒绝未取得所有权的高地址访问，并检查结构化 RAM 日志的恢复。程序不加载 XNU。默认 `cas.dsc` / `cas-complete-001` 路线保留原行为。

## 构建和打包

使用登记的相邻源码目录和固定环境。候选构建前提交改动；打包器要求源码干净且与成功构建时的身份一致。每个输出目录都必须全新。

```sh
source environment/cas-env.sh
python3 -m unittest discover -s tests -v
python3 scripts/build_firmware.py --uefi ../uefi-cas --diagnostics \
  --out "$HOME/cas-work/builds/cas-diag-002"
python3 scripts/package_boot.py --uefi ../uefi-cas \
  --firmware-build "$HOME/cas-work/builds/cas-diag-002" \
  --environment environment/cas-env.sh --stock-dtb /path/to/private/boot-02.dtb \
  --efi-ram-fat --diagnostics --out "$HOME/cas-work/artifacts/cas-diag-002"
```

原 stock DTB 的固定身份、RAM 临时启动与恢复流程见[构建说明](build.zh-CN.md)和[操作说明](operations.zh-CN.md)。打包器绑定诊断固件、shim 和 FAT，拒绝默认/诊断 profile 混用。FAT 只允许一个 EFI 路径 `\EFI\Tools\CasDiag.efi`，另有普通回 Android 和返回固件动作；没有 donor、内核或根卷。脚本不调用 ADB/Fastboot。

诊断 DSC/FDF 使用原有受限 heap 和不含物理存储驱动的选择，新增源码 CasDiag，设置 `CAS_XNU_DIAGNOSTICS=1`，保留日志尾槽并限制普通串口日志范围。它不把高地址 RAM 导入 GCD 或 UEFI 分配器。

## 菜单与当前依据

音量键选择，短按电源执行，长按电源返回。日志分有限页显示，短按下一页，长按退出。只有选中明确的菜单项才请求 warm reset。

| 项目 | 动作 |
| --- | --- |
| 枚举 | 读取本轮有界 FDT 副本、RAM/保留区、当前 UEFI 描述符、GOP，以及有界原始 SMEM RAM 表；结构化记录写入固件自有 RAM 槽。 |
| 测试高地址 256 MiB | 重新枚举并满足所有门限后，才请求精确 UEFI 分配；拒绝时不访问高地址。 |
| 读日志 | 显示上一记录的校验状态、构建、序号、flags、boot stamp，并分页显示本轮/历史文本。 |
| Warm reset | 提交 reset-request 标记后调用 `ResetSystem(EfiResetWarm)`；后续检查标记是否还在，不预设保存成功。 |
| 返回 | 提交退出标记，返回 UniXNU。 |

枚举不修改 FDT、SMEM、存储、变量或分配器拓扑；快照使用普通有界 pool，诊断文本只写自有 RAM 日志。诊断专用 memory-map 库只将既有 SMEM 保留区 `[0x80900000, 0x80b00000)` 映射为 NC reserved，SEC 在进入 DXE 前设置 RO/XP；默认生成表保持原内容，分配器仍为 128 MiB。任何 SMEM 读取前，程序要求**本次** FDT `qcom,smem` 的 `memory-region` phandle 恰好指向该 `no-map` 保留区、当前 UEFI reserved/UC 描述符覆盖该区，且 Memory Attribute Protocol 实际确认 RO/XP、无 RP。不获取硬件锁、不写 SMEM。动态或未解析的 FDT 保留区同样阻止探测。

`SmemCore.c` 只读分配元数据和 item 402。支持 SMEM heap 11、12（12 仅限 **uncached global partition**），RAM 表 1/2/3 版分别采用 24 字节 header 后的 64/72/72 字节条目。辅助区、仅 cached 项、未知版本、重复/重叠、magic/count/padding 错误及溢出全部拒绝。所有读取限制在 2 MiB 内，每次最多 4096 字节、每轮最多 1200 次读取与 64 KiB 轨迹、最多 32 行 RAM 表。两轮观察的有序 offset、length、原始字节必须完全相同；这能发现观察到的数据变化，不声称原子快照，也不替代 Qualcomm 远程锁协议。不读、不记录其他 item 的 payload。

线格式核对来源为 [Linux v4.19 SMEM](https://github.com/torvalds/linux/blob/v4.19/drivers/soc/qcom/smem.c)、[固定 LK RAM 表定义](https://github.com/msm8916-mainline/lk2nd/blob/5e6f1adcb1c070e28a70dc1f3d807ce9538f61c5/platform/msm_shared/smem.h)、[Qualcomm v3 RAM 表定义](https://www.mail-archive.com/u-boot%40lists.denx.de/msg570801.html)。有界解码器独立编写，没有引入这些驱动代码。

每行数字字段均输出；仅明确 read/write、APPS/shared domain 的 SDRAM/system-memory 行参与高地址门限，其他行及不可用尾部作为排除区。未知或默认 attr/domain 只能成为观察，不能成为所有权。

FDT 只能来自已验证的 cas 固定地址和大小，RAM/保留区必须使用支持的双 cell 宽度，每个区间均检查。输出包含 FDT CRC 和完整 UEFI 描述符的类型、物理/虚拟地址、长度、属性。当前 GOP 用于排除 framebuffer；历史 RAM bank 本身不能证明所有权。

## 探测契约

固定测试候选为 `[0x100000000, 0x110000000)`，256 MiB，位于较大的历史 512 MiB 调查区。它必须 2 MiB 对齐，并完整位于一条当前 FDT bank、一条当前 RAM-partition entry 和一段当前 UEFI conventional WB 区间内。当前 FDT 保留区、不合格 SMEM 行/尾部和 framebuffer 全部排除；低 4 GiB 的整体排除还覆盖固件、栈、输入走廊、控制与日志区。算术、数量、重叠、缺失依据均失败即拒绝。

当前固件只发布原有 **128 MiB 低地址 allocator**，因此本 profile 预期会**拒绝高地址测试**。拒绝是诊断结果，不是高地址通过。程序不会用 `AddMemorySpace`、历史 bank 导入或直接读写绕过门限。X-05 应先收集真实输出，再决定后续 allocator 候选如何调整。

门限通过后才执行共享 C 事务：

1. `AllocatePages(AllocateAddress, EfiLoaderData)` 必须成功且返回精确请求地址。
2. 对整段分配写入随完整物理地址变化的 64 位 pattern。
3. clean/invalidate cache、fence 后逐 word 读回；再以反码重复。
4. 清零整段、再次 clean/invalidate 和 fence，然后释放 pages。
5. 只有读回、清理和释放全部成功才通过。

异常返回地址只释放，不读写。普通 write/cache/read 错误返回统一进入清理和释放，主机 callback 测试覆盖这些路径；它们不证明 CPU exception、reset 或 bus fault 后仍能清理，仍需真机观察。

## 保留日志格式

诊断专用槽位于既有固件自有 log 区的尾 64 KiB：`[0x9f970000, 0x9f980000)`，不新增物理 RAM 所有权。诊断 shim 只清低 `0x30000` 字节，并将新的 `CNTVCT_EL0` boot stamp 放入 boot-info `Reserved[0]`；普通串口日志避开尾槽。默认 shim 仍清空原日志全区。

记录为小端，header **96 字节**：

| 偏移 | 字段 |
| ---: | --- |
| 0 | 八字节 `CASLOG01` magic，最后提交 |
| 8、12 | `u32` version=1、header length=96 |
| 16、20 | `u32` 总长度、ASCII payload 长度 |
| 24、32 | `u64` sequence、boot stamp |
| 40、44 | `u32` flags、CRC32 |
| 48 | NUL 结尾、补零的 32 字节 build ID |
| 80 | 16 字节保留零 |

CRC32 覆盖 header 和 payload，计算时 CRC 字段视为零。写入先使 magic 无效，再写有界 body/checksum、fence，最后提交 magic。读取器拒绝残缺、非法长度/版本/flags、错误 padding/CRC、非 ASCII 文本；其他 build ID 单独报告。stamp 相同表示同次启动复用，不同只表示**前次启动候选记录**，不能自动判定持久化通过。ABL、安全固件或 reset 都可能清除或覆盖该区。

EFI viewer 是机上的恢复读取器。已保存到本地的 slot 还可离线解码：

```sh
python3 scripts/read-cas-diag-log.py /path/to/saved-slot.bin \
  --build cas-diag-002 --current-boot 0x1234
```

该命令只读文件，不会取得物理内存 dump。诊断格式尚未接入 XNU panic/logging；该接口属于 X-06。

## 验证边界

可移植 `DiagCore.c` 就是 EFI 使用的范围门限、日志 codec 和分配/清理事务。24 项主机测试实际编译并调用该 C 代码，与独立 Python reader 交叉核对记录、破坏/截断记录并注入 callback 失败。SMEM 测试实际编译并调用 `SmemCore.c`，使用合成内存注入元数据/内容变化和读取失败，覆盖坏表、未知版本、越界、cached/辅助区拒绝和读取预算。独立 C harness 编译真实诊断映射 wrapper，核对只改变 SMEM HOB 选项、重复调用稳定、类型错误拒绝，且 conventional heap 仍为 128 MiB。各次实际测试数量和构建结果以候选报告为准。

完整固件/镜像构建报告另行标识实际产物。编译和主机测试不能证明当前 cas 内存所有权、高地址 cache 行为、实体按键、warm reset 保留或 XNU/iOS 启动；这些必须来自新镜像哈希绑定的 X-05 真机观察，不能复用旧 UEFI 候选的成功结果。
