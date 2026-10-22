# 架构与后续方向

[English](architecture.md) · [项目首页](../README.zh-CN.md)

## 已验证的启动路径

```text
USB Fastboot 临时启动
  → 原有 ABL / 设备树准备
  → cas BootShim：校验并有界复制 RAM 输入
  → Mu-Silicium SEC / DXE / cas 固件驱动
  → 从固件卷运行 UniXNU
  → 只读 RAM FAT → CasSmoke.efi → 返回 UniXNU
  → 菜单重启 → 原有 Android 系统
```

固件保留原引导器的显示缓冲，提供 GOP，通过 SPMI 读取实体按键，并发布位于 RAM 中的 FAT 文件系统。启动项指向固件卷内的固定版本 UniXNU。CasSmoke 输出可识别标记，再通过 EFI 应用接口返回。

本目标使用模拟变量和只读 RAM 存储，不包含物理存储控制器、物理变量后端、FTW 或固件更新器，Capsule 调用返回 `EFI_UNSUPPORTED`。测试序列前后的保护分区核对为这些实现选择提供额外的实测检查。

## 源码职责

| 仓库 | 当前与后续职责 |
| --- | --- |
| [UniXNU-cas](https://github.com/ht94911/UniXNU-cas) | 项目入口、设备配置、构建／打包检查、脱敏验收摘要；后续 cas 内核配置、脚本、补丁、驱动和 package 模块 |
| [uefi-cas](https://github.com/ht94911/uefi-cas) | 固件启动、BootShim、GOP、按键、RAM Block I/O、EFI 测试程序和板级库 |
| [UniXNU 上游](https://github.com/UniXNU/UniXNU) | 通用启动菜单、DarwinBoot、构建驱动和共享系统组件 |

一个 cas 设备页面可以关联两个自有源码仓库。这与[上游仓库职责划分](https://github.com/UniXNU/UniXNU/blob/main/CONTRIBUTING.md)及 [pipa 设备页](https://unixnu.com/devices/pipa)一致。后续通用修复适合评估后贡献上游，cas 专属组件随设备仓库维护。

当前 `board.toml` 可以被固定上游的 Context 读取；构建这个里程碑仍需专门的 cas 校验入口。通用 `unixnu build/bootimg` 路径没有完整实现 cas 的来源与 BootShim 检查，构建手册使用的是已支持入口。

## 后续系统移植

以下是可继续推进的方向，**尚未完成，也不构成版本承诺**：

| 阶段 | 需要建立的结果 | 可扩展的位置 |
| --- | --- | --- |
| 内核准备 | 选择兼容的 XNU 输入、配置和必要板级描述 | 设备配置、`patches/`、`scripts/`、`package/` |
| 内核入口 | DarwinBoot 交接、可观察的早期执行和有依据的内存使用 | 设备配置，以及必要的固件修改 |
| 存储与根文件系统 | 已验证的存储通路、根设备发现和文件系统挂载 | 设备驱动和内核配置 |
| 用户空间 | 可复现系统输入，以及实际观察到的用户空间启动 | 设备 package 和脚本 |
| 硬件支持 | 显示、触控、GPU、电源等驱动的独立验收 | 设备驱动，通用修复提交上游 |

已有 UEFI 里程碑在后续工作中保留为可用基线。任何新候选字节都使用新编号并完成对应验证，旧真机结果始终绑定原 SHA-256。公开代码不包含专有系统镜像、原厂固件、设备身份或恢复备份。
