# nezha iOS27 触觉源码候选

**人类已指定 GPU051。007 的直接马达振动、完全停止和正常返回实机通过；010 的普通前台应用已执行，但所有按钮无振动，CoreHaptics engine4099、HAL设备ID0、内核requests/plays0。当前011在独立测试树修复自定义属性传输并核对原HAL加载；系统触觉尚未通过。** 编译与实机证据按包身份绑定，旧失败轮次保留。复用与许可证见 `SOURCES.md`，依赖见 `dependencies.json`。

真机已确认 PMIH010x / HAP530、SPMI v8 bus1 SID7，CFG/PTN/HBOOST 分别0xf000/0xf100/0xf200，global APID2254/2255/2256；CFG5.1.8.1、PTN5.1.0.0、HBOOST31/01。马达按原厂LRA、160–180Hz范围，名义5555µs；旧安卓日志5723µs，005本轮读取有效5746µs、RC mode4/count0及8064字节FIFO。9Ω为原厂标称阻抗；具体马达商品型号与当前温度尚未确认。

当前实现包含：

- 原厂波形/谐振/RC状态入口、bus1 APID/EE核对、共享SPMI锁及不确定命令保留。
- 原范围2000mV上限、幅度1–1000、≤500ms受控短脉冲、FIFO原六效果/九primitive逻辑及可靠停止回读；原custom8500mV不作为本候选电压。
- 48kHz单声道Actuator HAL；40字节包头、session/epoch/sequence、Mach时间戳；2ms包络映射到本机LRA载波。持续触觉最多500ms后休息500ms，幅度更新不续期限。
- 完成、取消、旧会话拒绝、客户端退出、静音样本、流耗尽、休眠/正常重启退出；旧请求在硬件事务期间取消后，先退休事务再停止。STOP成功回执需要硬件PLAY_EN清除与HPWR停止，超时不当作完成。
- 007复用原厂HAP530运行态V/I通道恢复、direct自动谐振、2000mV时500mV headroom，以及SWR-aware停止恢复。诊断暴露真实PLAY_EN/HPWR/source、VSENSE/ISENSE原始值和原厂单位换算；这些仅是电气证据，不自动宣称机械振动通过。
- `/product/haptics`字节能力节点，只有Actuator插件、实际执行器和原D16预设都可见时发布；原iOS用户设置、铃声/通知策略和CoreHaptics合成仍走系统栈。

`nezha-haptics`源码开关：缺省0，1只读probe，2配置/停止后退出而不驱动，3发布执行器；没有启动测试振动。`nezha-haptics-ios`缺省0，1基础触觉能力、2带原D16合成预设。007使用 `nezha-haptics=3 nezha-haptics-ios=0`。旧006系统包基于此前源码，不能作为007代码的系统验证；007马达物理输出及停止通过后再封008系统包。CoreHaptics瞬态/连续与selection/impact/通知接入的源码链存在，实际请求、系统回调与触感未测。

能力限制：包络保留强弱/时长/间隔，sharpness仅对应原厂固定载波，不能称任意频率与Apple闭环效果等价。PWLE频率曲线、SoundWire/ADSP触觉、长铃声与精细组合未验；不对未知寄存器、OTP、NVM、持久校准或原厂保护阈值做写入。元数据属性/queued不能当作系统触觉成功。

GPU051集成与验证路径：

1. 从人类指定的固定源码/镜像创建独立副本。以其framework、SoC、OS、Data与已验模块为基线，应用公共SPMI、组件、设备三个增量。
2. 将 `Sm8850SpmiV8.c`、组件 `HapticsSpmi.c/HapticsCore.c` 和本目录五个内核源加入现有archive recipe，复用本包XNU compile records、arm64e flags、生成头文件/32线程。组件原v7 glue和OnePlus参数不加入nezha。
3. 将 `nezha_haptics_start`接到资源就绪的正常线程caller；将 `nezha_haptics_quiesce`接到卸载卷之前的正常关机与设备退出。不能只靠通知queued或PSCI兜底证明马达停止。kernel源无新IOService子类或私有vtable，无自定义静态constructor。
4. 用现有Mac HAL recipe构建独立 `NezhaHaptics27.driver`，4096页签名/严格trust/预链接；加精确 `/dev/nezha-haptics27`访问权限，不扩大其他设备。原D16/24A446调音资产和VirtualAudio留本地，沿指定包已有addon/union挂载路径。启用能力节点前检查插件、预设、caller真实已装载；具体权限与启动补丁在指定包上导出。
5. 执行组件原Test、追加factory cases、公共层bus1 cases、本目录queue cases，再做实际目标构建/PAC/KPI/import严格核对。静态语法检查不替代它们。
6. 真机先probe与50ms/25%短脉冲（显式工具编译命名NezhaHapticTest，避免内核proc_name长度截断），回读停止及原保护；再同次验证六效果、快速重复/取消、UISelection/impact/通知、CoreHaptics瞬态/连续、锁屏唤醒及音频/GPU并发。对齐请求序号、启动、停止、完成与人工触感。数据代际、原根卷和安卓保护均沿当时窗口最新事实。

回退：独立测试副本撤掉haptics archive/caller、HAL/addon增量和两个开关，恢复指定包原身份；不删除原厂输入，不重建Data，不变动17主包/安卓/root。最终合入由原主聊天审查。

