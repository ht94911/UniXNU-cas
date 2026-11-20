# 067 receiver and speaker routing on the accepted066 baseline

The066 phone results remain the recording, voice-uplink and ordinary-SMS
baseline. The067 candidate brings the board's factory receiver/stereo programs
from8a93a3b onto that first-input recovery implementation. Receiver uses only
the top amplifier with the factoryvoice profile and RCV0; speaker uses both
amplifiers with music_18dB_stereo and RCV1. DSP mode acknowledgement, muted
switching, running VI protection and microphone DMA lifetime stay enforced.
The current runtime/engine/graph/TAS route suite passes including a first-read
timeout and complete close/unmap/reopen during receiver selection. The related
idle/restart, runtime, capture-deadline and shutdown suites pass ASan/UBSan.

The failed064 Codec interface accepted only a scalar for the device'sssrc
property. The original24A446 caller passes an array, and the public CoreAudio
contract agrees. The067 interface keeps the available sourceimic independent
of the selected array, accepts empty deselection, rejects unsupported sources,
and supplies the actualmoni item kind through both translation and selector
queries. HasProperty probes no longer require an item qualifier. Mono capture,
stereo output and the final accepted-PCM reference stream remain explicit.

The original N71 constructor hardcodes its VAD graph map. The064 VAD plist
could not replace those entries, so speaker_ringtone still loaded the iPhone
speaker_general graph with its absent four-channel telemetry bus. The067
compatibility handoff uses the original VAD path builder, CFDictionary parser
and map move to install the board's stereo/mono entries into that existing map.
All31 original map keys are retained, including the original actuator graph.
The32 accepted cellular/ringing entries are retained and the8 config entries
fit the40-entry native applicator. The104-byte source-assembled handoff occupies
only exact checked BRK padding in the same24A446 VirtualAudio image; no private
Apple binary is included in the source export. Target/HAL SDK compilation,
full packaging and phone acceptance of067 are still pending.

Latest reference fetches: UniXNU8542980, myron-nextf1a1bfe, shengc8f92b2,
Pagania84204e and fairladyfb06775. No equivalent item/graph handoff was found
in those trees, cas or umi. The HAL architecture remains sheng4601795; the
hardware programs are from thisnezha's factory tables and retain the earlier
Pandora0301 silent-output/VI lifetime design. No foreign calibration is used.

# 066 first microphone recovery candidate

The exact061 image succeeded on one boot and failed on another: first four
input READs timed out, then STOP returned zero-length completions. The input
CLOSE/UNMAP and codec release completed, but the graph error kept every later
StartInput from recovering. The66 candidate permits one renewed recording
request after that first no-PCM timeout, only after all original input DMA and
endpoint resources are released. It retains the first error in the existing
input-flow diagnostic. Remote/invalid replies, cleanup failures, a second
failure, or any failure after real PCM remain terminal. The061 HAL, native
hardware settings and65 browser compatibility stay in the candidate.
Actual runtime/engine/graph/TAS host cases and related runtime, endpoint and
shutdown suites passed ASan/UBSan. Native066b target compilation and the actual
first-input recovery on the phone passed. The phone is released.

Native066 compiled all50 audio units and produced the exact accepted061 HAL,
but its newly enabled host check still expected the abandoned BuiltIn/Speaker
interface. The066b check instead verifies the accepted USB transport and
NezhaAudio27Device UID, keeping its stream, tap, PCM and disconnect assertions.
This correction changes only the host check; the HAL and recovery stay unchanged.

The066 image passed on the phone: its first input timed out, completed the
original input/codec shutdown, reopened once and then produced PCM. The user
confirmed both recordings and voice uplink; ordinary SMS also passed both ways.
Existing diagnostics retain the first timeout while later input graphs stay
healthy. The phone returned to Android and passed protection checks before the
window was released. Receiver/speaker switching remains a separate next change.

# Audio102 reuse for baseband045

The accepted microphone and speaker implementation comes from audio102
`ab45b24d583109bc346077afbd1696a8bcb717e5`. Its existing HAL, PCM engine,
DSP graph, codec setup, repeated-recording lifecycle and public source notices
are retained. `integration/baseband045-reuse.json` records the exact imported
files and interface selection. Private firmware and calibration remain local.

This source joins audio102 to the human-selected GPU024/baseband044 source.
The current44 image and its ongoing longer trial are retained. The combined
source completed Native047 and local packaging; combined device results remain
pending.

