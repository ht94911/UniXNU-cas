# nezha iOS 27 基带与蜂窝数据：源码交付与静态审查

任务：`docs/handoffs/IOS27-BASEBAND-DATA-VOICE-CODEX-20261107.md`。分支 `codex/baseband27-20261107`（基于 `codex/main-ios27@675500a`，仅本地，push 已禁用）。
本文是构建前交付。**人类指定测试包之前，不集成、不编译、不跑 CI、不封包、不上机。**

## 1. 固定来源

| 来源 | 版本 | 许可 | 用到的部分 |
| --- | --- | --- | --- |
| UniXNU/QcomBaseband | `dd0199c`（components.toml 固定的版本；2026-11-09 fetch 后无新提交） | BSD-2-Clause（HoshinoYumeka） | `baseband/*`、`patches/`、`ipa/*`、`platform/coachz/coachz_modem.c`、`tests/coachz_bb_model_test.c` |
| UniXNU-sheng | `4601795`（经 audio27 `abde8b4` 拷贝） | BSD-2-Clause-Patent | `QcomGlink/Smem/Smp2p/Qmp/Mdt/Scm` |
| power27-codex | `26254a8`、`e8d31f7` | BSD-2-Clause-Patent | SMP2P v1/v2 协商；GLINK 允许远端 intent ID 0（SM8850 真机已验证） |
| Linux 主线 `drivers/net/ipa` | v6.12 系列 | GPL-2.0（只参照取值，不拷代码） | v5.x 的寄存器字段、status 布局、表初始化顺序 |
| OnePlus SM8850 下游 `dataipa`/`datarmnet` | `ref/oneplus-sm8850-modules` | GPL-2.0（只参照取值） | IPA_5_5 端点和通道表、SMP2P 时钟投票、QMI INIT_DRIVER 字段 |
| tqftpserv / rmtfs（linux-msm） | 上游 | BSD-3-Clause（只参照语义） | TFTP 选项、会话、rmtfs IOVEC 语义 |
| OctoDevStudios/ios-popsicle（小米 17 Pro Max，SM8850，iOS 16） | `a4ef4ed`（2026-11-06） | 未声明许可，只读参照，不拷贝 | 同 SoC 在 XNU 下启动 modem 的实测：SMP2P item 方向、PAS 元数据低于 4 GiB、DSM assign 时的 SHM bridge、rmtfs 4 MiB 共享缓冲分配给 HLOS+MSS、TFTP 前缀和 servloc root_pd 180。它直接读安卓分区，与本项目只读钉住文件的边界不同，这部分不采用 |
| 本机安卓原厂（只读采集，2026-11-07） | 仅存放在本地 `research/phone-readonly-20261107` | 厂商私有，不进 Git | 固件版本和布局、DT、模块、运行状态、QRTR 服务表 |
| Apple 24A446 iPhone14,4 | 本地 IPSW 和解密卷 | Apple 私有，不进 Git | kernelcache kext、dyld cache、CommCenter、ADT、Mav21 bbfw |

上游原样拷贝放在 `*/upstream/`，各目录的 `SOURCE.txt` 里记录 SHA-256。27 派生文件全部由生成器做"计数式精确替换"得到：`tools/baseband27/derive-ipa55.py`、`derive-bridge27.py`、`derive-bridge27-test.py`、`gen-nb-ipa-v55.py`。原样拷贝不手改。

## 2. 硬件、固件与协议身份（安卓只读实测，另有标注的除外）

