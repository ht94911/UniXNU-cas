# 静态审查与可读测试预期

历史状态：以下记录对应2026-11-07初始源码审查，当时没有编译、C/C++执行、CI、封包或手机验证。当前资格与实测进展见[README](README.md)、[014实测](POWER014-RUNTIME.md)及各固定候选；不以这份历史清单覆盖已经取得的结果。
`out/source-static-review.json` 是研究目录中的文本核查回执；它不能证明编译或行为正确。

## 本轮处理的问题

| 风险 | 源码处理 | 实际验证仍待完成 |
| --- | --- | --- |
| 0xffff电流误判、温度单位、0容量/循环 | 有符号mA、精确温度换算、逐字段有效位 | 27实际测量/安卓UI对照 |
| 100%误推外接供电、旧读数永远保留 | 外接由MCA独立字段；同代快照、TTL、IORegistry与内部字典共同撤销 | UI消费者如何表现unknown |
| 两次USB GET假装同时功率 | 只有明确input_pair_coherent才计算；本runtime不设置它 | 同测点测量能力 |
| MCA迟到/重复/短包/错序/远端重置 | 确切长度/owner/type/opcode/property/seq/epoch/时间校验，seq不随重连归零 | 当前固件原始响应 |
| 原厂jiffies错当ms | schema显式250–1000ms；OS3模块250jiffies/HZ250为1000ms | 当前OS4设置 |
| 通知风暴打断每个请求 | 合并刷新、使当前充电器快照无效再重新读取 | 实际插拔延迟和通知数量 |
| GPI共享SE冲突 | 原厂LOCK/UNLOCK、保留RX/TX两种完成事件、解析IMMEDIATE的TRE索引 | 实机共享与异常路径 |
| DEALLOC误用状态值 | 等待控制完成位；超时后停止发后续命令并隔离 | 真实命令/IRQ语义 |
| DMA初始化失败后错误释放 | 分阶段跟踪；命令前授权具体地址；quarantine保留内存/票据 | 实际SMMU/缓存属性 |
| GLINK attachment瞬间清live indices | 新attach API保留所有交还指针 | bootloader/远端交还资格 |
| RX_DONE因TX满丢失 | pending回执队列保留intent busy，后续送回执，不重复client回调 | 拥塞与远端intent复用 |
| fragment、重复intent、坏FIFO/SMEM越界 | 校验remaining/分片和重复ID，校验索引/partition边界，坏流停止 | 夹具运行与模糊测试 |
| SOCCP reset后旧SMP2P指针 | 有重启证据才刷新partition/绑定新in/out并重新协商 | 迁移且无重启证据时停住，不保证自动恢复 |
| SC8585读混合事件寄存器 | 0x0a默认不读；0x15也要求独占事件权限；默认无pump读取授权 | 寄存器读副作用/ADC一致性 |
| 保护变化后旧enable回执生效 | 事务ID/epoch、限值/profile变化即停止；关泵确认后释放本方票据 | 目前无实际执行后端 |
| 生命周期只有API无人调用 | 原生电源服务接power-plane/正常shutdown；worker完成才ACK | 指定包ABI、转换超时、全系统资源顺序 |

## 测试源码与预期（均未运行）

`core_cases.c`：旧同机样本解码为−497mA/37.05°C/6800mAh；−1mA不丢弃；SOC100不推出有线；
过期/代际混合拒绝；18W只在显式同时样本标记下成立；MCA未发送/279B/错seq/截止时刻/旧epoch拒绝；
错误retcode保留失败；通知0 payload7仅按通知解码；序号耗尽停止；热限降低时先关泵，迟到旧ACK不启泵；远端换代时隔离。

`transport_cases.c`：保留交还FIFO指针；TX满时RX_DONE保持pending/busy，空间恢复后只补回执；
非法FIFO头不当空队列；损坏SMEM边界不继续遍历/分配；DFS index进入CONFIG0；SC8541 ID被SC8585 decoder拒绝；
ADC关闭时不发布数值；IBUS raw1024为1920mA、VBUS raw1024为6400mV；未读switching保持unknown。

指定包的真实IOKit测试/替身还需加入以下场景（本轮提供可读预期，未制造真实危险条件）：

| 输入/故障 | 预期 |
| --- | --- |
| suspend请求到达一次I²C读中 | 等本次有界事务结束→quiesce；成功后lifecycle3并ACK；不在DMA运行中撤票 |
| resume、remote重启、旧回调到达 | 重新核lease与epoch；旧样本和回调不能复活；重新协商/刷新后才发布 |
| 正常shutdown | stop采样→确认DMA静止→lifecycle5→原IOService shutdown确认；不发PSCI或强制复位 |
| quiesce失败 | lifecycle−1、不冒称ACK完成、保留资源；记录PM错误，不能声称正常睡眠/关机通过 |
| 实际没有资源owner或schema | bind/访问失败或充电字段unknown；不可填stub成功 |
| 插拔伴随未完成PPS/enable动作 | 更换attach/profile，旧认证和ACK无效；所有动作串行；失联由预先核定自主保护处理 |
| OVP/OCP/OTP/温度源无效 | 夹具驱动退出/降档；禁止真实过充/过热/短路实验 |

## 尚未解决的功能与资格

- 指定包的共享资源provider/late caller/构建ABI绑定未做，这是用户要求的集成前停点；无通用stub代替。
- 当前OS4完整模块、角色/通知语义、主充电与泵的总状态仍需资格。初次SOCCP未ready/部分初始化失败不会盲目重试；caller必须满足ready契约。
- 原厂90W策略、认证执行器、SC8585/HL7606实际控制与温控投票后端未实现；常规充电不能先宣称通过。
- GPIO/ICC/时钟和缓存属性、共享SE完成事件、正常休眠/关机及GPU/Wi-Fi/音频共存未实测。
- native IOPMPowerSource方法存在不证明Apple各UI消费者已接受；电池健康/身份和Android UI SOC校正不在本轮已完成范围。
- 此源码按驻留内核组件设计；DMA/intent及服务寿命保留到关机，不提供任意热卸载。失败保持资源可能阻止正常电源转换，必须记录而不能确认成功。
