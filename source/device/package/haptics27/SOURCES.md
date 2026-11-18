# 固定来源与复用范围

源码提交留本地；人类已指定GPU051测试，现已完成独立目标编译与电脑验证，等待真机窗口。Mac专属构建按既有例外只推公开源码与公钥、CMS加密取回；没有上传私有Apple/vendor/设备/聊天输入。

| 硬件／功能 | 固定来源与入口 | 来源已验证的范围 | 本次复用与差异 |
| --- | --- | --- | --- |
| HAP530 启停、FIFO、限值、停止 | `IssacL04/UniXNU-Pagani-Drivers@c888efd3ce1b817d13f28783a77cf689cb2255c9`，`drivers/PaganiHaptics/HapticsCore.c`、`HapticsSpmi.c` | 来源有主机模型与识别/初始化记录，播放验收仍待；不能称其系统触觉已实测 | 沿用核心、寄存器范围与故障退出；增加原厂状态、v8 transport、停止回读；保留其 BSD-2-Clause-Patent 许可和 runtime 符号 |
| SM8850 SPMI v8 | Qualcomm 下游 `OnePlusOSS/android_kernel_oneplus_sm8850@fc30e54174d254ff7f33622a9278e4435f6718d2`，`drivers/spmi/spmi-pmic-arb.c`；nezha 原 keys/ADC `04e0a9b18935d52b21b02f34563f5d98eb3a5511` | keys 与 ADC 原本机有实测；本次bus1身份/读写与007受控实际振动及完全停止已实测 | 公共层新增独立 BSD C 实现；global APID、bus-local owner、bit31 IRQ owner、shared bus lock、pending CMD；没有复制 Linux 函数体 |
| 本机原厂参数/校准 | N0 OS4 live FDT、vendor_boot_b 中 `qcom-hv-haptics.ko` SHA `159cfe080f10…`；`MiCode/Xiaomi_Kernel_OpenSource@45705be1220b4cfa8100516ad86711656c0b634e`，`drivers/input/misc/qcom-hv-haptics.c` | 旧安卓日志已运行2000mV direct与24kHz FIFO，校准周期5723µs/标称9Ω；这是安卓证据 | 按本机读取现有周期/RC结果，保留原校准；不执行原厂主动校准与 SDAM/PBS 写。Linux 源 GPL-2.0-only 仅作为协议核对，未复制到本次 BSD 源 |
| Actuator HAL、2ms 包络 | `FoXLinne/UniXNU_marble@7a0d68fff2557f12be9f8c47f618fd332b7a3e67`，`hal/MarbleHapticsPlugIn.c`、`package/MarbleHapticsRuntime.cpp` | 记录确认一次101ms硬件播放；该记录明确系统桥当时不存在，后续源码不等于实测 | 复用 HAL property / char-device 架构，新增独立 Actuator，不发布其 Speaker/Codec/Hawking；补 session/epoch/seq/取消回执和不续期限幅度更新；BSD-2-Clause-Patent |
| 系统触觉到波形 | `JopangJovan/hackios-xnu-userland-umi@aa843bae3672a49dafa5b82226fdcb30844fa6ff`，`userland/src/audio_plugin.c`；kernel `4a9f35d998a8217009f6d96fe7288f411fb7f71c`，`hk_haptic.c` | AW8697 路线源码和测试任务；不能用于本机硬件证明 | 用于路径对照，未复制 I²C、GPIO或 AW 参数；用户态 MIT、kernel APSL-2.0 |
| iOS27 donor | iPhone14,4 / D16 /24A446 原输入 | 原 `/product/haptics` 两能力属性；D16 preset、AID8001 actuator_general、VirtualAudio 原入口与等待代码存在 | 原 byte property 编码；未改 Apple 二进制、arm64e 私有 vtable 或 PAC slot；HAL编译后再核 SDK/PAC/import/sign/trust |
| GPU051原ADT身份补齐 | nezha `tools/audio27-product-prepare.py@825a746`，音频060/063已用的原ADT读者和UniXNU encoder | 原工具完整核对product-id/AcousticID和所有其他节点；原011 audiomxd有null product-id故障 | `scripts/prepare-haptics27-system-identity.py`复用工具；两属性从本地固定profile/donor取得，包装阶段执行，系统触觉效果仍需真机 |
| 最终系统文件信任 | 原011系统信任追加、UniXNU `scripts/trust-append-module.py`及nezha音频`record_runtime.py::verify_signature` | 011实际启动任务缓存和普通应用通过；012漏追加失败原证据保留 | `scripts/stage-haptics27-system-trust.py`读取最终实际六项CodeDirectory并核各4KiB代码页，补齐独立工程模块，原签名/AMFI规则保持 |
| Format2 接入 | `UniXNU/UniXNU@9931232fd68b8ae1db0abd957ecba417f1996e52` 文档/脚本；myron-next `f1a1bfe`；sheng27 `5b7d83c6` | 原框架流程；无现成 nezha HAP530+27端到端验收 | 按 source/SoC/OS/device 所有权交付；archive、caller、addon与封包在指定包后执行 |

