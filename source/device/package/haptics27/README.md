# nezha iOS27 触觉源码候选

**GPU051 已指定并实测：018 的正常选择、轻触、通知和短连续触觉均由人类确认实际振动，但幅度过低，强度未通过。018 已回安卓并完成 root、目标映射、17/27 Data 头和自有采集退出后验，已释放手机。019 在原厂 2V/500ms 范围内修正 Apple PCM 到本机 LRA 的强度映射，源码和电脑检查进行中。** 编译与实机证据按包身份绑定，旧失败轮次保留。复用与许可证见 `SOURCES.md`，依赖见 `dependencies.json`。

真机已确认 PMIH010x / HAP530、SPMI v8 bus1 SID7，CFG/PTN/HBOOST 分别0xf000/0xf100/0xf200，global APID2254/2255/2256；CFG5.1.8.1、PTN5.1.0.0、HBOOST31/01。马达按原厂LRA、160–180Hz范围，名义5555µs；旧安卓日志5723µs，005本轮读取有效5746µs、RC mode4/count0及8064字节FIFO。9Ω为原厂标称阻抗；具体马达商品型号与当前温度尚未确认。

当前实现包含：

- 原厂波形/谐振/RC状态入口、bus1 APID/EE核对、共享SPMI锁及不确定命令保留。
- 原范围2000mV上限、幅度1–1000、≤500ms受控短脉冲、FIFO原六效果/九primitive逻辑及可靠停止回读；原custom8500mV不作为本候选电压。
- 48kHz单声道Actuator HAL；40字节包头、session/epoch/sequence、Mach时间戳；2ms包络映射到本机LRA载波。持续触觉最多500ms后休息500ms，幅度更新不续期限。
- 完成、取消、旧会话拒绝、客户端退出、静音样本、流耗尽、休眠/正常重启退出；旧请求在硬件事务期间取消后，先退休事务再停止。STOP成功回执需要硬件PLAY_EN清除与HPWR停止，超时不当作完成。
- 007复用原厂HAP530运行态V/I通道恢复、direct自动谐振、2000mV时500mV headroom，以及SWR-aware停止恢复。诊断暴露真实PLAY_EN/HPWR/source、VSENSE/ISENSE原始值和原厂单位换算；这些仅是电气证据，不自动宣称机械振动通过。
- `/product/haptics`字节能力节点，只有Actuator插件、实际执行器和原D16预设都可见时发布；原iOS用户设置、铃声/通知策略和CoreHaptics合成仍走系统栈。

`nezha-haptics`源码开关：缺省0，1只读probe，2配置/停止后退出而不驱动，3发布执行器；没有启动测试振动。`nezha-haptics-ios`缺省0，1基础触觉能力、2带原D16合成预设。007 使用 `nezha-haptics=3 nezha-haptics-ios=0`；016 使用 `nezha-haptics=3 nezha-haptics-ios=2`。015 已实测 CoreHaptics 瞬态/连续/取消和停止回调，selection/impact/通知的普通请求也已执行；后续重复驱动和实际触感仍待通过。每轮证据绑定准确编译源、镜像和设备会话。

能力限制：包络保留强弱/时长/间隔，sharpness仅对应原厂固定载波，不能称任意频率与Apple闭环效果等价。PWLE频率曲线、SoundWire/ADSP触觉、长铃声与精细组合未验；不对未知寄存器、OTP、NVM、持久校准或原厂保护阈值做写入。元数据属性/queued不能当作系统触觉成功。

GPU051集成与验证路径：

