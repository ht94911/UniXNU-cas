# 静态审查与等待点

本文前半部分保留2026-11-16第一阶段静态审查记录。该阶段的独立交付已固定；随后人类明确指定GPU051，构建等待点解除。最新已编译源码389d369b4de4cdafda0582dbfcbd86b4630af725、016电脑资格及015实测边界见文末与README，不能把旧等待记录当作当前授权或验收。

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

第一阶段当时准备而未执行：组件原 `Test/check-pagani-haptics-host.py`及新增factory生命周期case，公共SPMI `tests/bus1_cases.c`，设备 `tests/queue_cases.c`。用户要求在编译之前等待，因此没有把这些C用例编译执行或声称通过。一次实机取证计划在 README.md。

实际尚未确认项：完整iOS request→VAD→HAL→kernel→motor、Apple回调与取消时序、振动感觉、连续/组合sharpness语义、持久稳定性及功耗。源码有精确范围，不能作为最终主包合入结论；当前交付可用于指定包集成审查。

GPU051指定后的审查：真实audiomxd UserName mobile/UID501已核，cdev501:501/0600及精确节点名长度已修；core/v8/queue生产主机检查和ASan/UBSan、实际arm64e native/HAL签名、严格38组件/PAC/import、loader、单/八核QEMU及113入口均有本地资格记录。007新增电气采样、HPWR失败恢复/终止、采样通道原值恢复和RC/刹车校准不写用例，核心98857/SPMI14615检查通过。电脑资格不替代实际机械振动。

005实际失败已保存：用户明确无振动，第三较强短脉冲HPWR停止故障，正常XNU返回拒绝；人工回安卓后的真实保护后验与采集器退出通过。007在独立GPU051树修正原厂运行态、直接播放谐振和可靠退出，最新native43933783541成功；007包尚未上机，旧系统006不作为新源码验证。当前不能给出硬件或系统触觉合入通过结论。

007后续实际修正：STOP完成后readback/cache先于停止状态发布，QUD_CANCEL撤掉diagnosticOwner不能留下播放前电气快照；正常HAL客户端期间采样，显式valid/sequence/sampledus。scripts/check-haptics-electrical-stop.py直接提取并执行生产stopped/cacheElectrical，在原硬件模型上确认owner先撤掉、HPWR恢复、失败回读无有效样本，ASan/UBSan通过。007实际direct振动/完全停止与一次正常返回后验已完成，系统触觉待008。

010已经实际执行普通UIKit应用，所有按钮无物理振动；精确app journal记录foreground/selection/impact/notification、CoreHaptics4099，uidd两执行器ID均0，无HALopen/start/PCM/plays。正常返回及root/GPT/prefix/退出/释放通过。011确认的接口缺口为nhpt未登记且返回不可跨进程封送的raw整数数组；改CFPropertyList并按原CustomPropertyInfo声明。初始化端点仅open/ioctl/close不播放，launcher限时目录枚举/preflight，应用查询uidd/duid/pibi；新增生产HAL主机契约用例随Mac构建执行。system HAL mount变化为沿既有成熟挂载路径的有限验证，不能提前宣称其解决009/010。

011实际HAL加载、Actuator35和nhpt跨进程读取均已通过，原VAD仍0、engine4099、normalplays0。缺drte在原CatalogDeviceInfo/HasProperty false分支继续分类，不能用作新接口返回或rootcause。应用本进程HALInitialize errno1与原audiomxd成功open分别记录，不能混成原server硬件错误。下一步仅原VirtualAudio加载/初始化/路由缺口。

012：native0e7c6ff/CI44124303751/attempt1已编译通过，841文件CMS取回；exact-source arm64e HAL/UI签名与所有4KiB codepages、原生产HAL host契约通过。原011只读日志窗口003已退出/保护后验/释放，完整Special日志59364条同boot_UUID记录解码；audiomxd118在104.527s的MobileGestalt Fault为null product-id。实际012基底ADT也仅含空product节点。复用原成熟identity工具后逐节点比较通过，仅新增原20B product-id与原4B AcousticID8001，ADT增148B，其余节点/属性保持。原Persist文件读取限时缺口保留，不把Special里的系统服务状态等同完整启动日志。

012上机失败后，最终信任审查必须覆盖实际addon的HAL/probe/Core/UI/launcher五个二进制及launchd.plist detached签名。生产`scripts/stage-haptics27-system-trust.py --check-only`实际拒绝原012，列出现场f3320d98缓存以及Core/UI/launcher四个缺失cdhash；同一检查对原011有效最终bundle通过。新信任追加只增加原工程模块，原模块/条目保持，不关闭AMFI或改原签名条件。

013实测与判读边界：最终trust补齐后，真实launchd缓存加载和原audiomxd114的GET_STATUS/error0已证；正常按钮执行但StartIO/Write/kernelplays仍0，Coreengine4099，系统触觉未通过。registerednhpt诊断带processID=495，明确为应用本进程HAL副本，其VirtualAudio插件0/VAD0不能作为原服务未加载的独立结论。原VA bundle preflight只证明装载前检，116s峰值读数无当次request/play关联，不作为本轮电气输出。正常返回、必要Android/root/GPT/maps/Data后验和4984实际退出已证。

