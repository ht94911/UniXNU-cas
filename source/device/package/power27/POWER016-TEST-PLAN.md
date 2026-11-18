# power016：充电退出与电源服务休眠恢复

承接已封power014和已派发power015的固定GPU015/USB829基础。普通充电的实机基线仍为013；本轮没有改变5/9V请求、FCC500/1500mA、SC8585过压档位、原厂保护、认证或充电泵使能。015的唯一Native42526496183继续处理；本016源码和宿主证据独立，尚未原生构建或上机。

## 已复现的问题与修正

普通充电的退出检查原先只在模式4把未完成效果返回失败。模式2/3降流或恢复未确认时，内层保留owned/uncertain，外层却可以继续调用quiesce并把生命周期报告为完成。宿主真实runtime的五种输入已经复现，统一检查所有充电后端的pending、unverified_effect、owned和uncertain；失败时保留资源，不重发、不确认PM完成。015包含同一修正，本016保持它。

invalidate原先撤下系统电源服务的数据，但CPU诊断副本仍保留旧样本的valid。独立OS/CPU消费回归已复现这个不一致。本016同时撤下两份gauge/charger样本以及pump的有效标记，保留历史数值；恢复后只由实际新采样重新建立有效性。

## 复用已有的电源控制入口

provider9/version1保留snapshot、history和CONFIG0/1充电控制。新增CONFIG2排队本电源worker的suspend，CONFIG3排队resume，arg1必须为0；其他参数仍拒绝。r008既有kind=config即可调用，不改USB协议或主机包。命令回执只表示目标已提交，effect_confirmed保持false；完成由实际lifecycle和后续样本判定。它只控制本电源驱动的已有worker/总线/投票，不请求其他设备休眠或系统复位，默认启动不自动执行这些动作。

| lifecycle | 含义 |
| --- | --- |
| 0 | 尚未运行 |
| 1 | worker运行 |
| 2 | 休眠已请求，尚未完成 |
| 3 | 已完成本方quiesce |
| 4 | 停止已请求，尚未完成 |
| 5 | 已停止且quiesce确认 |
| 6 | 恢复已请求，尚未完成 |
| -1 | 失败，资源保留 |

恢复待完成时不再把旧的3作为已休眠；停止已请求时也不把3当作最终完成。正在转换时新的诊断转换请求返回busy，停止/错误/退出后拒绝；重复同一已确认状态不增加硬件操作。IOPMPowerSource仍仅在实际1/3/5后确认对应步骤。

## 同轮电脑接线验证

1. 完成宿主证明、固定源码、唯一Native、严格预链接、原GPU015对象与两USB ABI消费者、固件/PAC、17入口及读盘/8CPU/显示/HID/电池类的必要电脑资格后，再按实际交接取得113窗口。复用全部仍有效的既有证据，不重派015。
2. 新鲜核指定c7fea50c的Android/root、实际userdata/GPT目标几何、原payload/Data inode/extent/pin、最新合法generation1头和固定镜像。原27 payload范围一次pinfile rawwrite、临时fastboot boot、唯一r008，启动后立即观察。
3. 要求实际mode4/backend2/lifecycle1和本轮日志可写，记录provider9及USB当前session。电脑没有合格PD/ICL时控制必须保持零写入、零owned；不把电脑线当原厂充电器。
4. 用当前会话的CONFIG2一次请求，保存ID并等待同一结果；从snapshot看到实际3、CPU样本/pump有效位撤下、owned0，另取USB状态确认数据通道继续。不能把queued当已完成，超时不重发或强制复位。
5. 同样CONFIG3一次请求，看到实际1、有效新样本与增长的采样时间。最终P1日志的generation和S1时间界定真实恢复后的采样；不能凭旧缓存判定恢复。
6. 可在同次启动做第二个完整循环，验证投票与总线可重复接续；任一实际失败保留现场，不循环尝试错误包。它验证本驱动资源流程，屏幕睡眠和完整系统唤醒仍需独立验收。
7. 取足证据后一次当前session/sequence正常XNU返回。实际Android/root必要短后验，停自有采集，现有APFS读取器只读取本方≤256KiB日志，核CRC/计数/capped与Data头不变，立即释放手机。

## 已有宿主覆盖与待验项目

32个真实runtime场景涵盖原24条、普通充电退出失败、请求排队/待完成、实际worker休眠恢复、新样本、quiesce/resume失败保留资源以及已休眠状态停止。另有OS/CPU缓存失效回归和有界诊断JSON/路由/busy/错误/参数拒绝；8组精确未变的核心、通信、固定PD、GPI和日志证据可沿用。它们不代替原生ABI或真机结果。

人类已要求无人值守：原厂充电器换线、9V/1500mA连续180秒充电与恢复5V/原OVP、屏幕操作等人工项目推后，等关闭无人值守后集中处理。PPS、私有认证、泵直充和90W仍未完成；不将本轮PM诊断当全部电源适配通过，不升主包。保持原Android/root/17/只读27根、generation1 Data不reset、不刷分区、固定复位0和单代理。