1. 从人类指定的固定源码/镜像创建独立副本。以其framework、SoC、OS、Data与已验模块为基线，应用公共SPMI、组件、设备三个增量。
2. 将 `Sm8850SpmiV8.c`、组件 `HapticsSpmi.c/HapticsCore.c` 和本目录六个内核源加入现有archive recipe，复用本包XNU compile records、arm64e flags、生成头文件/32线程。组件原v7 glue和OnePlus参数不加入nezha。
3. 将 `nezha_haptics_start`接到资源就绪的正常线程caller；将 `nezha_haptics_quiesce`接到卸载卷之前的正常关机与设备退出。不能只靠通知queued或PSCI兜底证明马达停止。执行器就绪由真实 `AHSFirmwareReadyNotifier : IOService` 类发布，原布局136B/168槽、无新增实例字段；复用已有 ALS/AES 晚构造与正常服务生命周期，最终 PAC 按目标产物核验。
4. 用现有Mac HAL recipe构建独立 `NezhaHaptics27.driver`，4096页签名/严格trust/预链接；加精确 `/dev/nezha-haptics27`访问权限，不扩大其他设备。原D16/24A446调音资产和VirtualAudio留本地，沿指定包已有addon/union挂载路径。启用能力节点前检查插件、预设、caller真实已装载；具体权限与启动补丁在指定包上导出。
5. 执行组件原Test、追加factory cases、公共层bus1 cases、本目录queue cases，再做实际目标构建/PAC/KPI/import严格核对。静态语法检查不替代它们。
6. 真机先probe与50ms/25%短脉冲（显式工具编译命名NezhaHapticTest，避免内核proc_name长度截断），回读停止及原保护；再同次验证六效果、快速重复/取消、UISelection/impact/通知、CoreHaptics瞬态/连续、锁屏唤醒及音频/GPU并发。对齐请求序号、启动、停止、完成与人工触感。数据代际、原根卷和安卓保护均沿当时窗口最新事实。

回退：独立测试副本撤掉haptics archive/caller、HAL/addon增量和两个开关，恢复指定包原身份；不删除原厂输入，不重建Data，不变动17主包/安卓/root。最终合入由原主聊天审查。

2026-11-17电脑结果：最新Mac原生43933783541/head042e9e7205ce4132042c53ead10ce7da3fa1b193成功；native279496b76bb3d6bc97e6ea11444cc1799429b638da271f6b71de3ddf147a5231。核心98857、SPMI14615检查及ASan/UBSan通过；38组件strict零缺失/零stub，1323原有PAC槽与3原资源区复核；loader真实parser、单核/8核QEMU通过；113部署镜像与75文件r008已核。007 boot31dda3c7ad6d51aa3da2d04c2c1f17294b83a35ac7d07f0495e4448f51359a15，embedded payload306fb898dd97760d097ea2f013bc05eb5b9180b7c51592b05f2f3015f7271d46。原729jobs/56addon/4trust模块保留，007不写手机磁盘payload。

实际轮次：001在SWR_ACCESS判定处拒绝，003在越界period处拒绝；均未驱动。005初始化通过，两个50ms/25%及两个300ms/50%仅命令与停止计数通过，用户明确反馈没有振动；第三300ms脉冲HPWR停止超时，正常USB退出被拒绝。随后人工回安卓，实际root/GPT/maps/17/27前缀后验和自有采集器退出通过并释放。007 据此修采样、自动谐振和停止链，后续实测结果见下一段。该005轮次验收为：总线/身份已测，机械振动失败；后续007实际结果见下段。

007本机实际通过（2026-11-17）：原厂5726µs、FIFO8064；25%/50ms后峰值502mV/38mA，50%/300ms播放中997mV/84mA，全幅300ms播放中1984mV/183mA。人类确认“有震动！我感受到了！”及“已完全停止”。3次100ms重复与6个FIFO效果命令/停止过，六效果实际触感未逐个验。取消有实际stop13/logerror0，但USB电气缓存仍play129/power3；新源在STOP后强制回读、为采样增加valid/sequence/sampledus并在HAL客户端期间采样，生产函数模型ASan/UBSan通过。一次正常XNU返回、Android/root/GPT/map-pin/17同135168/27generation1/52204退出与文件释放通过；未写手机payload。008系统候选在电脑构建，不能沿用旧006验收。

008系统实测（2026-11-17）：productstage2/addon5-error0/D16preset1/commit0已通过；有限CoreHaptics程序在原Data路径被SystemPolicy拒绝，plays0/stops0，结果缺失、stdout/err为0。初始V/I峰值是上轮Android残留而非本轮输出。一次正常返回、Android/root/GPT/maps/17同135168/27gen1/78952退出后验与释放通过。009复用音频105和传感器002已有readonly `/usr/libexec` union，为同一签名Core程序发布系统执行路径；不修改全局执行策略。

009/010系统实测（2026-11-17）：009有限程序从readonly/usr/libexec成功执行，supportsHaptics1但engine4099、requests/plays0。010普通应用原LSproxy installed1/placeholder0、open1，actual foreground278.145s；selection/medium impact50%/notification及重复按钮均已调用，人类明确反馈“实测所有按钮均无震动”。CoreHaptics engine持续4099，原CoreAudio查询Actuator与VirtualAudioDevice_Actuator都device0且无kernel HALopen/start。010正常XNU返回、root/GPT/map-pin/17同135168/27generation1/79064退出、48MiB内自有日志提取与释放通过。

