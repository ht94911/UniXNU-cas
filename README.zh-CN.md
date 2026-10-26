> 当前开发已证明真实XNU CPU0入口、VM与早期平台初始化；以下冻结EFI里程碑保留不变。新格式2、单核连续启动及实际限制见[移植状态](docs/porting-status.md)、[当前构建](docs/build-format2.zh-CN.md)与`porting.toml`。iOS界面尚未验证。

<p align="center"><img src="docs/assets/cas-banner.svg" alt="小米 10 Ultra — cas UEFI 与 UniXNU 社区适配" width="100%"></p>

<p align="center"><a href="README.md">English</a> · <strong>简体中文</strong></p>
<p align="center"><a href="docs/build.zh-CN.md">构建</a> · <a href="docs/operations.zh-CN.md">启动与操作</a> · <a href="docs/validation.zh-CN.md">验收记录</a> · <a href="https://github.com/ht94911/UniXNU-cas/releases">版本发布</a> · <a href="docs/architecture.zh-CN.md">架构与后续方向</a></p>

# 小米 10 Ultra 的 UniXNU 适配

面向 **小米 10 至尊纪念版 / Xiaomi 10 Ultra（`cas`、M2007J1SC、SM8250）** 的社区适配。本仓库是项目的统一入口，保存设备配置、受校验的构建工具、文档，并承接未来设备专属的系统移植工作。配套的 [uefi-cas](https://github.com/ht94911/uefi-cas) 保存 Mu-Silicium 固件适配源码。

**当前里程碑：** 已在测试手机上运行 UEFI 和真实 UniXNU 菜单；实体按键可操作；测试 EFI 程序可以从只读 RAM FAT 加载、执行并返回菜单，随后通过菜单回到原有 Android。**同一候选镜像完成三轮独立完整验证，34 项产物完成逐字节一致重建。**

DarwinBoot 交接、XNU 和 iOS **尚未测试**。本次发布对应早期真机适配里程碑。

## 已验证能力

| 功能 | 记录中的结果 |
| --- | --- |
| USB Fastboot 临时启动 | 已在测试 cas 与固件组合验证 |
| 显示 | 保留 ABL framebuffer，1080 × 2340 输出 |
| 实体按键 | 音量键导航，短按电源确认 |
| UniXNU 启动菜单 | 四项菜单、导航和错误返回 |
| 只读 RAM FAT | 16 MiB 卷，包含配置和 CasSmoke EFI |
| EFI 运行与返回 | 显示 `cas-smoke-001` 标记，返回后菜单仍可操作 |
| 缺失 EFI 文件 | 菜单底部状态行显示 `Not Found` |
| 回 Android | 经 UniXNU 或基础固件菜单退出，核对 Android 启动完成 |
| 重复验证 | 同一镜像三轮独立完整运行 |
| 可复现重建 | 在已记录工具链中，34 个输出逐字节一致 |

触控、GPU 加速、内部 UFS 文件系统、USB 键盘、充电管理及 UEFI 内的 ADB 不在本次里程碑范围。其他手机、内存版本和固件组合尚未验证。详见[完整验收记录与证据边界](docs/validation.zh-CN.md)。

## 从这里开始

| 目的 | 入口 |
| --- | --- |
| 了解已完成的结果及限制 | [验收记录](docs/validation.zh-CN.md) |
| 构建冻结候选 | [构建与可复现说明](docs/build.zh-CN.md) |
| 使用菜单、退出或恢复 Android | [启动、按键与恢复](docs/operations.zh-CN.md) |
| 审阅固件实现 | [uefi-cas](https://github.com/ht94911/uefi-cas) |
| 继续向 XNU / 用户空间移植 | [架构与后续方向](docs/architecture.zh-CN.md) |
| 报告问题或参与开发 | [贡献说明](CONTRIBUTING.zh-CN.md) |

[版本发布页](https://github.com/ht94911/UniXNU-cas/releases/tag/v0.1.0) 提供源码 bundle、从源码生成的 EFI／FD／FAT／shim、许可证、固定环境清单和脱敏验收摘要。完整 `boot.img` 依赖匹配的私有原厂 DTB，保留在本地，**公开下载不包含完整启动镜像**。所需输入见构建说明。

## 冻结里程碑

候选：**`cas-complete-001`** · 启动镜像大小：**17,616,896 字节**

```text
a085d19b6a4a1ea429f2f10cb191050f316cf044d4fd864b53d398e4f6a4f67c
```

| 源码 | 真机验收对应提交 |
| --- | --- |
| [设备配置和工具](https://github.com/ht94911/UniXNU-cas/tree/cas-complete-001) | `346a404f6cee464e3e95f6ded464caf804a1f89e` |
| [cas 固件](https://github.com/ht94911/uefi-cas/tree/cas-complete-001) | `893f5025cea91a121f2566730779cb34955325fb` |
| [UniXNU 上游](https://github.com/UniXNU/UniXNU/tree/67e8ef0aa2a0a1fcb940889fb39ed1752b4b1593) | `67e8ef0aa2a0a1fcb940889fb39ed1752b4b1593` |

`cas-complete-001` 标签保留实际验收的原始源码提交。`v0.1.0` 增加公开文档和发布支持文件，固件实现没有变化。冻结源码标签中的历史 README 早于最终验收，请通过默认分支文档查看已完成的里程碑。全部版本信息见 [source-pins.json](source-pins.json)。

## 一个设备项目，两个配套源码仓库

`UniXNU-cas` 是统一项目入口，`uefi-cas` 是它的固件源码依赖。UniXNU 社区列表按**一个 cas 设备适配**收录，设备页可以同时关联这两个仓库；sheng 和 pipa 也采用这种组织方式。

后续设备配置、内核配置、设备脚本、补丁、驱动和 package 可以继续在本仓库扩展；UEFI 修改继续在 `uefi-cas` 进行。进入下一个适配阶段不需要另建一个设备项目。

## 致谢与许可证

基于 [UniXNU](https://github.com/UniXNU/UniXNU)、[Project Silicium](https://github.com/Project-Silicium/Mu-Silicium) 和 [Microsoft Project Mu](https://microsoft.github.io/mu/)。已有 Qualcomm／Kona 和社区适配用于参考，它们的结果不作为 cas 真机证据。

cas 原创贡献采用 [BSD-2-Clause-Patent](LICENSE)，其他组件保留各自许可证。上游来源和发布许可说明见[来源与许可证](docs/sources.zh-CN.md)。