One baseband shared archive defines Qcom transports and the SoC resources.
Audio builds50 other translation units against the actual sharedQcom headers
and stagedSoC header. The originalRpmh resource gains auxiliary fields, so a
matching function name alone is not an ABI check. The existingAudioService
static initializer uses the same `nezha_macho_late.py` as the baseband.

Audio keeps diagnostic8 and ADSP lease8, TAS/GENI lease9 and microphone lease10.
MSS keeps diagnostic9 and uses its separate SMMU lease11 after047 exposed the
TAS9 conflict; diagnostic IDs are independent of resource ledger IDs.
Actualhardware banks/audio7+streams9-12 and IPA8+streams13-14 stay unchanged.
The originalaudio AOP0 route is preserved when already registered. The
existingGNSS sharedIPCC append implementation allows laterMSS registration
and copies route state under the commonlock before invoking its callback.

Localchecks passed:152sourcefiles/205syntaxviews/203publicfunctions,
originalTCSfreshcompletion/restore, actualADSP-first/MSS-first route and
lease8/9 separation, originalbasebandlate-start, and existingbasebandpower,
SMMU/SCM wrapper checks. The referenceAudio ADSP contract with currentB
sharedcore passes4184checks. Artifacts remain in localresearch.

The orderedarchive recipe and link include the audio archive. The original
explicitna=3 fan-out and HALunion mount select it; no automaticrecordingjob
or privateblob is added. Localpayload inputs and targetcompile/signatures,
PAC/strictclosure/onecommonowner checks are still required. IMSregistration,
normalOSPCM, ordinarySMSdelivery and two-wayvoice remain separate checks.

Native04543946907759/CI896e129 ended in sourcepatch application before
compilation. The importedHAL patch expectednative_cryptex immediatelyafter
the audio mount, while currentcellular service mounts are inserted there.
The repairedpatch binds to that actualcellular comment and retains the
identicalna3/NezhaAudio27 union behavior. Originalencrypted13file diagnostics
are kept; localactualaudio+cellular context application passed. Newbuildpending.

Repairedsource f245bb011fe37962caeba2238f88ecc6fb0ab825 was reconstructed
and staged79units (29shared/baseband,50audio). Native04643948646759/attempt1
CI c4adc2782b491dc7128ca3e439faf7d5f79cd44b is following the sameCMSflow.
Only the HAL patch changed from045inputs;046 later failed as recorded below.

Native04643948646759 completed with failure; its447 encrypted files are
retained. All50 audio objects and the signed HAL were generated, but the
archive report failed because the shared-component metadata dictionary was
overwritten by the set of excluded source names. Keep that dictionary and
name the source exclusion set separately. The selected50 sources and compiler
arguments are unchanged; a new complete target build remains required.

Local packaging now pins the actual selected addon/trust SHA values instead
of requiring the older GPU023 pair. A preview with the accepted102 HAL retained
all10 current044 addon members and all4 trust modules, appending3 HAL files and
one module. It is a local preview, not the next target artifact. The exact102
audio input finalizer was imported to fill the owned34MiB slot before strict
prelink/HMAC; its63-file firmware/calibration input pack remains private.

Native04743952592746/attempt1/head3f4dc736 succeeded621 encrypted files.
Compiled source fd50b47233af83ce1860967c8204370fba77c172 produced29
shared/baseband and50audio objects.336 native imports resolve,53HAL imports
resolve in the original iPhone27 cache, and the signed HAL is byte-identical to
accepted102. Both late initializers and every single common owner are checked.
Strict38components/275 original branches,1062baseband and382audio PAC slots,
GPU/Legacy/HID tables,sixIOSurface routines and finalHMAC pass. Actual single
and eightCPU computer controls reach the protected missing-map refusal.

The private63-file audio pack fills only the owned34MiB slot. The complete
payload is144MiB and cannot fit the existing112MiB pinnedpayload. Reuse the
accepted full temporary boot path with the exact044FD; keep the existing disk
payload and17menu unchanged. Local donor product-id/AcousticID8001 are added
while preserving mav21 and all16cellular fixups.983B arguments selectna3 and
systeminput1, retain zero captureprobe default and observeCallAudioService.

