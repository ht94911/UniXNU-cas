# GPU024 B 包接入记录

本轮底包为第197条指定的 GPU024/source `673cde8`。板级源码位于独立分支 `codex/baseband27-gpu024`，实现提交 `699982a`；本轮只做电脑端源码、宿主检查和本地索引镜像。原生编译、系统接口适配、启动封包和真机验收由后续会话继续。

`draft.json` 记录当前源码选择。共享输入身份见 `owners.json`，其修改只应用于生成树副本。源码交付的历史说明见 `../SOURCE.md`。

## 1. 固定人类指定的包

- 记录以下身份：
  - 指定包的完整源码提交和各依赖提交；
  - kernel/payload/boot 的 SHA，真实 donor（24A446 iPhone14,4）；
  - 编译记录；
  - 已验证的 GPU/触摸/背光/USB/Wi-Fi/音频/电源、四入口菜单，以及原 Data 与 seed 身份。
- 在独立副本上应用本分支的提交，原包保持不动。
- 复用该包现有的 Format 2、package.sh、归档、签名和日志流程，不另建构建或 USB 系统。
- 原 17 文件、主包指针、只读 27 根、安卓/root、分区、已有持久 Data 都保持原授权边界。

## 2. 共有资源合同（`nezha_bb_bind` 必须绑定到包内已有的拥有者）

| 回调 | 合同 |
| --- | --- |
| `pas`（NbPasOps） | 在 NezhaScm 的 PAS 入口（audio27 `nezha-scm-audio-pas.patch` 的形式）上增加 PAS 4、0x26、15，复用同一把全局 SCM/WAITQ 锁，不新建 SMC 实现。命令仅限 1/2/5/6。 |
| `assign`（NbMssOps） | DSM 0x85000000/0x4f00000 和 0x89f00000/0x100000 从 HLOS 分配到 MSS_MSA（hyp assign_mem），经同一个 SCM 拥有者。只有 PAS 证实停止后才撤回。参数表放在低于 4 GiB 的物理连续页中。小米 17 Pro Max 移植（ios-popsicle a4ef4ed）的实测：调用可能要先给这一页建 SHM bridge（qcom_tzmem 的做法，SCM 0xc/0x1e），再调用 assign（0xc/0x16）。指定包若已有 tzmem 拥有者，就复用它。 |
| `smem_lock/unlock` | 复用现有 SMEM hwlock 3；临界区内不等待远端。 |
| `doorbell`、IPCC 分发 | 复用唯一的 IPCC 拥有者：只发送 modem 客户端 2 的信号 0 和 2；入站信号 2 交给 `nb_baseband_smp2p_event`；不重复读清全局 RX FIFO。 |
| `proxy` | xo、cx.lvl 384 与 mss.lvl 320（原厂 DT 的 mx-supply 指向 rpmh msslvl 轨，不是 mx.lvl；数值来自 cx-uV-uA/mx-uV-uA）、rproc_ddr/crypto_ddr 带宽，以及 QMP `load_state modem on`。用共有的 QMP/RSC 租约，不抢 Wi-Fi/音频/GPU 的投票。 |
| `ipa_power` | IPA core_clk（原厂 DT 是 RPMh 时钟 <0x34 0x0c>）和三条 ICC 路径 ipa_to_llcc、llcc_to_ebi1、appss_to_ipa 的投票（DT 的 5 档 bus-vector：MIN/SVS2/SVS/NOMINAL/TURBO，第一阶段取 SVS），走共有租约。 |
| `ipa_smmu` | apps SMMU（0x15000000）的板级保留 bank 8、ASID 0xfd00、slot 13/14，SID 0x4a0（AP）和 0x4a2（uC）；实际安装前仍由公共拥有者核对占用与冲突，流程同 Wi-Fi/audio 的 ArmSmmu4K 流程。IOVA 只在 0x20000000–0x3fffffff；另有 0x14683000/0x2000 恒等映射。原厂为 `dma-coherent`：映射用可缓存/外共享属性，或改用非缓存 CPU 映射，两种都要核实。不做全局 reset，不用直通模式。 |
| `ipa_memory` | zero 0x2000，ring 64 KiB（64 KiB 对齐），LAN_RX/MODEM_RX/MODEM_TX 缓冲；物理连续，IOVA 在上述窗口内，互不重叠。 |
| `rmtfs` | 4 个 RAM 映像，初始内容来自第 6 节的固件文件。共享缓冲按原厂 `qcom,rmtfs_sharedmem`：4 MiB 动态物理连续内存，client 1，经同一个 SCM 拥有者 hyp assign 给 HLOS 和 MSS_MSA（都是读写），地址在 ALLOC_BUFF 回应里交给 modem。永不写回。 |
| `tftp`（NbTftpStore） | 只读查找/读取固件文件里的 `modem_pr`、`modem_oem` 等树；写请求只进 RAM 池。 |
| PAS 元数据缓冲 | 物理连续、页对齐，**整段低于 4 GiB**（原厂 canoe_mpss_resource 的 dma_phys_below_32b；`nb_pas_begin` 会检查）。 |
| PAS 读取端（QcomMdtRead） | 从固件文件读取 `modem.mdt`/bNN、`modem_dtb.*`、`ipa_fws.*`，经 ShengUfs 现有的钉住文件 extent 读取接口，只读。 |

