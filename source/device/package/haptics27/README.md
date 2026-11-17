# nezha iOS27 触觉源码候选

**人类已指定 GPU051；当前007马达候选已完成目标构建、严格预链接、封包及113入口验证，等待文件窗口。007实机已经确认振动与完全停止，系统触觉尚未上机。** 第一阶段交付保持不变；此测试树从 GPU051 固定源码76c595268304创建。最新已编译的内核源码为c7d3ba749849fc2ab46840d275957c50bfbb914f；后续仅文档提交不改变该构建身份。依赖见 `dependencies.json`，复用与许可证见 `SOURCES.md`。

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

实际轮次：001在SWR_ACCESS判定处拒绝，003在越界period处拒绝；均未驱动。005初始化通过，两个50ms/25%及两个300ms/50%仅命令与停止计数通过，用户明确反馈没有振动；第三300ms脉冲HPWR停止超时，正常USB退出被拒绝。随后人工回安卓，实际root/GPT/maps/17/27前缀后验和自有采集器退出通过并释放。007据此修采样、自动谐振和停止链，尚未启动。功能验收仍为：总线/身份已测；机械振动未通过；HAL/UI/CoreHaptics/长期稳定性未测。

007本机实际通过（2026-11-17）：原厂5726µs、FIFO8064；25%/50ms后峰值502mV/38mA，50%/300ms播放中997mV/84mA，全幅300ms播放中1984mV/183mA。人类确认“有震动！我感受到了！”及“已完全停止”。3次100ms重复与6个FIFO效果命令/停止过，六效果实际触感未逐个验。取消有实际stop13/logerror0，但USB电气缓存仍play129/power3；新源在STOP后强制回读、为采样增加valid/sequence/sampledus并在HAL客户端期间采样，生产函数模型ASan/UBSan通过。一次正常XNU返回、Android/root/GPT/map-pin/17同135168/27generation1/52204退出与文件释放通过；未写手机payload。008系统候选在电脑构建，不能沿用旧006验收。

008系统实测（2026-11-17）：productstage2/addon5-error0/D16preset1/commit0已通过；有限CoreHaptics程序在原Data路径被SystemPolicy拒绝，plays0/stops0，结果缺失、stdout/err为0。初始V/I峰值是上轮Android残留而非本轮输出。一次正常返回、Android/root/GPT/maps/17同135168/27gen1/78952退出后验与释放通过。009复用音频105和传感器002已有readonly `/usr/libexec` union，为同一签名Core程序发布系统执行路径；不修改全局执行策略。
