# CPU0连续启动

当前目标使用XNU10063.141.1、原生4KiB页及iPadOS17.7.11 ABI。仅启用CPU0，
依次进入调度器、IOKit、根卷选择和init加载；SMP在单核用户态之后接入。

设备补丁保留入口和VM记录，在machine_startup前恢复XNU正常异常向量，后续阶段
写入保留RAM。成功阶段继续执行；内存和平台输入不满足条件时保留原有失败路径。

ArmCpu观察器记录实际PPI27处理、rtclock_intr返回和ICC_EOIR1_EL1完成，采样
第1、2、8次及此后每64次中断。它不替换中断控制器、不改变定时器编程，也不主动
调用处理函数生成结果。适配来源为UniXNU-umi@3eb72578597d2ca16471b172d45eddc096a34a04，
许可位于third-party/UniXNU-umi-LICENSE。

| 记录 | 邮箱字节范围 | 内容 |
| --- | --- | --- |
| Loader |0..256| 启动身份、分配、入口和CRC，另有原始阶段/反码对 |
| 早期异常 |256..384| 异常向量、syndrome、故障地址、寄存器和前一阶段 |
| 平台 |384..512| 实际CPU、时钟、GIC和易失NVRAM属性 |
| IRQ |512..832| 处理、返回、EOI及定时器计数 |
| 启动 |832..960| 调度、IOKit、根卷与init阶段 |
| 首次panic |1024..1280| 调用地址、原因、寄存器快照及有界格式字符串 |

所有记录位于既有4KiB映射内，并绑定固件启动和候选。运行时包使用序号、payload
异或校验及magic/反码提交；1280字节恢复读取器核对稳定快照，旧512字节读取器
不能覆盖新增包。

最小根卷沿用上游mockfs及自有签名ARM64/iOS17静态进程。该程序验证getpid与
HW_PAGESIZE，不是Apple launchd。受保护loader仅接受CPU0基本参数及显式绑定的
rd=md0变体，并核对内核、ADT、根卷和trust-cache的实际哈希。

构建和验证范围见[构建流程](build-format2.zh-CN.md)、[移植状态](porting-status.md)
及porting.toml。
