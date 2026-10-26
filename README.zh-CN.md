# 小米 10 Ultra（cas）的 UniXNU 适配

[English](README.md)

本仓库是小米 10 Ultra（`cas`，SM8250）通过 [UniXNU](https://github.com/UniXNU/UniXNU) 运行 iPadOS 17.7.11 的板级配置。内核为 XNU 10063.141.1，4 KiB 页；donor 为 iPad7,1 / 17.7.11 / 21H461（j120ap）。

## 状态

| 部分 | 状态 |
| --- | --- |
| UEFI 与 UniXNU 菜单 | 已在真机验证：`fastboot boot` 临时启动、继承 framebuffer、实体按键、RAM FAT、EFI 运行与返回（tag `cas-complete-001`） |
| XNU | 真机上 CPU0 已到 VM 与早期平台初始化 |
| 进行中 | 标准 UniXNU 构建、userdata 上的 UFS 根卷、8 核、Hello 界面、ST FTS 触摸 |

## 仓库分工

| 仓库 | 职责 |
| --- | --- |
| UniXNU-cas | 板级配置、设备树、defconfig、板级 package 与补丁 |
| [uefi-cas](https://github.com/ht94911/uefi-cas) | Mu-Silicium 固件：BootShim、SEC/DXE、GOP、按键 |
| [UniXNU-sm8250-common](https://github.com/ht94911/UniXNU-sm8250-common) | SM8250 共享的 CPU、GIC、定时器资源 |
| [UniXNU](https://github.com/UniXNU/UniXNU) | 构建框架、OS profile 与共享组件 |

## 构建

在与本仓库相邻的 UniXNU 目录中执行：

```sh
./unixnu defconfig cas_defconfig -D ../UniXNU-cas
./unixnu fetch
./unixnu build kernel
```

原厂设备树、IPSW 组件、Data seed 等私有输入不随仓库分发。

## 致谢

硬件支持主要参考 [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi)（小米 10，同 SoC、同触摸控制器）、[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) 以及 HackiOS 在 cas 上的记录。许可为 [BSD-2-Clause-Patent](LICENSE)，复用文件保留原有声明。