- SoC：SM8850（`canoe`，soc_id 660）；modem 是集成的 MSS `qcom,canoe-modem-pas`，不走 PCIe/MHI。
- 固件：`MPSS.DE.9.0-e64d0ee8fe`。modem 映像 PAS 4（`modem.mdt` 加 b00–b27，约 67 MB）；modem_dtb PAS 0x26；`ipa_fws` PAS 15（由 AP 加载，日志为 "xbl loading No"）；GSI 固件 v41。
- 内存：mpss 0x8aa00000/0xe100000，dtb 0x99500000/0x80000，DSM 0x85000000/0x4f00000 和 0x89f00000/0x100000（分配给 MSS_MSA）。SMEM 0x81d00000/0x200000，hwlock 3。
- 门铃：IPCC 0x1106000，modem 客户端 2；信号 0 是 GLINK，信号 2 是 SMP2P。SMP2P `qcom,smem = <435 428>`：入站 435、出站 428（按 smp2p.ko probe 的存放位置和两处使用确认；ADSP 边 audio27 也是这个规则），条目为 master-kernel、ipa、smem-mailbox。GLINK SMEM item 478/479/480。
- **SMP2P 版本**：原厂 `smp2p.ko` 的 `qcom_smp2p_alloc_outbound_item`（已反汇编）发布版本 1；如果远端 item 已经存在且版本更高，就用远端版本。之后等双方版本一致，不改写自己的版本。SOCCP 边在真机上是 v2（power009）。ios-popsicle 只支持 v1 的 SMP2P，在同 SoC 上 modem 仍能到 ready 并启动服务，说明 modem 边按 v1 协商；本实现在冷启动时也发布 v1。
- QRTR：AP 是节点 1，modem 是节点 0；实测 64 个 modem 服务（`qrtr-services.txt`）。AP 需要提供 rmtfs 14、memshare 52、servloc 64、TFTP 4096、IPA 0x31。
- IPA v5.5（hw_type 24），GSI v12，EE 0。AP SMMU CB 是 SID 0x4a0（uC 是 0x4a2）；可用 IOVA 窗口 0x20000000/0x20000000；`dma-coherent`；额外恒等映射 0x14683000/0x2000。
  - 端点按下游 IPA_5_5 表，**不是**主线 `ipa_data-v5.5.c`：CMD ep14/ch12，LAN_RX ep17/ch14，MODEM_TX ep2/ch11，MODEM_RX ep24/ch1。
  - 实测开启哈希（`ipa_fltrt_not_hashable` 打印为 False，DT 也没有 not-hashable 属性），所以 INIT_DRIVER 带哈希表 TLV 0x1b–0x1e，共 189 字节。
  - SMEM_IPA_FILTER_TABLE 是 item 497，大小 0x19000，位于 modem 分区。
- EFS：modemst1/modemst2/fsg 各 8 MiB，fsc 128 KiB。这是设备身份数据，**只留本地**。
- Apple 侧（24A446 静态分析）：
  - 原版 ADT `/baseband`：compatible `baseband,n41`，class 3；`/product baseband-chipset = mav21`。根 compatible D16AP 属于有基带的硬件型号。
  - **当前已部署的 27 ADT（beb99e3c…，persist001–020 共用）没有 `/baseband`，也没有 `baseband-chipset`。**
  - BBUEUR20Loader 的 PK hash 为 48 字节，默认值 `d40eee56…15ac87`；ChipId 0x001440E1。
  - **27 根卷自带 `/usr/standalone/update/Mav21.Release.bbfw`**（96967983 字节，与 IPSW 中的同名文件同长），所以不用像 Coachz 那样另外打包 Apple 固件。
  - AppleBasebandPCIControl 用 `setProperty("bootstage", v, 32)`；libTelephonyBasebandDynamic 把值小于 2 视为下线。
  - libIOABP 的 channel/control selector 与 21H461 相同。

## 2.1 同硬件与公开实现检索（2026-11-09）

| 范围 | 结果 |
| --- | --- |
| UniXNU 官方设备列表与指南 | 基带只有 QcomBaseband/Coachz（SC7180，iPadOS 17）。指南 docs/porting/baseband.md 已逐段读过 |
| GitHub 关键词：hackios、HackiOS、UniXNU、QcomBaseband、sm8850 xnu/ios、kaanapali modem、xnu qrtr/ipa | 唯一的基带桥是 UniXNU/QcomBaseband。在 SM8850 上，ios-popsicle 已让 modem 带服务完整启动，但"未接入 iOS"；data-a-live（17 Pro）、pudding、onelineage、k90 的蜂窝都未开始。hackios 系列仓库（K20 Pro、dada、dodge 等）没有蜂窝实现 |
| Linux 主线与下游 | 主线 ipa v5.x 和 qcom_q6v5_pas；下游 dataipa/datarmnet 与 OnePlus SM8850 内核（canoe_mpss_resource）已对照 |
| QQ 历史 | 187 个关键词，1122 条精确命中，日期 10-22 至 11-06。线索见本地 research/QQ-LEADS.md（不进 Git） |

## 3. 复用表

