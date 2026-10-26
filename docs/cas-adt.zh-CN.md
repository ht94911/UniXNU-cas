# cas Apple Device Tree 生成器

[English](cas-adt.md)

X-03 为固定的 cas CPU/平台内核路线生成并审计最小 Apple Device Tree（ADT）。2026-10-24 的主机检查已通过。所有报告仍明确标记 `boot_ready: false`、`hardware_tested: false`；当前内存所有权、GOP、异常级和 XNU 实际运行属于后续任务。

## 输入和命令

使用 Python 3.10 或以上。生成器仅使用标准库；可选的真实消费代码探针还需要 Git、C 编译器及固定的本地框架/XNU 输入。

```sh
# 可重复、无熵的主机 fixture，不能作为启动输入。
python3 scripts/make-cas-adt.py --mode fixture --out /tmp/cas-adt-fixture-001

# 输出八个已观察到的 affinity，仅供主机审计。
python3 scripts/make-cas-adt.py --cpu-profile all --mode fixture --out /tmp/cas-adt-all-001

# 同时核验本地原始证据的哈希和字段来源。
python3 scripts/make-cas-adt.py --mode fixture \
  --evidence-root "$EVIDENCE_ROOT" --out /tmp/cas-adt-traced-001

# 使用新 OS 熵，输出保存在私有 Linux 文件系统。
python3 scripts/make-cas-adt.py --mode private \
  --evidence-root "$EVIDENCE_ROOT" \
  --out "$HOME/cas-private/adt-001"

python3 -m unittest discover -s tests -p test_cas_adt.py -v
```

输出目录必须全新，包含 `cas.adt`、`audit.json`、`provenance.json`。溯源表覆盖每个属性，包括各节点的 `name`，记录 cas 事实、明确的软件输入或 schema 策略。报告仅记录随机种子的长度和文件哈希，不记录其字节。私有模式要求 POSIX 权限实际生效：目录 `0700`，文件 `0600`。无法落实权限时拒绝输出；本机 I: 盘的 WSL DrvFS 已实测会被拒绝。失败可以留下空目录，该目录不能重用。

事实文件按 SHA-256 固定三份原始输入：本地历史 cas 采集、`config/memory-map.json`、`config/display-profile.json`。指定 `--evidence-root` 后，程序核验文件身份，并独立从原文重新提取所选硬件字段。未提供原始证据时，来源核验明确为 `not_run`。公开仓库只保存选出的硬件事实，不包含原始采集或设备专属标识。

## 编码与消费契约

`config/cas-adt-schema.json` 是机器可读的消费契约，不是通用 JSON Schema。`scripts/adt_codec.py` 使用小端节点/属性计数、32 字节属性名及四字节对齐的 payload。解码器拒绝重复名称、非零 padding、残缺/尾随字节、无效字符串、整数截断及超限的数量、深度和大小。生成结果必须通过解码、重新编码的逐字节比较。

消费契约固定框架 `67e8ef0`、ArmCpu `1951cc8`、ArmDTPlatform `0d54470`、DarwinBoot `d099354` 与 XNU `d8b8029` / 10063.141.1。完整提交和十二份消费文件的精确哈希保存在 schema 中。继续沿用 iPadOS 17.7.11 / 21H461 参考目标；先前记录的 donor 版本差异仍未证明兼容。

| 路径/属性 | 编码与依据 |
| --- | --- |
| `/` | 根名称 `device-tree` 用于 ArmDTPlatform 匹配；model、compatible 使用真实 cas 字段。 |
| `/chosen` | `osenvironment=main`；DRAM base/total 仅作描述；`darwinboot-memory-size` 为一个 `u64` 分配请求。 |
| `/chosen/memory-map` | 本输入不带 ramdisk、trust cache、addon，故为空。加载器可在副本中补充后续映射。 |
| `/cpus/cpuN` | `reg`、`timebase-frequency` 均为 `u32`；`device_type` 精确为 `cpu\0`。CPU0 为 `running`，其余为 `waiting`。 |
| `/arm-io/ranges` | 三个 `u64`：逻辑子总线基址、物理父地址、长度；窗口仅包围两个 GIC 区域。 |
| `/arm-io/intc` | `arm,gic-v3\0`；四个 `u64` 表示两个相对基址/长度对；stride 为 `u64`。 |
| `/psci` | 已采集的 `arm,psci-1.0`、`smc`。 |
| `/timer` | 架构定时器及原始 PPI cells 的描述；不设置 `device_type=timer`，不虚构 MMIO `reg`。 |
| `/memory`、`/reserved-memory` | 以 `u64` 基址/长度对描述历史 RAM bank 和保留区；不能替加载器取得或保护内存。 |
| `/defaults`、`/options` | ArmDTPlatform 所需节点；`sleep-disabled=1`，options 为空。 |