011根据此现场补齐：原loader已静态核三目录及.driver后缀。沿成熟/system/Library/Audio/Plug-Ins/HAL readonly union发布同一独立Actuator，保留原BuiltinAudioPlugin。HALInitialize只开/ioctl/close现有端点取就绪状态，不发播放；nhpt按AudioServerPlugInCustomPropertyInfo登记为CFPropertyList，供原CoreAudio跨进程传输；补成熟Actuator可选属性，记录末次未知selector。应用同时核uidd/duid/pibi，launcher在有限deadline内记录目录枚举和CFBundlePreflightExecutable。硬件电压/波形/校准、正常系统策略和FIFO执行器保持007沿用；待新包实际加载及正常请求。

011系统实测：原audiomxd(118)在106.032s初始化HAL且open内核endpoint成功；launcher系统HAL目录枚举expected1、CFBundlePreflight1/error0。前台应用277.495s原CoreAudio得到plugin34、Actuator35，registerednhpt CFPropertyList传递成功；VirtualAudioDevice_Actuator仍0，StartIO/Write0、Coreengine4099、kernelrequests/plays0。人类明确“四项点击后依然都没有震动”。drte最后未知属性经同原24A446音频062证据核为HasProperty gated可选项，无删除/拒绝设备分支，不作为根因或猜测返回值。正常返回、保护/root/17同135168/27gen1/77328退出、708608字节自有日志读取和release通过。后续核原VirtualAudio加载、初始化和ActuatorVAD创建。

012补齐011已观测的Autobox状态ioctl拒绝：原fileglob检查先于vnode检查，按既有原进程/精确devfs节点和唯一只读GET_STATUS恢复访问。原VirtualAudio建立VAD仍待证；请求原011持久日志的Android只读文件窗口，获取后释放，不为该调查重启iOS。

012诊断另记录HAL属性返回进程PID、原VirtualAudio插件和Default VAD查询、原`vain`四字节初始化状态及原VirtualAudio.bundle预检。`vain`只读契约由同24A446的VirtualAudio消费者与MediaExperience核定，不写初始化状态，不改变原路由和固件就绪条件。普通应用读到的HAL副本PID用于与原audiomxd区分。此轮尚未编译或真机验证。

012构建已通过原Mac/CMS流程，native源码`0e7c6ff9912cad29597a52e9c035c7e0dd0ccdb6`。随后原011日志显示audiomxd118在104.527s报`invalid product id '(null)'`；实际GPU051/011的`/product`仅含name，而固定D16 donor有20B product-id及4B acoustic-id。封包复用nezha音频060/063的成熟`audio27-product-prepare.py@825a746`，恢复这两个原属性并逐节点核对其他属性不变。设备源码中的`scripts/prepare-haptics27-system-identity.py`从原工具复用，不包含私有donor值；值来自固定本地profile及donor。原D16 AID8001调音资产沿原根读取，未改Apple二进制或原厂校准。包装提交与已编译native提交分别记录；新身份尚待正常系统链路真机验证。

012实机（boot7de0f063…）：`launchd_cache_loader[24]`因新启动任务缓存cdhash `f3320d98081a1cb5c109c64daf2aaa69a283ffd5`未在最终trust bundle内而panic，系统触觉尚未启动。原照片、USB断点和同次失败保留；人工返回安卓后GPT/map/17同135168/27generation1及23036采集器实际退出/释放通过。封包漏掉了011已完成的系统文件信任追加：缓存、Core测试程序、launcher、普通应用四条。新`scripts/stage-haptics27-system-trust.py`复用既有cache签名验证及UniXNU trust writer，读取最终实际addon代码页和detached CodeDirectory，补齐缺项并核原GPU051文件/729jobs/4trust模块保持。013独立副本沿已编译012内核/HAL，只修正上述封包缺口，不重派Mac CI；正常系统触觉仍待真机。