| 阶段 | 硬件/协议 | 来源 | 已有验证 | 复用范围 | 27/nezha 适配 | 缺口 | 验证方法 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| modem 启动 | PAS/SCM、MSS、DSM 分配 | sheng QcomMdt；audio27 NezhaScm PAS 补丁形式 | audio ADSP PAS 真机 | MDT 解析、PAS 命令序列 | `NbPas`（有界、每步 64 KiB）、`NbMss`（ELF32 映像，proxy/assign 回调） | DSM 的 hyp assign_mem 调用尚未接 NezhaScm | 上机看 PAS 返回、SMP2P ready 和 crash-reason item 421 |
| 控制传输 | SMEM/SMP2P/GLINK/IPCC | sheng + audio27 + power27 | SOCCP 与 ADSP 真机 | 全部 | modem item 号；SMP2P 版本选择；intent ID 0 | 无 | 宿主模型；上机看 GLINK 版本和 IPCRTR 打开 |
| QRTR/服务 | QRTR、rmtfs、servloc、TFTP、memshare、IPA QMI | 新写（参照 Linux 语义） | 无 | — | 全部本机化；rmtfs 只在内存中保存 | memshare 是否必需待上机确认 | 上机记录服务上线顺序和每类请求次数 |
| 数据面 | IPA v5.5/GSI v12、QMAP | QcomBaseband v4.2 实现，派生到 v5.5 | Coachz v4.2 真机 IPv4 | 命令、RX、GSI、QMI 结构 | 寄存器生成器、Qtime 聚合、8 位端点、v5 status、哈希表、端点 2/24 | IPA 的 SMMU CB 和 IOVA 由包内提供者负责；时钟/ICC 投票未接 | 宿主模型；上机看 INIT_DRIVER/INIT_COMPLETE、TX/RX 计数 |
| iOS 桥 | ABM/BBUpdater/CommCenter、SAHARA、QMUX、PDP | QcomBaseband baseband/ | Coachz 21H461 真机 | 全部 | Mav21 ChipId、48 字节 hash、端点、纯 QMAP、bootstage | 27 PDP 是 Skywalk，而桥用 BSD ifnet `pdp_ip` | 上游宿主模型（派生版）；上机看 ABM 引导、QMUX 客户端、SIM、注册 |
| 系统任务 | launchd 策略 | 设备树 BootWatchdogPolicy | — | — | 在 ADT 开关打开时保留 CommCenter（集成补丁） | — | 上机看 CommCenter 进程 |
| IMS/通话 | Apple CoreIMS/IPTelephony；modem 语音 | 第 8 节 | — | — | 设计阶段 | 依赖数据通路、ISIM、音频 | 后续阶段 |

## 4. 原厂逆向对照（摘要）

| 原厂函数/消息 | 输入/输出 | 状态 | 27 对应 |
| --- | --- | --- | --- |
| `qcom_smp2p_alloc_outbound_item` | 版本 max(1, 远端版本)、features 1 | 出站 item 发布 | `NbMss.c setup` |
| `qcom_smp2p_intr`（negotiate） | 版本一致才合并 features；v2 SSR 时把入站 last_value 清零 | 协商完成 | power27 的 `QcomSmp2p.c` |
| `ipa3_freeze_clock_vote_and_notify_modem` | 入站 "ipa" 时钟查询，回应出站 bit0（响应）和 bit1（投票） | modem 等待 IPA 时钟 | `nb_ipa_clock_query` |
| `ipa3_qmi_init_modem_send_sync_msg` | 带哈希 TLV，189 字节 | INIT_DRIVER | `ci_init_driver_req` |
| `ipa_smem_init`（主线） | item 497/0x19000，在 modem 分区 | 过滤表 | `allot_smem` |
| GLINK intent（SOCCP 实测） | ID 可以为 0 | intent 表 | `QcomGlink.c` |
| BBUEUR20Loader | PK hash 长 0x30 | SAHARA 读寄存器 4 | `cbs_default_pk_hash` |
| AppleBasebandPCIControl `bootstage` | SInt32，值 3 表示运行 | 上线/下线 | `coachz_baseband.cpp` |

## 5. 已实现与待实现

已实现（提交 1911770 起至本文所在提交）：
- QRTR 路由；
- QMI 编解码；
- rmtfs（内存）；
- servloc、TFTP、memshare；
- PAS 加载器与 MSS 生命周期；
- IPA v5.5 全部派生源；
- IPA 板层；
- baseband 工作线程；
- 27 桥派生与胶水；
- SMP2P v1/v2 与 intent ID 0；
- modem SMP2P item 方向修正为入 435/出 428，PAS 元数据限制在 4 GiB 以下（均已按原厂确认）。

待实现，需等指定测试包，依赖包内提供者：
- 启动 caller（`integration/NbBasebandStart.c` 草案见集成文档）；
- SCM PAS/assign_mem；
- IPCC 中断分发；
- IPA 时钟/ICC 投票；
- IPA SMMU 租约；
- 固件读取端；
- IOKit 类初始化；
- launchd 策略补丁；
- ADT 节点。

## 6. 生命周期与错误处理