2026-11-17电脑结果：最新Mac原生43933783541/head042e9e7205ce4132042c53ead10ce7da3fa1b193成功；native279496b76bb3d6bc97e6ea11444cc1799429b638da271f6b71de3ddf147a5231。核心98857、SPMI14615检查及ASan/UBSan通过；38组件strict零缺失/零stub，1323原有PAC槽与3原资源区复核；loader真实parser、单核/8核QEMU通过；113部署镜像与75文件r008已核。007 boot31dda3c7ad6d51aa3da2d04c2c1f17294b83a35ac7d07f0495e4448f51359a15，embedded payload306fb898dd97760d097ea2f013bc05eb5b9180b7c51592b05f2f3015f7271d46。原729jobs/56addon/4trust模块保留，007不写手机磁盘payload。

实际轮次：001在SWR_ACCESS判定处拒绝，003在越界period处拒绝；均未驱动。005初始化通过，两个50ms/25%及两个300ms/50%仅命令与停止计数通过，用户明确反馈没有振动；第三300ms脉冲HPWR停止超时，正常USB退出被拒绝。随后人工回安卓，实际root/GPT/maps/17/27前缀后验和自有采集器退出通过并释放。007据此修采样、自动谐振和停止链，尚未启动。该005轮次验收为：总线/身份已测，机械振动失败；后续007实际结果见下段。

007本机实际通过（2026-11-17）：原厂5726µs、FIFO8064；25%/50ms后峰值502mV/38mA，50%/300ms播放中997mV/84mA，全幅300ms播放中1984mV/183mA。人类确认“有震动！我感受到了！”及“已完全停止”。3次100ms重复与6个FIFO效果命令/停止过，六效果实际触感未逐个验。取消有实际stop13/logerror0，但USB电气缓存仍play129/power3；新源在STOP后强制回读、为采样增加valid/sequence/sampledus并在HAL客户端期间采样，生产函数模型ASan/UBSan通过。一次正常XNU返回、Android/root/GPT/map-pin/17同135168/27generation1/52204退出与文件释放通过；未写手机payload。008系统候选在电脑构建，不能沿用旧006验收。

008系统实测（2026-11-17）：productstage2/addon5-error0/D16preset1/commit0已通过；有限CoreHaptics程序在原Data路径被SystemPolicy拒绝，plays0/stops0，结果缺失、stdout/err为0。初始V/I峰值是上轮Android残留而非本轮输出。一次正常返回、Android/root/GPT/maps/17同135168/27gen1/78952退出后验与释放通过。009复用音频105和传感器002已有readonly `/usr/libexec` union，为同一签名Core程序发布系统执行路径；不修改全局执行策略。

009/010系统实测（2026-11-17）：009有限程序从readonly/usr/libexec成功执行，supportsHaptics1但engine4099、requests/plays0。010普通应用原LSproxy installed1/placeholder0、open1，actual foreground278.145s；selection/medium impact50%/notification及重复按钮均已调用，人类明确反馈“实测所有按钮均无震动”。CoreHaptics engine持续4099，原CoreAudio查询Actuator与VirtualAudioDevice_Actuator都device0且无kernel HALopen/start。010正常XNU返回、root/GPT/map-pin/17同135168/27generation1/79064退出、48MiB内自有日志提取与释放通过。

011根据此现场补齐：原loader已静态核三目录及.driver后缀。沿成熟/system/Library/Audio/Plug-Ins/HAL readonly union发布同一独立Actuator，保留原BuiltinAudioPlugin。HALInitialize只开/ioctl/close现有端点取就绪状态，不发播放；nhpt按AudioServerPlugInCustomPropertyInfo登记为CFPropertyList，供原CoreAudio跨进程传输；补成熟Actuator可选属性，记录末次未知selector。应用同时核uidd/duid/pibi，launcher在有限deadline内记录目录枚举和CFBundlePreflightExecutable。硬件电压/波形/校准、正常系统策略和FIFO执行器保持007沿用；待新包实际加载及正常请求。

011系统实测：原audiomxd(118)在106.032s初始化HAL且open内核endpoint成功；launcher系统HAL目录枚举expected1、CFBundlePreflight1/error0。前台应用277.495s原CoreAudio得到plugin34、Actuator35，registerednhpt CFPropertyList传递成功；VirtualAudioDevice_Actuator仍0，StartIO/Write0、Coreengine4099、kernelrequests/plays0。人类明确“四项点击后依然都没有震动”。drte最后未知属性经同原24A446音频062证据核为HasProperty gated可选项，无删除/拒绝设备分支，不作为根因或猜测返回值。正常返回、保护/root/17同135168/27gen1/77328退出、708608字节自有日志读取和release通过。后续核原VirtualAudio加载、初始化和ActuatorVAD创建。

012补齐011已观测的Autobox状态ioctl拒绝：原fileglob检查先于vnode检查，按既有原进程/精确devfs节点和唯一只读GET_STATUS恢复访问。原VirtualAudio建立VAD仍待证；请求原011持久日志的Android只读文件窗口，获取后释放，不为该调查重启iOS。

012诊断另记录HAL属性返回进程PID、原VirtualAudio插件和Default VAD查询、原`vain`四字节初始化状态及原VirtualAudio.bundle预检。`vain`只读契约由同24A446的VirtualAudio消费者与MediaExperience核定，不写初始化状态，不改变原路由和固件就绪条件。普通应用读到的HAL副本PID用于与原audiomxd区分。此轮尚未编译或真机验证。
