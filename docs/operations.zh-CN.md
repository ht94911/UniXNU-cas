# 启动、按键与恢复

[English](operations.md) · [项目首页](../README.zh-CN.md)

本手册描述已测试的**小米 10 Ultra（cas / M2007J1SC）**组合：Android 15、OS3.0.4.0.VMKCNXM、4.19.157-moon 内核，以及固定的原厂 DTB／DTBO。它不代表另一台手机或系统升级后仍然兼容。

## 临时启动前

使用本地重建的 `cas-complete-001`，大小 **17,616,896 字节**，SHA-256：

```text
a085d19b6a4a1ea429f2f10cb191050f316cf044d4fd864b53d398e4f6a4f67c
```

保持 USB 连接，现场需要有人能看屏幕、操作按键，并有已知恢复输入。每轮保存新编号、开始时间、镜像哈希、当前设备／模式和退出计划。若上一轮只有启动意图而没有结果，先检查手机，再决定下一步。

下方为 ADB／Fastboot 命令格式，把 `USB_SERIAL` 和 `ABSOLUTE_BOOT_IMG` 替换为刚核对的实际设备和文件。每条操作显式选择当前设备。在 WSL 构建主机上，可由 Windows 运行 USB ADB／Fastboot，由 Linux 运行构建工具。

```text
adb devices -l
adb -s USB_SERIAL shell getprop ro.product.device
adb -s USB_SERIAL shell getprop ro.product.model
adb -s USB_SERIAL shell getprop sys.boot_completed
```

确认 `cas`、`M2007J1SC` 和 Android 启动完成，然后进入 Bootloader 并重新枚举：

```text
adb -s USB_SERIAL reboot bootloader
fastboot devices
fastboot -s USB_SERIAL getvar product
fastboot -s USB_SERIAL getvar unlocked
fastboot -s USB_SERIAL getvar max-download-size
```

只有当前设备确认为 cas、Bootloader 实际报告 `unlocked: yes`，且下载上限大于镜像大小时才继续。Android root 或 green／orange 文字不能替代当前 Fastboot 状态。本手册不包含解锁、刷写分区或重分区。

核对本地镜像大小及 SHA-256 后，只发出一次：

```text
fastboot -s USB_SERIAL boot ABSOLUTE_BOOT_IMG
```

保存主机响应并观察手机。`Sending/Booting OKAY` 只证明命令响应完成，不能据此认定菜单已运行。实测流程使用 RAM 临时启动和 RAM FAT。

## 菜单与实体按键

菜单标题为 **UniXNU - Xiaomi 10 Ultra**，没有自动执行倒计时。

| 操作 | 行为 |
| --- | --- |
| 短按音量上／下并松开 | 移动一个条目；持续按住会在延迟后重复 |
| 短按电源并松开 | 在松开时确认 |
| 按住电源约一秒后松开 | Escape，主菜单忽略，释放时不再误触确认 |
| 持续长按电源直到硬件重启 | 故障恢复，与约一秒的菜单动作不同 |

| 菜单项 | 行为 |
| --- | --- |
| Run CasSmoke EFI test | 从只读 RAM FAT 加载 `EFI/Tools/CasSmoke.efi` |
| Check missing EFI handling | 尝试缺失的 `EFI/Tools/Missing.efi`，返回错误 |
| Reboot to Android | 重启到原有 Android |
| Return to firmware information | 退出 UniXNU，进入提供信息与重启选项的基础固件菜单 |

CasSmoke 显示 `CAS EFI SMOKE - build cas-smoke-001` 和 `StartImage reached the test application.`。短按电源返回，检查 UniXNU 仍能导航，底部状态行显示 `Success`。源码在返回前还会打印 `CAS_SMOKE_RETURN_SUCCESS`；瞬间闪过的内容只有确实看到才能记为观察结果。

缺文件测试直接重绘菜单，返回值在靠近按键说明的**底部状态行**：`'Check missing EFI handling' returned: Not Found`。本版本没有需要额外关闭的错误确认页。

CasSmoke 源码还实现了较长电源按键触发 Escape、返回 `EFI_ABORTED` 的路径；它不属于此次三轮里程碑中独立验收的真机用例。

## 退出与恢复

正常退出时选择 UniXNU 或基础固件菜单中的 **Reboot to Android**。观察 Android 启动，再通过 USB ADB 核对设备身份及 `sys.boot_completed=1`，记录实际采用的退出路线。

黑屏或菜单无响应时保留最后画面和主机响应。如果重新枚举并确认为 cas Fastboot，可使用 `fastboot -s USB_SERIAL reboot` 恢复 Android。否则采用本机已验证的持续长按电源直至重启方式，约 10–15 秒仅作提示，以实际重新启动为准。随后重新确认模式，结果未知时不要自动再次发送 `boot`。

原 Android 无法启动时停止叠加试验，使用设备持有者自己的恢复资料检查当前状态。持久恢复写入不在这份临时启动手册范围；分区备份也不能代替个人数据备份。

## 保存证据

分别保存菜单、按键、EFI 标记、返回菜单、回 Android 的观察及时间和镜像 SHA-256。五分钟稳定性需要持续观察，中断时间不计入观察窗口。现场观察与主机／模拟器日志分开标注。更广泛的能力声明须先对照[里程碑验收](validation.zh-CN.md)。
