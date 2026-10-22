# 真机验收与可复现记录

[English](validation.md) · [项目首页](../README.zh-CN.md)

**`cas-complete-001`** 里程碑于 **2026 年 10 月 23 日（UTC+8）**完成。结果针对一台小米 10 Ultra，型号 M2007J1SC / cas，已记录的约 12 GB 内存配置；临时启动前后运行 Android 15 / OS3.0.4.0.VMKCNXM、4.19.157-moon 内核。

已验收镜像大小 **17,616,896 字节**，SHA-256：

```text
a085d19b6a4a1ea429f2f10cb191050f316cf044d4fd864b53d398e4f6a4f67c
```

## 三轮独立运行

三轮独立运行全部使用这一镜像。

| 试验 | 菜单与 EFI 观察 | 退出方式 |
| --- | --- | --- |
| `t02-complete-001` | 四项菜单；音量单步导航；短电源确认；约一秒电源长按释放无误确认；至少五分钟实测稳定显示；CasSmoke 标记及返回可导航菜单；缺文件底部 `Not Found` | UniXNU **Reboot to Android**，随后 USB 核对身份和启动完成 |
| `t04-complete-002` | 独立新启动；菜单和导航；CasSmoke 标记；返回 `Success` 后仍可导航 | UniXNU **Reboot to Android**，随后 USB 确认 |
| `t04-complete-003` | 独立新启动；菜单和导航；CasSmoke 标记；短电源返回 `Success`；退出到基础菜单并显示 Firmware information | 基础菜单 **Reboot to Android**，随后 USB 确认 |

五分钟稳定性记录来自第一轮。

## 证据等级

| 类型 | 实际记录 |
| --- | --- |
| 现场观察 | 屏幕文字、实体按键、菜单跳转和 Android 画面，并记录时间 |
| 主机观测 | 显式选择的 USB 身份、临时启动响应、Android `boot_completed=1`、保护分区读取和哈希 |
| 离线检查 | 126 项源码测试、镜像／FV／FAT 校验，实际构建 EFI 字节的 QEMU 执行、返回及缺文件恢复 |
| 独立重建 | 新的完整构建与打包输出，对比启动镜像、28 个 EFI、FD、DTB、gzip、RAM FAT、shim，共 34 项一致 |

QEMU 使用合成 cas 日志头，不验证手机 GOP、SPMI、BDS 或 RAM Block I/O。本项目没有录到 UEFI 视频或串口轨迹。[机器可读验收摘要](evidence/accepted-results.json)记录三轮结果、验证方式和原报告哈希，原始私有证据由项目持有者保留。

## 保护状态

第一轮前与第三轮后，**18 个保护分区**的哈希一致，**6 个 UFS LUN 的主／备 GPT**检查通过，24 份对应二进制块哈希相同，并与恢复基线一致。三轮过程采用 RAM 临时启动和 RAM FAT，没有刷写分区、解锁、清 userdata 或重分区。

这些检查不证明 userdata 中每个文件都没有变化，也不能代替个人数据备份。

## 可复现与发布身份

[34 项产物对比](evidence/rebuild-comparison.json)记录固定工具链下的字节比较及相同哈希。[source-pins.json](../source-pins.json)记录三个源码提交、固件基础、依赖版本、工具版本和私有输入哈希。

原始源码 bundle 也已通过空 bare Git 对象库导入，恢复出预期提交与文件树。源码恢复与独立完整构建是不同检查，也都不代表已在其他系统或工具版本复现。

`cas-complete-001` 标签标记实际验收源码，`v0.1.0` 增加文档与支持文件，固件实现保持一致。发布附件另附 `SHA256SUMS` 校验文件。

## 范围边界

已验证：UEFI／UniXNU 菜单、实体按键、只读 RAM FAT、EFI 加载／运行／返回、缺文件错误返回、退出到基础固件菜单、菜单重启回 Android 和记录中的重建。

尚未验证：DarwinBoot 向 XNU 交接、XNU／iOS、触控、GPU 加速、USB 键盘、内部 UFS 文件系统、充电管理、UEFI 中的 ADB、其他 cas 手机／内存版本、其他固件和其他机型。这里不宣称已经完成上游编号移植阶段中的全部子项。

完整启动镜像及原厂 DTB 衍生内容保留在本地，公开发布只提供审阅过的源码及源码构建组件、文档、许可证和脱敏证据，不包含原始设备固件、身份、GPT 数据或恢复备份。
