# Xiaomi 10 Ultra 的 UniXNU 适配

[English](README.md) · [构建](docs/build-format2.zh-CN.md) · [移植记录](porting.toml) · [验证](docs/validation.zh-CN.md)

本仓库提供Xiaomi 10 Ultra / Mi 10 Ultra（`cas`、SM8250）的板级配置和平台集成。
当前目标为XNU 10063.141.1、原生4KiB页及iPadOS17.7.11（j120ap）ABI。
启动目标仅使用CPU0和RAM存储，SMP在最小用户态之后接入。

## 状态

| 能力 | 结果 |
| --- | --- |
| UEFI | 已验证临时启动、继承GOP、实体按键、只读RAM FAT、EFI执行/返回和Android恢复 |
| XNU CPU0 | 候选005已验证内核入口、VM及早期平台/时基初始化 |
| 当前构建 | macos-26/Xcode26.6已完成格式2连续内核、匹配预链接及自有签名静态进程构建 |
| 运行时工作 | 持续定时器IRQ、RAM根卷、EL0和iOS界面仍需新候选的真机证据 |

[构建记录](docs/evidence/format2-build-001.json)固定源码、编译器、产物哈希及CI运行。
静态进程是独立的系统调用测试程序，不是Apple launchd。当前范围和硬件待办见
[porting.toml](porting.toml)及[支持状态](docs/porting-status.md)。

## 源码归属

| 仓库 | 职责 |
| --- | --- |
| UniXNU-cas | 板级DTS、RAM保留区、组件选择、受保护组装及设备记录 |
| [UniXNU-sm8250-common](https://github.com/ht94911/UniXNU-sm8250-common) | 共享CPU拓扑、GIC及定时器资源 |
| [uefi-cas](https://github.com/ht94911/uefi-cas) | BootShim、SEC/DXE、GOP、实体按键及重启支持 |
| [UniXNU](https://github.com/UniXNU/UniXNU) | 构建框架、OS配置及共享组件 |

## 构建

使用`ci/xnu/format2-sources.json`固定的框架版本，在框架目录运行：

```sh
./unixnu defconfig cas_continuous_defconfig -D ../UniXNU-cas
./unixnu fetch -D ../UniXNU-cas
./unixnu status --json -D ../UniXNU-cas
./unixnu build kernel -D ../UniXNU-cas -j4
```

[构建说明](docs/build-format2.zh-CN.md)涵盖源码固定版本、私有输入和受保护固件。
已验收EFI版本的操作见[按键与恢复](docs/operations.zh-CN.md)；冻结的
`cas-complete-001`源码及[v0.1.0验证](docs/validation.zh-CN.md)独立保留。

本适配使用RAM临时启动。专有输入、私有ADT/身份、恢复备份和设备绑定启动镜像
不进入Git。贡献要求见[CONTRIBUTING.md](CONTRIBUTING.md)及上游
[设备仓库指南](https://unixnu.com/zh/porting/device-repository)。