如果指定包没有某个拥有者，先补共有 SoC 接口并审计。不能用返回 0 的桩函数代替，也不能让两个子系统各自操作同一控制器。

**共用传输源码**：audio、power、baseband 的 `QcomGlink/Smem/Smp2p` 在合并包里只能编译一份（同名符号）。用本分支 `upstream/` 的版本：它是 sheng + audio 的修改 + power 的 SMP2P v1/v2 + intent ID 0。与 power27 的另一份差异，到时逐条合并后重新跑三方的宿主测试。

## 3. 源码与构建

已实现 `platform/NbBoard.cpp` 的 `nezha_bb_bind` 和 `integration/NbBasebandStart.c` 的 late initializer。共享控制器由现有 `Sm8850Resources`、`NezhaScm`、`ArmSmmu4K` 和 ShengUfs 拥有。

- `prepare-nezha27-baseband.py --tree .` 在生成树中复制所选包、共享 SoC、ShengUfs 和标准 ADT 模块，核对固定输入后应用补丁。原源码目录保留；输入变化时拒绝静默复用旧副本。
- 包补丁按 `integration/series` 应用；SoC 使用 `soc-stream-count.patch`。这些是组件/包源码补丁，不放入只接受 XNU 路径的 kernel patch series。
- `format2-baseband-node.patch` 让原有 typed-node 生成器导出已配置的顶层节点；复用原编码与验证逻辑。
- `build-nezha27-shared.py` 编译六份 Qcom 共享传输源和两份 SoC 资源源，仅一份。
- `build-nezha27-baseband.py` 编译十份 platform 源、QRTR、四份服务源、三个 IPA 翻译单元、启动/诊断入口和原桥接翻译单元。被 `#include` 的 IPA 源不重复编译。
- ArmSmmu4K 继续由 GPU024 的 Adreno840 归档提供；B 归档只引用它。
- C/C++ 分别复用指定 XNU 的 `vfs_conf.o.json` 和 `IOPlatformExpert.cpo.json`，沿用现有 `compile_command`、`kernel_target` 与 `nezha_macho_late`，加 `-mgeneral-regs-only -Wall -Wextra`。
- 桥接类的构造器移入现有 late 注册路径。唯一开关通过包补丁统一；桥接协议与系统接口进一步适配不在本轮。
- `board.toml` 已按拥有者顺序接入配方，defconfig 已加入两份归档；原启动参数和严格预链接根保持相同。

本轮已执行的入口：

```sh
sh tests/baseband27/check.sh ../research/upstream/QcomBaseband
```

宿主工具可通过 `BB_SOC_SOURCE`、`BB_UFS_SOURCE`、`BB_SMMU_SOURCE`、`BB_FRAMEWORK_SOURCE`、`BB_LINUX_SOURCE` 指定固定本地参考。它们不进行原生构建或手机操作。

后续原生构建仍须核对对象、构造器、符号和严格预链接；宿主测试不代替这一步。

## 4. 开关、日志与回退

- 开关只有 ADT 的 `/chosen/nezha-baseband = 1`。启动参数行已用 994/1000 字节，**不新增启动参数**。
- 开关关闭时：
  - 不调用 bind 和 worker，不发布 AppleBaseband*；
  - 策略补丁不起作用；
  - 内核只多出不执行的代码。
- 回退：用原包 boot/payload，或者在副本中去掉 ADT 开关重新封包。B 包本身不写任何手机文件，回退不需要清理。
- 日志：
  - `NEZHA27 BB stage=... result=...`（阶段变化时打印一行）；
  - 首次共享传输初始化、接收端接入、首次 poll 均有前后标记；
  - 桥自带的每 10 秒报告；
  - `nb_ipa_report`（端点计数）；
  - QMI 头追踪最多 6000 行。
  - 使用原 USB 诊断框架的只读 provider 8（`nezha-mss`），提供 `nezhaBasebandStatus` 和有界计数快照；与 GPU024 的 provider 7 分开。
  - 日志里的 IMSI/ICCID/IMEI/号码只在本机脱敏保存。

## 5. 用户态与信任

- 第一阶段不加入新的用户态二进制，也不加入 addon。Apple 的 Mav21 bbfw 已在 27 根卷 `/usr/standalone/update/Mav21.Release.bbfw`，不需要另外打包。
- launchd：
  - 打补丁后，`sheng-lite`/`sheng-disable-jobs` 不再禁用 `com.apple.CommCenter`；
  - 开关打开时每次开机重写 disabled.plist，避免持久 Data 里沿用旧文件。