013实机（2026-11-18）：native源码 `0e7c6ff9912cad29597a52e9c035c7e0dd0ccdb6`、Mac run44124303751/attempt1/headbc864f1，封包源码 `7f4d0ebb0488f43f9057aff0f699af3e4223f83f`；boot `5b25ef72b1634703d8cb6e4393356d9c26c44dfdab345aa97298a3014975b36c`、payload `f460e5adddc74a87ee7620c1947dcc93b126148bd455234bdcae025a6cdf62b3`。新缓存已加载，原 audiomxd114 在116.136s实际 GET_STATUS 成功/error0，确认 fileglob 只读命令修复。普通应用495执行 selection/impact/notification/CoreHaptics 和重复按钮，用户确认全部无振动；StartIO/Write0、kernel requests/plays0、engine4099。应用查询显示 Actuator35、VirtualAudio插件及Default/Actuator VAD0，但诊断 processID=495 是应用自身 HAL 副本，不能将它当作原服务的设备表。原 VirtualAudio bundle preflight成功也不能代替初始化完成。116s电气峰值为残留状态，不是本轮播放证据。一次正常XNU返回、Android/root/GPT/maps/17同135168B/27generation1、4984采集器实际退出与释放通过；自有应用/launcher/Core日志692224B保留于原013 run。

无人值守接续：按用户要求不提问，人工触感、按键和恢复测试后移。原013启动UUID `B0FC16CCF1BF45908606B59088E0407D` 已由保存timesync唯一关联；安卓只读窗口候选 `haptics013-logs-readonly113001` 已完成电脑资格并按113原队列等待，0boot/0write，最多32MiB/150s。只读取原两份Persist日志并核新鲜inode/extents/catalog UUID；实际读者退出、必要后验和释放后离线解析。当前仍需原服务日志确定VAD和固件就绪条件，不把静态条件或应用查询ID0提前认定为根因。

014 根因与修复（2026-11-18）：013 在本机同boot6045E813E837433A96CE5CF5226590BC复现全部按钮无振动。立即返回后取回同boot两Persist/一Special的750286条记录；原audiomxd117在111.291s发布可路由pact/Actuator端口，115.022s完成VirtualAudio初始化，却在115.322s报`Could not find haptics VAD. Aborting Haptic Server initialization`。同24A446 MobileGestalt的DeviceSupportsHaptics回调创建IODeviceTree迭代器，按`IORegistryEntryGetName`默认名称比较`haptics`。旧发布器只有`setName(name,gIODTPlane)`，name属性是OSData，`IORegistryEntry::init`仅从OSString初始化默认名，故默认返回类名`IORegistryEntry`。014按原IODTImportEntry命名方式，在attach前同时调用`setName(name)`和`setName(name,gIODTPlane)`；不改系统二进制或能力查询返回。生产发布器验证重现旧代码查找成功/默认名识别失败，并确认新代码保持ready/plugin/preset门槛和原字节属性。014目标构建及真实VAD/系统PCM/机械输出待验证；固件就绪条件在VAD真实建立后继续沿原接口处理。

015就绪通知适配：固定24A446原`AudioToolbox`在触觉VAD路由后按`IOServiceFirstMatch`等待`AHSFirmwareReadyNotifier`；GPU051/014严格38组件未含该类，原`kern.hv_vmm_present`由缺省`vmm-present`读取0。新`NezhaHapticsReady.cpp`以原168槽/0x88字节IOService子类提供本机执行器就绪通知，复用ALS/AES晚构造方式，只有原厂初始化、实际devfs端点、HAL和D16预设成功后由同一马达worker发布。休眠/退出/错误先同步退休通知；没有修改虚拟机标志或跳过原路由。原类start仅存64位毫秒`FirmwareReadyTimestamp`并registerService，通知消费者只检查真实类首匹配。就绪/名称/停止生产宿主用例通过，015目标编译和实机仍待。014固定boot`cbd7d9edbd06`/native1a72da2、Mac44275314321、strict/PAC1323/loader/单八核/六信任与本机75r008入口已通过并文件排队，旧013失效候选撤回；015不会改变014固定镜像。

后续源码对齐：原notifier的clock_get_uptime→absolutetime_to_nanoseconds→除1000000确定时间戳单位为毫秒。015编译源190a95f仍用微秒；当前原AudioToolbox只按真实类首匹配就绪，不读取该时间属性。后续源码已改毫秒并执行同生产用例；不能将新源码单位套用到固定015二进制。