- 只有一个工作线程，轮询有界。顺序：IPA 电源 → SMMU → ipa_config → ipa_fws（PAS 15）→ ipa_setup → modem PAS/启动 → 服务。任何一步失败，本次开机就停止基带，并打印阶段和错误码；系统其他部分不依赖它。
- modem 崩溃（SMP2P fatal 或看门狗中断）时：桥上 bootstage=0，`coachz_baseband_down`，IPA 进入 down。第一阶段不重启 modem，IPA 端点和 DMA 内存保留到下次开机，不做没有证据的释放。
- PAS shutdown 返回 -EINVAL 时，按 Linux 解密关机的规则重试（50 次，间隔 100 ms）。只有 PAS 证实停止后，才撤回 DSM 分配。
- 所有服务输入都做长度和 TLV 边界检查；未知请求不回应（与 rmtfs 一致）；TFTP 会话有超时和数量上限；QRTR 发送队列有界（32×8 KiB），满了就计数丢弃，不阻塞。
- 桥：ABM 每个通道最多缓存 1024 帧，前 32 次丢帧记录头部；QMUX 客户端在 DEL_CLIENT 或 modem 下线时作废。
- EFS 写入只留在内存，**从不写回**任何分区或文件。

## 7. 测试输入与用例（宿主，`sh tests/baseband27/check.sh <QcomBaseband>`）

| 文件 | 覆盖 |
| --- | --- |
| `ipa55_values.c` | 端点寄存器写序列、mem_setup 4 条和 table_setup 8 条命令、过滤位图 0x94e6、INIT_DRIVER 189 字节 |
| `ipa55_model_test.c` | 纯 QMAP 出站/拆包（含 pad、命令帧、截断、未知 mux、只有 pad 的帧）、v5 status（8 位 src/dst、tag、miss、异常、越界）、TRE 预算与门铃 |
| `bridge27_values.c` | ChipId、48 字节 hash、DPM 端点 2/24、WDA 只发一次纯 QMAP |
| `coachz_bb_model_test27.c` | 上游 836 行桥模型（QMUX/CTL、SAHARA、方法、通道、数据口与激活、eSIM），只改 27 的预期值 |
| `transport_model_test.c` | GLINK intent ID 0 收发与 RX_DONE、零大小判为错误；SMP2P v1、预先存在的 v2、v2 SSR、版本 3 拒绝 |

2026-11-09 全部通过（gcc，ASan/UBSan）。上机用例见集成文档第 9 节。

## 8. IMS/语音设计（后续阶段）

- 27 自带 Apple 的 AP 侧 IMS（CoreIMS.framework、IPTelephony.framework；CommCenter 负责 P-CSCF 解析，区分蜂窝和 Wi-Fi 两种 IMS 传输）。首选路线 A：AP 侧 IMS 跑在 modem 提供的 IMS PDN 上（WDS 走 IMS APN，绑定第二个 QMAP mux，即 `pdp_ip1`）。鉴权用 ISIM/USIM AKA，经 UIM 桥转发 APDU；媒体和音频走 AP 侧，依赖 audio27 的麦克风/听筒/扬声器通路。
- 备选路线 B：modem 侧 IMS，同原厂安卓。语音 PCM 经 modem ↔ ADSP 语音图，需要 audio27 提供语音 DAI/图，并把 BasebandVoice.driver 的通道映射过去，工作量更大。
- 依赖：
  - 数据通路稳定，IPv4/IPv6 双栈；
  - 运营商捆绑包里的 IMS 配置；
  - UIM 的 ISIM 应用和逻辑通道；
  - audio27 提供的已验证播放/录音；
  - 人类指定的测试号码。
- 禁止拨打紧急号码，紧急能力按实际实现上报；不猜 PIN/PUK。

## 9. 验收标准与阻塞

数据阶段（按顺序、分别取证）：
1. modem 运行；
2. QRTR 服务上线；
3. IPA INIT_COMPLETE；
4. ABM 引导完成；
5. QMUX 客户端；
6. SIM 状态和 ICCID 读取（只本地记录）；
7. NAS 注册在 LTE/NR，含 NSA 锚点和 SA 状态；
8. WDS 会话、地址/DNS/MTU；
9. `pdp_ipN` 收发计数；
10. DNS 回包、TCP/TLS/HTTP，以及系统浏览器真实访问；
11. 断开/重连、飞行模式、重复开机。

5G 要另外拿出 NR 承载与同一次数据流关联的证据，状态栏图标不算数。

阻塞：
1. 固件和配置树的交付方式需要人类决定，见集成文档第 6 节（新钉住文件方案）。
2. 27 PDP 是 Skywalk，桥用 BSD ifnet，CommCenter 是否接受要上机验证；备选是适配 PDPQMAPSkywalk 的接口。
3. CommCenter 的推迟票据（postponement ticket）条件：27 里仍有 `/cc/props/is_postponement_ticket_available`，可能需要 27 版兼容副本。
4. memshare 是否必需。
5. EUR20 的 SAHARA 映像 ID 清单。
6. IPA SMMU 和时钟投票在指定包里的提供者。
7. 测试用 SIM、运营商和测试号码由人类提供。
