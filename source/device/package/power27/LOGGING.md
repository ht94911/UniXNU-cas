# 有界电源日志与 USB 重连观测

使用原 Data 已存在的 `/private/var/log`，独占创建本轮 `nezha-power-<hex>.log`，上限256KiB。VFS写入在独立线程，以同步追加、属主/链接数/普通文件及no-follow检查执行；不分配pinned文件、不rawwrite Data或重置卷。正常XNU关机标志触发电源线程撤销已拥有的受限控制、停止采样及日志，再按原生命周期结束。失败或失联不增加重启方式。

既有 `NEZHA_POWER_LOG_V1` 头及CRC footer保持。完整行类型如下：

| 类型 | 保存内容与口径 |
| --- | --- |
| P1 | 原电池/有线/SC8585采样及控制状态；ms为该次启动的单调时间 |
| CPREG | 启动继承的充电泵控制寄存器，只在原读取路径成功后记录 |
| U1 | USB连接、generation、配置/DTR和失败变化；CPU缓存的事件/命令/失败码及积压，不从电源线程访问USB MMIO |
| A1 | 发生变化的MCA元数据，以及每次受限控制回执；分别保留wire与decoded结果，raw只含这些白名单状态或PDO数据 |
| S1 | 电源控制收尾状态与实际XNU关机标志 |

U1不因poll计数变化重复写；A1不重复同一attachment内未变化的只读回复。控制回执不去重。所有记录走已有非阻塞队列，文件关闭、队列满或锁竞争的丢失计数明确保留。查询USB快照遇到STATUS_BUSY时重试下一次周期，不把全零快照当成功。selected USB头与原生archive必须同源，提供追加的缓存core字段；构建脚本从已选 `drivers/QcomXnuUsbDebug/include` 获取公开接口。

日志容量将满时停止收纳并为footer预留128字节；provider9报告bytes/capped。到限记录仍有CRC和计数，但不是完整运行时段。VFS错误或系统未完成正常收尾时可能缺footer，读取端保留已恢复字节、实际记录数、末尾截断长度和complete=false/CRC未验证。CRC或声明计数矛盾必须报出，未知类型和不完整末行不用于完整性通过。

充入量按P1的实际时间计算。选一个连续、同attachment、有效采样的180秒区间，两端只在相邻有效采样内插值，不用聊天答复间隔，也不跨大间隙填补。MCA MAIN_BUCK电流不是总USB输入电流；电池端顺序采样功率仅为估算。

此日志与重连候选不改变ICL/FCC资格、高压/PPS、认证或充电泵使能条件。新USB和正常关机收尾需实机验证，不能以宿主模型晋升功能基线或主包。