015实际结果（2026-11-18）：唯一临时bootaa45df9f52ac/sessiona7690af33a7ff8eb，原VAD80/vzzz和HapticServer116.884s真实初始化，Core引擎/瞬态/连续/取消/停止API全通过。普通UI程序514自动及人工按钮调用，真实驱动play1/stop1/360mV25mA，随后283.263s报告EBUSY撤就绪；物理反馈仍未验。原613766条日志/21370匹配全部唯一时钟关联，预热141.352s第一次WriteMix失败后关闭流并连续NotRunning。正常XNU返回/安卓root/原payload/17同135168/27gen1及29640退出通过，704512B本方及14.37MB同boot服务日志完整取回，14:30:59UTC释放。

016按这次证据修PCM退休/休息：过期包只退休序号并丢弃，不补播放迟到效果；队列容量不足或500ms保护休息丢弃当期包而保留同一stream。HAL仅对ERANGE/EBUSY/EAGAIN恢复继续，真实IO错误仍关闭。原core保护停为IDLE不再统一当duration超限；真实cap或已回读停止的EBUSY进入500ms休息，不能以该分支隐藏未清PLAY_EN/未完成bus CMD。增加现有USB快照的PCM接收数/frames/nonzero/peak/lastError/leadUs/maxQueue和有限错误日志。生产队列与STOP/休息用例、实际Mac HAL及目标编译已通过，真机重复播放待验。源码包括已校正的毫秒FirmwareReadyTimestamp。

016 电脑终态（2026-11-19）：编译源 `389d369b4de4cdafda0582dbfcbd86b4630af725`；唯一 Mac44329367469/attempt1/head4be17e4869ac8483c3503e8cc6c9af99a481a6cb 成功，856项文件已取回解密核验。9触觉对象、就绪168槽、最终1491PAC、严格38组件零缺失/零stub、3原资源、6最终信任和全4KiB签页、实际loader、单八核QEMU、Windows75r008/14入口场景/183依赖全部通过。boot `cdd80b2785a57712603913e106d7099c47131880c22a1afdc7ffffa95bb67ea8`，embedded payload `a8fca5788a03d133a3cc08b602a2981d11d317fc68ec26f1662205d2a746c7b2`。硬件2V/500ms保护和停止保持；源码状态记录的修正不改变这份固定编译产物。016尚未上机，正常重复输出、机械反馈、锁屏唤醒和稳定性不记通过。

017 源码接续：016同一次启动已由用户确认“短连续触觉每次都能振动”，快照plays55/stops55、ready1/error0且流保持；三项UIKit按钮仍无振动。原UIKit按绝对系统运行时间发送事件，随后反馈引擎完成回调延后数十秒；原24A446 CoreAudio三API仍按24MHz换算，实际XNU时基625/12（19.2MHz），形成时基差异。复用nezha音频072@7c065c8的精确donor时钟修正和原osfmk task accessor，HALInitialize在流启动前核频率及两个方向一秒换算，必要时通过无输入数据的专用ioctl修正。十一内核对象/新的HAL目标编译与017真机待；原厂2V/500ms/STOP保持，不用提高幅度隐藏短反馈失败。

018 源码补齐原渲染时基：017 CoreAudio 三API校正仍未覆盖 AudioToolbox 内联的 host ticks→seconds。原 ServerManager::RenderProc 使用 1/24MHz 乘本机19.2MHz计数，再据此比较 UIKit 绝对事件时间；立即0不受该条件延后。当前沿017原 Pin、UUID/slide/指令核对及原子常量发布入口，补原 AudioToolbox 的频率及倒数两常量，先核完整原映像再写。HAL在本机19.2MHz初始化时总是调用该入口，避免已校正CoreAudio时遗漏第二映像。原ARM64渲染+实际queue比较重放已证明旧值延后、新值准时允许，生产准入/发布ASanUBSan通过；018目标/实机待，017固定包仍单独等待队列验证，不能套用新源码。

019 包络复用：018自动选择和轻触起振时分别只有31和22permille，将Apple低幅度合成PCM按满刻度比例送进本机LRA使反馈几乎不可觉察。复用HakiOS redwood@f128f1d8的gain_for算法策略，把非静音包络归一化为本机已验证驱动的5/8到1，在1/3PCM刻度达到原2V幅度；静音阈值128、2ms分段、波形持续时间、序号/取消、500ms上限和休息不变。仅重写归一化算术，不导入AW86927寄存器、Android增益值、升压、电压或校准。019实际强度与停止待后续同四项普通请求验证。