pandora 当前 `d2c7089b`、u0er nezha `ed8978ac`、fairlady、cas/umi、myron-next均检索过。pandora fork aogg 的 `89f57e4e5e11eb72b68b0a9e23ff8e627805c804` 只有 HV-Haptics只读调查，bus1写通道与系统桥未完成，不采用其旧板参数。公开 Linux 主线 input/misc 有 pm8xxx-vibrator、cs40l50等，但没有当前所需 HAP530 qcom-hv-haptics；Linux-next直接路径也未提供。lore本轮403，邮件正文未取得。

QQ 使用人类指定 `M:/QQ_History`，本次档案135181条、2026-10-22至11-15；地图16主题、无全库overview；按中英文与芯片/服务词搜索，47条直接触觉命中，展开18处上下文、核3张关键图片。有效线索是“硬件启停与音频建图分开”和 marble/Pagani源码入口；“已修好”的发言没有替代包/日志验收。私聊、成员、原图、设备输入与完整定位留本地 evidence。

本地原始证据：`/mnt/i/Code/UniXNU-nezha/evidence/haptics27/20261116/`。新源只保存事实/协议派生常量，不含私有 firmware、DTB、校准转储、聊天原文或身份。

007协议修正继续核同一OS4模块与MiCode固定源：`haptics_runtime_resume/haptics_visense_enable`对应PTN0x68/69通道恢复，`haptics_init_visense_config`对应CFG0xc7峰值模式，`haptics_load_constant_effect`对应direct自动谐振，原9Ω headroom规则对应2000mV时500mV余量，`haptics_wait_brake_complete/haptics_toggle_module_enable/haptics_suspend_config`对应停止恢复与最终module-off。VSENSE原值乘50/64得到mV，ISENSE原值乘1000/4096得到mA。原厂GPIO/OTP/SDAM/校准和保护阈值不作试探写入；芯片峰值不能证明机械位移。

系统路径追踪补充（2026-11-17）：008程序被Data执行策略拒绝，009改readonly/usr/libexec后真实执行，supportsHaptics1但CoreHaptics engine error4099，无HAL请求/马达播放。原24A446 AudioToolbox AVHapticServer.mm确认com.apple.audio.hapticd，由原audiomxd服务缓存发布；初始化需要haptics VAD真实路由到Actuator与AHSFirmwareReadyNotifier的firstmatch。此为原binary静态调用及实际错误，不把能力节点或启动注册冒称正常系统链通过。后续UI普通App从Sensor002/Camera成熟LS注册接口复用，010实际前台应用和按钮调用已验，人类无振动；engine4099、requests0。

011接口核对：原24A446 HALS_PlugInManager::LoadPlugIns（CoreAudio UUID895a68d237523e149007b07e705a2178）确实扫描/Library/Audio/Plug-Ins/HAL、/System/Library/Audio/Plug-Ins/HAL及/Library/Apple/Audio/Plug-Ins/HAL，筛.driver。原公开AudioServerPlugInCustomPropertyInfo仅支持None、CFString、CFPropertyList跨进程类型，010未登记的原始u32诊断数组不满足此契约，011改登记CFPropertyList。Initialize探测现有端点和成熟可选属性复用同固定PaganiAudio源；system HAL union复用GPU051及音频已用挂载接口。新静态证据保存在本地service010/loader-and-property011.json；没有移植21H461/J120私有修补。

012状态ioctl核对：011原audiomxd106.033s明确Autobox file-ioctl拒绝，XNU sys_generic::ioctl先调用mac_file_check_ioctl而后才达现有vnode_hook。补齐fileglob层只对EPERM、DTYPE_VNODE、原allowed进程和同一NZ_HAPTICS_GET_STATUS精确节点应用既有nezha_haptics_ioctl predicate；49个实际条件分支ASan/UBSan用例及原XNU补丁应用检查通过。未放宽其他文件/ioctl或普通应用直接驱动权限。

015执行器就绪服务：同本机24A446 AppleHapticsSupportLEAP原AHSFirmwareReadyNotifier(start0xfffffff008becf70，168slot/136B)与AudioToolbox首匹配调用只读核定。复用本包ALS/AES晚构造与IOService正常注册/退休方式，本机只有实际HAP530端点与已安装HAL/预设成功后发布；不复制Apple指令或固件。原FirmwareReadyTimestamp为毫秒，新源码校正，015编译版单位差异单独记录。