Final kernel53328b198c89/boot744576ad548a/payload23d780a45231 are deployed
on both computers with75host files checked. The new actual temporary start,
read-only diskpayload postflight and non-replay cases passed on Linux/Windows.
Original044 full native analysis completed: primaryVoice/Sms registration held
813.236480s until normalshutdown; cellular defaultTLS204passed. No network
updates or3000s renewal were observed.047 then booted once on the localhost
and failed beforeADSP: TAS identity claim waited5000ms because theMSSSMMU
already owned ledger9. Human remained atAppleLogo. One normalreturn and
Android/root/maps-pin/same135168Data/gen1/collector10984exit released the
window06:19:08UTC. Diskpayload was notwritten. Original047logs stayqueued.
048 moves only MSSSMMU to11, preserving originalaudio102 hardware/leases.
The actual TAS claim/release and identity sequencer plus nativeIPA/SMMUbinding
checks passed with audio8/9/10 held. Targetbuild andhardware remainpending;
ordinarySMS delivery and two-wayvoice remainunverified.
# 049：保留基带共享中断的暂忙返回（2026-11-17）

直接复用的生产音频基线仍是102/ab45b24。原音频7交付和人类本轮确认均以
102为扬声器、麦克风与录放基线；109是额外有限自动录音验证。ADSP/PAS固件、
WCD/GENI、输入输出图、HAL和原102回收流程保持，没有另建DSP或HAL。

048已一次实机启动：85.240055秒identity开始，85.299080秒phase4/error0，
两功放revision48/48，59.025ms完成，047的TAS9五秒租约冲突已消失。
随后PAS1/36停止、SMMU准备、DTB认证和PAS1内存准备返回0，但86.143664秒
driver6/ADSP10/error-8，GPR、服务和HAL尚未上线。主卡原AVC初始化、AKA/IPsec
注册及默认TLS蜂窝HTTPS同时已通过；完整日志分析结束，手机正常返回保护释放。

真实共用Sm8850Resources的分发器在基带已经处理FIFO时返回Sm8850Busy。
旧AudioResourceOwner把它经fail()写入owner.error=NA_BUSY；当前poll虽返回暂忙，
下一次却先被owner.error拒绝为NA_OWNERSHIP(-8)。原102独立包只有音频消费，
合包引入了这一条实际并发路径。49只使此返回沿原na_adsp_poll的NA_PENDING
等待，不写永久错误；真I/O错误仍保留。真实旧函数复现，修正后两次暂忙不清FIFO，
释放后事件继续、两种启动顺序及ADSP8/TAS9/mic10/MSS11租约测试通过ASan/UBSan。
原102 PAS同步路由/CPU alias/租约边界也通过。048未保存首个owner内部错误，
故此源码缺陷与实机-8的一致性已建立，物理原因仍需49新原生包核验。
# 050：复用102/Pandora0301静音保图覆盖冷空闲

049已实机证明共用中断暂忙修正有效，PAS1认证返回0，ADSP9/driver5/error0，
服务和102HAL/input成功发布。系统没有Start/PCM请求，三个图仍Off；随后
原SMEM423新fatal明确Island SCID12未配置，进入NA_REMOTE(-4)。
这一故障音频080/102已有pandora0301来源和真实保图方案；目前只在首个
输出/输入请求后保持输出/VI。合包Logo阻塞期间无首次请求，暴露冷空闲缺口。
050只在生产input_ready接入的原Assembly初始化后设置runtime.output_held，
原runtime/engine已为output_idle静音，输入请求与probe保持0；原功放Standby
及VI保护、客户端首次Start/Stop、CLOSE/UNMAP/normalshutdown流程全部复用。
实际生产状态机与native adapter检查冷空闲运行图/全零PCM/未激活功放/输入0、
随后客户端输出与停流/关机、原partial setup失败通过ASan/UBSan。
050实际原生44062594298成功，准备函数4模式arm64e执行验证原生产hold1、其他hold0及错误0；HAL102和共享归档与49字节一致。严格封包及单八核控制通过，11:02UTC本机完成就绪排队，实机仍待，不称已修桌面或通话。

# 061：首次双向输入沿已验102静音输出启动

060等TAS激活后开麦克风已在真机执行，但四次READ仍两秒超时、STOP只返
零字节，不能再将TAS先后认作完整根因。053人工普通去电双向通过；第一次
输入在Alerting阶段即产PCM，后续活动通话复用原输入图。102跨boot录音和
109有限系统录音在静音输出/VI状态下首次采集成功，45项已读硬件值与060同。

061在首次实际输入请求且本epoch尚无PCM时，只在worker本地将输出置为
静音；共享输出请求保持。沿原input-only gate完成板级准备、采集和真实
非空PCM回执后恢复输出。原input超时及回收照旧，失败输入不会持续阻塞
健康输出。不改硬件值、HAL或自动启录，不以电脑检查代替普通来电上行。
实际runtime/engine/graph/TAS测试覆盖铃声Stop→首次双向、真实PCM解锁、
四READ超时后的输出恢复；旧runtime在新场景失败，新场景和原场景通过。