八个历史 affinity 为 `0x000, 0x100, ... 0x700`，默认只输出 CPU0。八核树不证明 SMP；固定 ArmCpu 的从核路线仍需后续候选显式启用 `sheng-smp`。历史定时器频率为 19,200,000 Hz；虚拟定时器 PPI 11 对应 ArmCpu 直接使用的 INTID 27。省略旧式 timer device type，避免 `pe_identify_machine.c` 将架构定时器误当 MMIO 定时器。

GICD 为 `0x17a00000/0x10000`，GICR 为 `0x17a60000/0x100000`，stride 为 `0x20000`。审计同时检查整数宽度和总线翻译后的物理地址。

NVRAM proxy 是新建的空 8 KiB CHRP v1 bank：32 字节 Apple header、包含 2 KiB 空变量空间的 `common` 分区及剩余 `free` 分区。所有分区校验和与整 bank Adler32 均核验，generation 为零，变量数为零；没有使用设备 NVRAM dump。

DarwinBoot 从**内核路径加 `.adt` 后缀**读取 Apple tree。请求中的 `DeviceTree` 是 FDT 输入，不是 ADT 交付位置。加载器根据当前 UEFI memory map 分配 owned window，并从当前 GOP 填写 `boot_args.Video`；这些接口要在 X-06 接通。

## 内存角色与省略项

所有区间采用左闭右开边界。静态审计覆盖三个互不重叠的历史 RAM bank、31 个固件/保护区和六个 MMIO 范围，并检查对齐、溢出、包含关系及重叠。历史 bank 总长度为 `0x2f9900000` 字节，不代表连续空闲区间。

高地址候选为 `[0x100000000, 0x120000000)`，大小 512 MiB，加载器请求 256 MiB。候选位于一段历史 bank 内且避开已记录保留区；**当前分配器所有权尚未验证**。已验收固件的分配窗口仍只有 `[0x94000000, 0x9c000000)` 的 128 MiB，小于 DarwinBoot 256 MiB 最低要求。X-04/X-05 必须先解决这一缺口。

整个历史 splash 区 `[0x9c000000, 0x9e300000)` 都在保护清单中。1080×2340 显示配置的 framebuffer 基址、stride、像素格式为空，必须从当前 GOP 经 `boot_args.Video` 交接。生成器不输出假的 framebuffer 节点，也不选定 XNU 日志地址。

未知属性和节点直接拒绝。没有从 umi 导入 Apple 序列号/chip identity、MAC、UUID、旧随机种子、根卷绑定、分区选择、保存的 NVRAM 变量、RAMDisk、trust cache 或 GPU addon。私有模式只增加 64 字节新 OS 熵，其产物仍需后续运行条件验证。

## 主机验证结果与复现

最终 **53 项测试通过**，覆盖畸形编码、区间边界、来源路径、CPU/GIC 宽度、NVRAM 损坏、禁用字段、溯源覆盖、round trip 和 CLI 行为。两份公开 fixture 无熵且可重复：

| 配置 | 字节数 | SHA-256 |
| --- | ---: | --- |
| CPU0 | 13,864 | `b0d09551ce2e8098953fa6679f52c63777f71b23812ce5aea21ca9186c65b9a4` |
| 八核，仅主机 | 15,712 | `2ba1f2a320a556b99ce2e4a7f03c7aa103b3c940762dd04f148e18a2b1e141a4` |

独立检查实际编译了固定 XNU 的 `device_tree.c`、DarwinBoot 原始 `AdtNode` 函数及 CHRP header/checksum helper。host shim 替代其内核/UEFI 依赖，保留解析语句。探针对照 ArmCpu/ArmDTPlatform 的真实读取契约，但没有执行完整驱动。三个正例通过；截断、CPU reg 宽度错误、GIC 地址翻译错误、NVRAM checksum 错误分别触发对应断言。四个拒绝结果表示探针契约生效，不代表生产驱动能安全拒绝所有畸形树。

```sh
python3 scripts/audit-cas-adt-consumers.py \
  --framework ../UniXNU \
  --xnu ../build/xnu-work \
  --evidence-root "$EVIDENCE_ROOT" \
  --out "$HOME/cas-private/consumer-001"
```

探针要求小端 64 位主机。固定输入保持只读，先校验源码哈希和模块提交，再编译。新的私有输出保存原始许可证、编译器身份、命令及各用例结果。管理归档为 `records/xnu-adt/xa01-001`，其中源码快照和测试证据与唯一可写源码 checkout 分开。X-03 没有操作手机，也没有构建新的固件镜像。

## 来源说明

Apple tree 布局与空 CHRP 思路参考已保存的 [UniXNU-umi ADT 实现](https://github.com/reAAAq/UniXNU-umi/blob/2afd98a065d60d1c067e29b4be4611962a7db8d7/scripts/umi_adt.py) 及固定 UniXNU/sheng 消费源码；没有把其硬件常量或私有字段作为 cas 事实。新增代码采用 BSD-2-Clause-Patent，提取的 XNU 代码在本地探针输出中保留 Apple 原始声明。