- CommCenter 的推迟票据条件：27 二进制（SHA aa96ad91…，静态）仍有 `/cc/props/is_postponement_ticket_available`。首个数据测试先看日志是否在这里卡住；确认卡住后，再按 QcomBaseband `coachz_cellular_compat.py` 的方式，针对 24A446 的 UUID 和地址做兼容副本，加入包的 owner trust 流程，并把只读 OS 覆盖方式先交人类确认。不预先修改。
- 27 的 PDP 是 Skywalk（AppleBasebandPCIMAVPDP），桥用的是 BSD ifnet `pdp_ipN`。如果 CommCenter 不接受，下一份候选改为提供 PDPQMAPSkywalk 所需的接口。

## 6. 本地索引镜像（第197条决定已给出）

modem 运行需要以下内容：

| 内容 | 大小 |
| --- | --- |
| `modem.mdt` + b00–b27 | 约 67 MB |
| `modem_dtb.*` | 0.1 MB |
| `ipa_fws.*` | 约 0.05 MB |
| `modem_pr` | 37 MB |
| `modem_oem` | 23 MB |
| `qdsp6_oemsw.mbn` | 21 MB |
| `qdsp6m.qdb` | 8 MB |
| EFS 初始内容（modemst1/2、fsg 各 8 MiB，fsc 128 KiB） | 约 24 MiB |
| 合计 | 约 180 MB |

本轮采用独立索引归档：

- 新建只读钉住文件 `/data/unixnu/ios27/baseband.img`，约 208 MiB。内容为带索引的归档（文件名、偏移、长度、SHA-256）。
- 写入只用 pinfile rawwrite，并核对与六个现有文件的 extent 完全不重叠。
- XNU 只按 ADT 里的 extent 读取，**从不写入**。
- EFS 是本机自己的身份与校准数据，放回本机文件里。它不离开手机，也不进 Git、不上传。modem 的 EFS 写入只留在 RAM。

本轮已在 Git 外生成 208 MiB、616 个条目的本地镜像，完整 SHA、索引和三份输入布局检查通过。具体路径及摘要保存在本地研究目录。未安装到手机。

归档由 `tools/baseband27/pack-image.py --input <规范化目录> --output <Git外的新路径> --size-mib 208` 生成，同时写 `.sha256`。输入目录中固件文件使用原相对名称；四份 RAM 初始文件位于 `ram/`。工具拒绝符号链接、覆盖已有输出以及在 Git 工作树内输出。

`tools/baseband27/check-image.py <本地镜像> --sha256 <摘要>` 使用同一组 C 读取/解析源作电脑端检查。它不执行安全服务调用。

XNU 的三项 ADT 输入为 `nezha-baseband-extents`、`nezha-baseband-blocks`、`nezha-baseband-sha256`。extent 复用 ShengUfs 的 4 KiB 分区相对三元组，按实际 GPT 和其他受保护映射核对。后续手机安装步骤由接续会话按其适用授权处理。

## 7. 启动参数与容量

- 启动参数行仍为 994/1000，不变。
- ADT 增加约 200 字节（开关、/baseband、chipset，固件文件获批后再加 extent）。
- 内核增加的部分：
  - 代码约 9 千行源码；
  - 运行缓冲约 300 KiB（QRTR、TFTP、intent、发送队列）；
  - TFTP RAM 池 2 MiB；
  - rmtfs RAM 映像约 24 MiB；
  - IPA DMA 缓冲约 4–8 MiB。

  需要核实指定包的 DRAM 预留和 vnode 预算是否充足。

## 8. 检查顺序

1. 宿主模型（`check.sh`），开 ASan/UBSan。
2. 原生对象编译；检查符号、类、构造器；严格预链接；签名和信任；离线 QEMU 只验证"开关关闭时与原包行为一致"。
3. 真机需要适用的手机窗口，按当时最新的接线、设备身份、文件映射/GPT/启动链规则做前后核对；不抢别人的窗口，固定复位保持 0。

## 9. 真机验收顺序（同一次开机尽量集中取完整时间线）

1. 依次看：PAS 15/4/0x26 返回、SMP2P 版本与 ready、GLINK 版本、IPCRTR/DS 打开、crash reason。
2. QRTR 服务上线；rmtfs、TFTP、servloc、memshare 的请求计数；IPA 时钟查询；INIT_DRIVER/INIT_COMPLETE。
3. AppleBaseband 发布、ABM 引导（SAHARA）、bootstage、QMUX 客户端。
4. SIM 状态（如需 PIN 就停止，并说明原因）、ICCID/IMSI 激活 TLV、运营商匹配。
5. NAS 注册：制式（LTE/NR、NSA/SA）、信号。
6. WDS 会话、`pdp_ip0` 地址/DNS/MTU、TX/RX 计数、DNS 回包、TCP/TLS/HTTP、Safari。
7. 断开/重连、飞行模式、重复开机（冷启动后不插拔卡直接联网），以及与 GPU/Wi-Fi/音频的共存。
   同时记录 CommCenter 的崩溃日志、"not provisioned" 和 AttachAPN 重置（kEX_NULL_BEARER）日志：这三处在其他机型上出现过。

保留诊断版本、升级B 包基线、合入主包，三件事分别决定。B 包关闭时回退使用原包。
