# 静态审查与等待点

本文前半部分保留2026-11-16第一阶段静态审查记录。该阶段的独立交付已固定；随后人类明确指定GPU051，构建等待点解除。最新已编译源码c7d3ba749849fc2ab46840d275957c50bfbb914f及007测试状态见文末与README，不能把旧等待记录当作当前授权或验收。

第一阶段当时状态为 **implemented，尚未编译或真机验证**。当时测试包未指定，没有执行编译器、CI、封包、ADB/fastboot/COM或手机队列领取。

已执行的检查：三个工作树 `git diff --check`；所有触觉新增C/C++与已改组件文件用 tree-sitter 语法解析（共享头规范化条件 `extern "C"`区域；保留的v7 glue中`__has_include`、weak全局声明及固定underlying匿名enum的4个解析器诊断与原基线相同，规范化后无诊断，非预处理/类型检查）；Info.plist、dependencies.json、porting.toml结构读取；原厂ELF符号/反汇编及原D16 ADT/plist/VirtualAudio文本引用核对。本包框架的 `porting-record.py check --base 8dd3729` 通过14个结构化area。当前上游工具不认识继承的QcomXnuUsbDebug组件，故使用原本包框架catalog核对，不修改上游catalog。语法结果保存于本地 `evidence/haptics27/20261116/static-syntax.json`。这些检查不证明链接、PAC、HAL加载、硬件启停或iOS触感。

本次逐路径静态审查覆盖：

| 边界 | 代码结论 | 指定包后验证 |
| --- | --- | --- |
| SPMI | v8 features分总线、global APID命令、bus-local owner；每事务共享锁并重核PPID/EE；超时pending未退休前禁止新CMD | 新鲜APID/owner、读写通道实际映射、DONE/DENIED/DROPPED、与keys/ADC并发 |
| 板级校准 | 原型/修订读到后才读状态结果；状态selector临时修改后恢复；保留RC/count/刹车校准，不执行校准命令 | 原bootloader是否保留合格period/RC、实际CFG/PTN minor、f0范围、温度/故障 |
| 启动限值 | CFG/PTN寄存器白名单与2000mV硬上限；HBOOST/SDAM不写；foreign SWR/open-loop boost拒绝；FIFO沿原分配 | direct25%/50ms、effect/primitive实际gain/时长及stop读回 |
| 队列 | 完整包长度、frames、session/epoch/sequence、绝对时间窗、容量和溢出；旧epoch/过期样本不启动 | 两客户端、快速重复、取消跨bus事务、producer退出、queue满及计数回绕 |
| 播放/停止 | amplitude不重置期限；500ms上限及cooldown跨session；STOP独立保存硬件回读结果；停止时不做主动f0测量 | 停止延迟、HPWR状态与PLAY_EN清除、FIFO耗尽、总线错后可观测停止 |
| 休眠/退出 | 单worker硬件所有权；request gate在bus工作期间释放；取消先退休事务，再硬件stop；异常不释放live mapping | 精确正常关机caller、rootdomain通知、100ms stop及1000ms退出预算、panic边界 |
| HAL | 仅Actuator、不碰Speaker/Codec；float32→PCM16，NaN为0；Start/Stop按client计数，旧fd关闭取消，实时callback不等待硬件 | 原audiomxd枚举及UID匹配、VAD graph、buffer/clock时间基准、错误码与系统取消 |
| iOS身份 | 原D16能力属性为OSData字节；plugin/preset/executor可见后才发布，无私有vtable/PAC硬编码 | MobileGestalt启动缓存、实际/Library路径、系统设置/静音、CoreHaptics/通知请求 |
| 记录/隔离 | 本地三层源码提交，未改活动board/feed/patch series或主包，无Apple/vendor原件进Git | 指定包完整source manifest、archive/init caller、addon/sign/trust及镜像固定SHA |

准备而未执行：组件原 `Test/check-pagani-haptics-host.py`及新增factory生命周期case，公共SPMI `tests/bus1_cases.c`，设备 `tests/queue_cases.c`。用户要求在编译之前等待，因此没有把这些C用例编译执行或声称通过。一次实机取证计划在 README.md。

实际尚未确认项：完整iOS request→VAD→HAL→kernel→motor、Apple回调与取消时序、振动感觉、连续/组合sharpness语义、持久稳定性及功耗。源码有精确范围，不能作为最终主包合入结论；当前交付可用于指定包集成审查。

GPU051指定后的审查：真实audiomxd UserName mobile/UID501已核，cdev501:501/0600及精确节点名长度已修；core/v8/queue生产主机检查和ASan/UBSan、实际arm64e native/HAL签名、严格38组件/PAC/import、loader、单/八核QEMU及113入口均有本地资格记录。007新增电气采样、HPWR失败恢复/终止、采样通道原值恢复和RC/刹车校准不写用例，核心98857/SPMI14615检查通过。电脑资格不替代实际机械振动。

005实际失败已保存：用户明确无振动，第三较强短脉冲HPWR停止故障，正常XNU返回拒绝；人工回安卓后的真实保护后验与采集器退出通过。007在独立GPU051树修正原厂运行态、直接播放谐振和可靠退出，最新native43933783541成功；007包尚未上机，旧系统006不作为新源码验证。当前不能给出硬件或系统触觉合入通过结论。

007后续实际修正：STOP完成后readback/cache先于停止状态发布，QUD_CANCEL撤掉diagnosticOwner不能留下播放前电气快照；正常HAL客户端期间采样，显式valid/sequence/sampledus。scripts/check-haptics-electrical-stop.py直接提取并执行生产stopped/cacheElectrical，在原硬件模型上确认owner先撤掉、HPWR恢复、失败回读无有效样本，ASan/UBSan通过。007实际direct振动/完全停止与一次正常返回后验已完成，系统触觉待008。

010已经实际执行普通UIKit应用，所有按钮无物理振动；精确app journal记录foreground/selection/impact/notification、CoreHaptics4099，uidd两执行器ID均0，无HALopen/start/PCM/plays。正常返回及root/GPT/prefix/退出/释放通过。011确认的接口缺口为nhpt未登记且返回不可跨进程封送的raw整数数组；改CFPropertyList并按原CustomPropertyInfo声明。初始化端点仅open/ioctl/close不播放，launcher限时目录枚举/preflight，应用查询uidd/duid/pibi；新增生产HAL主机契约用例随Mac构建执行。system HAL mount变化为沿既有成熟挂载路径的有限验证，不能提前宣称其解决009/010。