后续只读方案已经固定原013bootUUID B0FC16CCF1BF45908606B59088E0407D和两份Persist文件（10487936B/1671648B）。领取前沿共享Windows锁和实际最新释放；安卓读者复用成熟APFS完整extent读取，每块不超过256KiB，总量32MiB/150s，元数据/catalog/映射新鲜核对。离线解析沿既有625/12实际timesync校正并筛同boot。此方案本身没有新的系统链通过结果；无人值守期间人工验证后移，不修改Apple二进制、hypervisor属性或未核固件就绪条件。

014实际根因审查：本机013同boot日志证明原VirtualAudio初始化、Actuator物理端口和AID8001调音图均成功，HapticServerManager因缺少hapticsVAD退出。原MG默认名称消费者与XNU `IORegistryEntry::init/getName`和`is_io_registry_entry_get_name`实现核对：OSData name不会建立默认名，仅plane名可供fromPath查找，默认GetName却回退类名。修复仅在生产发布器attach前补默认名，并记录default/plane两实读名称。`scripts/check-haptics-ios-discovery.py`执行生产函数并复现旧plane-only节点拒绝/新节点识别；保持ready/plugin/preset和原OSData编码，正常系统输出未提前记通过。宿主Clang sanitizer库在维护后缺失的构建失败保留，使用现成GCC ASan/UBSan完成同生产用例。

015就绪边界：原类匹配使用IOProviderClass/metaCast，普通IOService的显示名称不满足此条件；新类使用真实OSMetaClass及原168槽IOService，無附加实例字段。生产用例实际执行init/attach/start失败、仅命名拒绝、单次构造、时间单位、重复ready、退休失败保留引用、休眠后再发布。caller只置assetsReady并唤醒原worker，registry注册/退休全程不持hapticGate；原芯片/校准/2V/500ms/STOP路径保持。015目标ABI/晚构造移除/完整服务启动仍待编译及实机。

016生产队列覆盖迟到、部分过期、future队列满/回收、rest保持session和下一序号；实际DIRECT→IDLE回放证保护早停与真正deadline/budget休息分别保持streamready，EBUSY恢复要求coreIDLE/PLAY_EN已清/bus无pending。PCM错误观测按有限计数记录，停止回读原代码保持。HALWriteMix定向模型新增临时错误后同sequence再写与硬IO错误关闭，实际Mac目标用例已通过。

016 最终电脑审查：389d369/Mac44329367469/attempt1/856文件、9原生对象/原168槽/1491最终PAC、严格38组件零缺失和零stub、3原资源、6最终信任/4KiB签页、真实loader/单八核QEMU、本机75r008和183依赖已通过。015已创建原VAD/HapticServer且Core API成功；仅一次360mV/25mA弱驱动与随后断流，不作为完整机械反馈。016原厂限值、STOP和总线保护保持；真机重复播放与触感待验。

017静态审查：新的FIX_HOST_CLOCK仅在当前HAL进程拥有端点、流未启动且原执行器有效时允许；helper要求current_task、原CoreAudio UUID/代码/slide/常量，IOMemoryDescriptor pin与原phys读写和缓存同步复用音频072。独立clock gate不持motor gate做VM工作；原硬件寄存器、队列、限值和STOP不变。实际osfmk startup.o.json用于task accessor，不能只向BSD record追加MACH_KERNEL_PRIVATE。73个原fileglob例外模型、productionprepareClock与原23words/5 edits和绝对事件时基模型通过ASan/UBSan；原同步生产函数MachO目标ISB1/DSB2/DC CVAU/IC IVAU已核。完整native和真机待。

018静态：沿同一无payload专用ioctl/独占音频进程/未启动流条件，新增第二映像header/UUID/slide/pin/原RenderProc5words核对，频率及倒数只允许原值/本机值后CAS/回读。两映像都先核完才改，已改CoreAudio仍需核AudioToolbox，普通应用和硬件链无变化。生产prepare/publish的原值/混合值/已改/24M/UUID/指令/slide/权限错误及回读测试通过；原ARM64 RenderProc和queue-horizon比较重放在284/498/623/1000秒复现20%时间偏差，修正后事件不再推迟；立即0与24MHz旧路保持。目标编译及机械系统验收待。

019静态与当前范围：018 all4普通触觉的人类纠正确认和78/78启停已保存，强度未过；USB随后失联，用户返回安卓后必要保护/51644退出/实际释放过，未发送USB返回命令。019只更改生产queue的int16绝对峰值→permille包络，不改协议/权限/PAC/时钟修复/马达寄存器/cap/校准。全int16幅度域检查静音阈值、正负对称、单调、max1000、真实低onset样本、同原begin/end/晚包/休息/取消/关闭；2000mV上限和最大500ms仍由既有core执行。来源5/8..1策略只做数学重写；本机实际手感及增强后停止需019实机。
