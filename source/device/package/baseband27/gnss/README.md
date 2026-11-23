GNSS045 protected result and GNSS046 scan notification (2026-11-23 Beijing).
The fixed Native029 candidate has completed its trial and bounded log read.
Six original files contain770457 records; the selected boot has104 GPS and89
WiFi fusion inputs. One of App496's189 ordinary full-accuracy callbacks matches
GPS by timestamp (0.751ms), delivery (6.138ms) and horizontal accuracy. Android /
root, maps-pin, both135168-byte Data headers, generation1/869extents and87476
collector exit passed; trial and read windows are released. Keep accepted043
GPS baseline. Raw same-fix receiver/BeiDou pages are absent after USB reconnect
failed, so BeiDou contribution to this App callback remains unverified.

First query still took165.039s. Initial locationd70/235 queue-item exceptions
and successor scan callback670d98 synchronous waiting remain unresolved.
GNSS046 uses the existing retained-notification procedure for this callback:
retain its adapter and CF options/results before returning, copy a72-byte block
to the original silo, verify the same current open notifier, then invoke the
unchanged scan handler with the original options/results/status. The unused
device is not retained. Thirty-eight actual callback/invoke instruction replays
cover logging, busy queues, absent CF inputs, error statuses, ended/replaced /
closed notifiers and reuse of the caller frame/device after input-owner release.
CF copying and PAC are modeled; hardware benefit is pending. All159 original
records remain; the256-byte worker/descriptor and its hook add17 records,
requiring capacity176. No new modem or GNSS measurement/permission logic.

GNSS044 actual failure and GNSS045 DynamicStore correction (2026-11-23 Beijing).
Native02845050991211/52baf6dc succeeded;851 files and all219 driver objects match
Native027. Its returned158-entry parser/whole pages,strict38/import348,
basebandPAC1062,IOSurface,HMAC,current single/eight/loader,Win75 and real entry
passed. Fixed bootd53eb0f0b346 was tested once;App486 keeps only startup and
services_query_pending,with no position or LOC START. Human reports more than
five minutes waiting. Initial object exception at116.223s remains. Seven main
snapshots identify CLDaemonStatus::onDynamicStore at6ec860,not availability.
The single session-bound normal18df2697 returned Android/root;maps-pin,
17/27 same135168 headers,generation1/869extents and45628exit passed before
18:39:02Z protectedrelease. Originalconsole2,901,316B is preserved. The WiFi
availability delta remains a diagnostic candidate; its overall speed benefit
is not established by this failed boot. Accepted043 App GPS remains baseline.

GNSS045 adds one guarded instruction for the observed DynamicStore callback:
original56-byte block copied on the original silo,original invoke and actual
SCDynamicStoreCopyValue/CFNumber/status12/release behavior retained. Store and
changed-key captures are unused by the original invoke;daemon status lives for
the process. Forty-four original/deferred callback and updater executions cover
logging,busy queue,0/1,absent store/three keys/invalid number and caller stack /
unused parameters reused. Platform/PAC/copying are modeled,not real GPS proof.
Keep all158 previous records and GNSS measurement/START/STOP/ordinary permissions;
capacity159 native compilation is required. Initial dispatch-object lifetime,
SuggestedAction heartbeats and GPS/BeiDou same-fix App acceptance remain pending.

GNSS043 acceptance002 and GNSS044 source (2026-11-23 Beijing).
The original GNSS043 image8b34a8734c79/Native027 was used for one independent
trial. App556 records159 ordinary full-accuracy positions and all five starts /
four stops. Each restart returns new position data in7..10ms. In the second
window-side restarted session,17 App callbacks match63 retained native GPS
inputs by timestamp,receive time and precision;93 WiFi inputs are distinct.
Indoor callbacks after USB reconnection are not promoted to GPS. The first
query still takes53.996s. Six original same-boot logs contain734910 records;
one masked CommCenter timestamp is unresolved and excluded from correlation.
Android/root/maps/Data generation1 and collector27392 exit are verified;
the bounded read window released at16:57:08Z. No payload was written.

The current four main-thread samples (142/202/263/323s) wait in the original
WiFi availability callback at66fdd0. A new local helper retains the notifier's
adapter, copies a48-byte block to the existing service silo, checks the same
current open notifier and calls the unchanged availability updater. The
original invoke consumes only the notifier; its device capture is unused.
Eighteen original/deferred instruction replays cover logging, busy queues,
ended/replaced/closed notifiers and reused caller stack/device. Original
authorization decisions and GNSS measurement/START/STOP code are unchanged.
Keep all148 original signed-page records and add10 records (136-byte worker
plus its guarded call); the existing native table capacity becomes158.
The exact native build, package and hardware result remain pending.

The initial dispatch queue-object exception at113s and later suggested-action
heartbeat failures remain separate unresolved startup defects. The original
043 successful226 GPS callbacks are retained, but do not establish stable
startup, every satellite restart, moving precision or long-term acceptance.
Native027 is not rebuilt; new Native028 is only for the158-entry capacity.

GNSS043 actual trial progress (2026-11-22 Beijing).
The fixed Native027/ab50bbd boot8b34a8734c79 started on the local host once.
The last live receiver sample reports390 complete epochs,3510 published blocks,
388 unsupported NavIC slots omitted,zero rejected/incomplete epochs. After
foreground sky reception,the human observed several rapidly changing positions.
Current App641 has255 authorized CoreLocation callbacks and240 distinct
coordinate pairs,ages5..13ms;184 samples report horizontal accuracy9.2678m.
Original helper bootAB06CED5 and actual App exec640.679s bind this sample to
this trial. These are fresh system positions; native satellite origin and
same-fix GPS/BeiDou usage still require the retained system logs. One ordinary
locationUnknown was followed by more callbacks. The USB reconnect descriptor
failed,so no XNU return command was sent; manual Android/root/maps-pin,
unchanged17Data and27generation1/869extents,collector56852 exit and protected
release were verified at11:42:13Z. No payload was written. The original system
log reader is computer qualified and queued after the actual GPU077 owner.
Reuse this exact candidate and saved outputs; do not rebuild Native027 or
claim that position changes alone complete satellite acceptance.

GNSS043 complete candidate qualified (2026-11-22 Beijing).
The GNSS042 GLO15 correction reached150 complete receiver epochs and1500
published blocks before USB disconnection. It recovered transport continuity,
but its complete same-boot logs show1062 unsupported NavIC-system7 errors
and1081 native measurement resets. App566 has256 fresh positions; all295
retained fusion inputs are WiFi. Protected Android/root/maps/Data generation1
and collector2940 exit were verified, followed by six read-only service files
and protected release. Ordinary App satellite delivery remains unverified.

Productionab50bbd retains every real LOC source slot for epoch sequence,
clock and reset validation. After the complete source epoch arrives, exclude
NavIC8 from the selected PDS worker's output because its optional NavIC
capability is absent. Retain original GPS/BeiDou SV and clock bytes, then
number the representable PDS blocks continuously. The capability and NavIC
identity are unchanged. The existing provider distinguishes unsupported
source slots from empty slots. Missing or malformed omitted slots still
invalidate the source epoch; an all-NavIC epoch emits no fabricated data.

ASan/UBSan832protocol/820lifecycle/477measurement/10QMUX passed. Six original
worker replays cover GPS/BeiDou with capability0 and1; a full10-slot generated
epoch produces8 supported native blocks and ends the original sequence.
The first generated full-band fixture chose GPSL2/QZSSL2 and was rejected;
using the explicit GPSL5/QZSSL5 bands passes without changing production.
Old NavIC7 reproduces the actual capability0 rejection. Allocator, imports,
PAC, capability query and final publication are modeled, not phone evidence.

Unique Native02745009243023/8615a144/attempt1 succeeded;851 artifact files
match hashes,216of219 runtime objects are unchanged. Actual receive/publish
calls and NavIC comparison, unchanged148-entry applicator, strict38/import348,
baseband1062PAC, IOSurface and resource/HMAC checks passed. Current-kernel
single no-map rejection, eight CPUs with7 secondary CPUs, HID/display/backlight,
loader, Windows host75 and actual entry/journal-reader checks passed.

Fixed boot8b34a8734c79/payload0eeed7e87968/kernelde9070f26094 are in the
existing LinuxGPS output and I-drive host-trials. Only kernel and its HMAC
change; GNSS042's148-entry ADT, App012 and GPU051 are unchanged. The candidate
is file queued behind actual GPU076; the selected-window gate stopped the
first start before any GNSS claim, device command or collector. Wait for its
actual protection/collector-exit release, then test supported native epochs
and a real GPS/BeiDou fix against fresh ordinary CoreLocation output.

The bounded service reader now identifies the current boot using original
timesync wall/tick anchors and the App PID/exec identity. GNSS042 proved the
initial header time can be relative; reusing its original file and a wrong-App
negative passes. The same-fix reporter reproduces GNSS037's104ms/40.15m
WiFi mismatch and never accepts coordinate/time updates alone as satellites.
No stable GNSS or main-package promotion is claimed.

GNSS042 source delta (2026-11-22 Beijing).
GNSS041 starts the real LOC session and observes up to69 satellites/BDS18,
but returns no valid satellite fix and publishes no complete measurement
epoch. App604 records95 location callbacks; the100 saved fusion inputs all
carry WiFi. Its first state query takes28.027seconds, then the first LOC
START occurs later in the boot. Android/root/maps/17Data/Data generation1
and collector1572 exit are verified before releasing the phone.

The original minor177 factory ELF descriptor encodes gloTime (LOC0x19) as
15 packed bytes (1+2+4+4+4); the adapter incorrectly required19bytes, so
even an empty GLONASS slot interrupts the receiver's complete epoch.
Accept the actual15-byte aggregate and continue omitting its incompatible
PDS extended-GLO clock. No clock or measurement values are substituted.
The production worker's GPS/empty-GLO-with-real-time/BDS fixture now
publishes two complete PDS blocks accepted by the unchanged original
worker. Malformed14/16..19-byte fields remain rejected.

GNSS041's first main-thread snapshots at151/211/271/331seconds wait in
CLDaemonStatus::onThermalNotification at6ed060, then the status silo misses
its heartbeat. Defer that void callback's unchanged72-byte block on its
original silo. Its original invoke still reads the actual current thermal
level, not callback arguments, and retains the status13 notification and
update. Normal/busy queues, logging on/off and levels0/1/5 pass24 original
instruction executions. Framework, clock and PAC are modeled.
Retain all147 earlier records and extend the existing capacity to148 for
this one additional callback. Native target, package and real App satellite
acceptance remain pending; GNSS041 task reply and prior fixes are retained.

GNSS041 task reply source delta (2026-11-22 Beijing).
GNSS040's complete saved boot68E82912 records show the background task
scheduler's serial submission queue waiting in the original DAS synchronous
proxy at442/502/562/622seconds, followed by locationd heartbeat exits.
App661 eventually obtained WhenInUse/fullAccuracy after a77.512second state
query, but received no positions; LOC START and measurement epochs were zero.
GNSS040's complete receiver epoch source was not exercised on the phone.

Use Foundation's finite-reply proxy for the observed DAS task submission.
Complete invocation review and original-instruction replay show that a
nonzero timeout creates a reply semaphore. Options0 sends asynchronously,
then the original invocation waits for the real reply before returning to
BGSystemTaskScheduler's byref result reads. Merely setting a timeout on
options4 still enters the unbounded synchronous transport first; that
intermediate shim is rejected before packaging or phone use.
The final36byte shim keeps the original identifier,
descriptor, error block, completion, result and cleanup remain in place.
The real reply deadline is2seconds; invalidation cleanup retains its original
wait, so this is not a claim that the whole submission always finishes in2s.
The36byte shim adds4 original-byte-guarded records. Increase the existing
applicator capacity from143 to147 and retain all143 earlier records.
Retained original submission/proxy/wait instructions passed normal, nil
proxy, reply and timeout-cleanup replay. Runtime, transport, clock and PAC
are modeled; new native target/package and same-fix App acceptance remain
pending. GNSS037's real GPS/BeiDou fix and GNSS040 measurement code remain.

GNSS040 complete package qualified (2026-11-22 02:10 Beijing).
Native024 run44930935062/head d51d4a6eae7f1bb24a50f733bbda22e5fa4064f2
completed successfully. Its exact encrypted artifact14718745911 was received
once and all851 returned files verified. Of219 runtime objects,215 match
Native023; only NbGnss, NbGnssProtocol and the two existing bridge objects
changed. The actual receive/publish/encode calls and returned source match
production015ff1d. Strict38/imports348, baseband1062PAC, original IOSurface,
three resource/HMAC bindings and current-kernel single/eight checks passed.
The eight-CPU control reaches HID/display/backlight with7 secondary CPUs,
zero skipped stops; the no-map control refuses storage as expected.

Complete boot8a5ed8cb576f, payload76e215d68bd7, kernel929aa985dfd9 change
only the kernel and its HMAC member from038. The143-entry ADT, App012,
GPU051 addon and trust are byte identical. Windows host75, the actual manual
entry and the original bounded App reader fixture passed. The fixed package
is file queued; current GPU074 retains its phone until actual protection and
collector-exit release. No GNSS040 phone boot or ordinary App satellite
acceptance is claimed. GNSS038's manual Android recovery, protected release
and all153 retained fusion inputs marked WiFi remain the latest GNSS result.
The next step is one controlled040 trial and same-fix GPS/BeiDou-to-App
verification, with normal Android/root closeout and bounded local logs.

GNSS040 complete-epoch source delta (2026-11-22 Beijing).
The whole original PDS worker rejects an empty block lacking a real band,
even after the original sequence validator accepts its sequence. GNSS039
Native02344927784537/ed8a2ff succeeded and851files/219objects/one converter
object are verified, but it is computer-only evidence and is not packaged
or queued for phone use. That unchanged-worker counterexample is retained.

The existing NbGnss worker now assembles one complete receiver epoch before
publishing PDS blocks. It validates all original slots in order with the same
clock/reset, rejects partial/gapped/changed epochs, and bounds storage to32
blocks/128KiB. Empty zero-band slots carry no measurements and are excluded
only after every original slot arrives. Representable PDS blocks receive
contiguous packet sequence numbers; original receiver sequence/count/clock
remain separate diagnostic data. SV bytes and clocks are unchanged. No
replacement position, band, time, tracking state or quality result is created.
Duplicate/backward/reset/start/stop/reconnect and multiple clients retain
explicit failure handling; failed output stops that client's current epoch.

ASanUBSan832protocol/535lifecycle/462measurement/10QMUX passed. Production
NbGnss-generated three-slot epoch (GPS, empty GLO, BDS) outputs two real-band
blocks; the unchanged24A446 full worker accepts both in one persistent owner,
finishes its sequence and reads38dB-Hz for both original synthetic SVs, with
zero invalid detection calls. A synthetic first GPS block lacking receiver
frequency information was correctly rejected, then the fixture supplied its
explicit frequency. Imports/PAC/allocation/final publication remain modeled;
this is not real App acceptance. NextNative024 full target/strict/resources/
current single-eight/package040/Windows then same-fix GPS/BDS App test.

GNSS038 physical closeout / GNSS039 source delta (2026-11-22 Beijing).
The single GNSS038 boot7864ff8fe145 was manually returned to Android after
USB reconnection failed. Android/root/maps-pin and both135168-byte Data
prefixes/generation1 passed; collector32832 exited and the phone window was
released at16:07:11Z. Six same-boot files21993400B/793524 records were read
without writes and released at16:10:02Z. All153 retained fusion positions
were Wi-Fi; ordinary CoreLocation satellite delivery remains unverified.
589 sequence gaps caused3534 resets/2945 expected-first errors, and9704
unhandled detection-method errors are retained locally in the038 run.

GNSS039 corrects the existing LOC-to-PDS adapter only. Zero-band empty
constellation blocks retain the receiver sequence, maximum and clock/reset;
nonempty zero-band input and unrepresentable COMPASS measurements still
fail. An empty deprecated COMPASS block maps to unknown system0 without a
fabricated band or BDS measurements. PDS validity bit9 and wire85/89 are
original detection-method enums, not LOC azimuth/elevation floats. Clear
that unsupported validity and those slots; C/N0/time/Doppler remain exact.
The native worker reads bit10 C/N0 and bit9 detection independently.

ASanUBSan832protocol/464lifecycle/462measurement/10QMUX passed. The retained
24A446 worker reproduces old float-as-detection calls and has none with new
output, while C/N0 is identical. Its unchanged whole-epoch sequence validator
accepts all10 generated blocks and reproduces failure when empty block4 is
dropped. The actual rejected receiver packet was not retained, so its precise
constellation/band combination remains to be verified on the next trial.
Allocator/imports/PAC are modeled; this is not ordinary App acceptance.
Native023/package039 complete target qualification and same-fix App GPS/BDS
validation follow; Native022, GNSS037 and unchanged userland are not rebuilt.

GNSS038 computer qualification complete (2026-11-21 23:34 Beijing).
Production7366a7b / record5ae4b4e, Native02244916678838/attempt1/head076332c;
851 sealed artifacts verified,219 objects with215 identical and only GNSS
state/protocol plus original/late bridge objects changed. Actual compiled
receive->measurement publisher->wire converter confirmed. Strict38/348,
1062 baseband PAC slots, seven original IOSurface routines, private resource
HMAC and current-kernel single/no-map plus eight-CPU HID/display/backlight
checks passed. Completeboot7864ff8fe145 / payload6569bdefec85 /
kernel831b079c777d; only kernel and HMAC FAT members change from GNSS037.
The entire143ADT, App012 and GPU051 runtime remain byte-identical. Local
75-file host and actual manual-entry/closeout fixture passed; bounded App
reader replay recovered667648 bytes from retained GNSS027 data. File queue
ready, no GNSS038 phone boot or satellite App acceptance yet. Next: latest
actual protected release binding, one temporary boot, actual measurement
and ordinary CoreLocation same-fix evidence, normal Android/root postflight.

GNSS038 source increment (2026-11-21): receiver measurements now translate
LOC0x86 to the original PDS0x91/101-byte SV wire contract. Reuse Qualcomm
LOC IDL/LocApiV02@1ed86b1d, the existing baseband038 transport/worker and
original24A446 measurement readers. The per-client event0x01000000 and
report-constellation0x88 request follow the stock client; actual ACK plus
status are required. Explicit unsupported measurement configuration keeps the
previous position path and reports the limitation. NV, constellation hardware
controls, remote ownership, normal application authorization and the original
quality rules are unchanged.

The adapter preserves receiver clock/reset/sequence, C/N0, time, Doppler,
verified validity/status and actual bands. LOC/PDS BDS and later signal enums
are mapped separately. Unsupported split sub-sequences are rejected instead
of being marked complete; 16+8 extended measurements are supported per block.
Start/stop, duplicates/backward clocks, clock reset, two consumers, failed
output and reconnect use the existing bounded state machine. Measurement
counters extend the existing diagnostic page; measurement delivery is distinct
from position delivery. Host ASan/UBSan:832 protocol,395 lifecycle,352 measurement
checks (355 when emitting the generated replay packet),10 QMUX. Six retained
original-reader/worker cases confirm clock/reset/CN0 and constellation handling;
allocator/container operations and final validation are modeled. This is source
and host evidence. Native target, complete package and App acceptance remain
pending. The original143 ADT entries, App012 and GPU051 baseline are retained.

GNSS037 physical status: receiver reporting was verified at sequence1510 with
1509 deliveries and4GPS/6BeiDou used in that fix. Ordinary App450's254 positions
remained associated with Wi-Fi fusion; the nearest final sample differed40.15m
and104ms. Same-boot527 fusion rows all indicated Wi-Fi. Android/root, pin/extent,
17/27Data135168-byte prefixes/generation1 and collector880 exit were protected
and released. Existing logs are complete and retained; no GNSS038 phone run
has occurred. Next: compile this delta, qualify target/strict/PAC/HMAC/package,
then the ordinary CoreLocation same-fix GPS/BeiDou test under the file queue.

GNSS034 已完成脱USB接收及正常安卓后验（2026-11-20）：本机
26a2c83c/0355bdkernel一次启动，source3593e6e，App493前台250新鲜
回调和原生日志307位置都属WiFi／type4；卫星fix/BDS未验。原LOC
STARTACK0/session3在USB最后500s仍running1，271状态listabsent、位置0；
系统245活动GNSS客户端2保持至1481s，无心跳终止重启。屏幕通知等待
本轮未再见；App初次查询已从256记录中轮替，不能用末12ms盖全部启动。
助手97.411s零延时请求真实执行，但45s期限到后退出，App218.261s才启动，
开机App交付延迟仍待。人类室外5分钟没有一直观察，随后约5分钟只见精度
变、坐标稳，USB重连再出错；人类回安卓，root/maps-pin/17-27同135168/
Data1/71608退出后15:17:06UTC释放，819200B App读取过。原六日志
23,443,928B于15:21:11UTC只读后验释放，1147175records／151663所选
唯一校时齐。34服务持续及START进步，真实卫星仍未完成。

GNSS035修实际参考请求88B仍NOT_SUPPORTED的首个字段判断：原24A446
message builder在0x17f773c把messenger上下文地址低字节写TLV0x1a，
不是时间类型；真正timestamp独立在TLV0x10，原cached caller加
kCFAbsoluteTimeIntervalSince1970再乘1000。删除错误的非零context
拒绝，保字段宽度、UTC／坐标／精度／来源检查与真实ACK＋status完成。
全部256上下文字节保持同一LOC98B输出；832协议／228生命周期／10QMUX
ASan/UBSan及独立编码往返通过。新目标编译／回执／GPS-BDS仍待。

GNSS033 实机发现开机等待（2026-11-20）：App485 的定位总开关查询
201248→385383ms，耗时184.135s；下一次查询4ms。locationd280 的四组
142/202/262/322s 主栈停于 CLDaemonStatus::onDisplayNotification 的
原sync调用2c1960；之后560/658/736依次启动。LOC177仍Ready，但全轮
START0、reference0、位置0，未进入脱USB接收。一次正常XNU回安卓，
root/maps-pin/17-27各135168/Data1和9784退出后13:17:45UTC释放。
原生日志六文件18,753,208B于13:27:47UTC只读释放，701187records／
171735所选唯一校时记录通过。本轮相对032卫星启动退步，保留033源码
和诊断，不升主包。

GNSS034 修该单向屏幕通知：retain适配器，原silo复制48B block，worker
核当前notifier及其原closed0x220字段，再执行原notify_get_state和事件10。
原回调参数未被invoke使用；52个原/新指令用例覆盖busy、end/replace、
closed、原栈复用和读失败。新增136B代码／10条记录，需要容量132→142；
真实142页应用模型已通过并保原132条记录。另ccd4372去掉助手固定120秒
sleep、保一次请求和真实完成期限，定位期间用正常idleTimerDisabled
保持屏幕亮，停止或后台时恢复。App01244746630879/3564ba4成功36件；
新内核、整包和实际延迟／卫星验收仍待。

GNSS032 实机与原日志已完成（2026-11-20）：接收机 START141.180083s
收到 ACK0。143.608891s 的 STOP 对应最后后台客户端退出；人工 Start
794.476s 到达定位服务并在794.497s完成，同轮日志覆盖至890.539s，
未再见停止事件。App491 的22次普通位置更新（19新鲜、3缓存）来源为
无线定位／系统融合。GPS及北斗参与同次解算仍未验收。原六日志文件
22,306,816B、784,263 records／147,160所选唯一校时记录已保存，
安卓只读后验及窗口释放于10:45:37UTC完成，不重复采集。

GNSS033 源码补齐参考位置和卫星状态适配。LOC 的可选卫星列表缺省时，
保留 mandatory altitudeAssumed 状态、清除旧卫星值，并用list_present=0
表示未知列表；显式空列表仍为list_present=1。参考位置按原24A446
PDS0x38调用的UTC毫秒、WGS84坐标、精度及来源，转换为LOC0x39，
协议字段来自保留的Qualcomm LOC IDL。仅收到真实ACK和状态完成后
回复原请求；辅助请求失败不阻断正常启停，也不写入卫星fix记录。
正常最终STOP在真实ACK时完成当前请求；更新跟踪间隔仍等新START
ACK才完成。现有10秒报告增加同fix的UTC、technology、deliveries及
GPS／北斗使用计数，供脱USB接收后读取。没有新增采集设施。
64协议／228生命周期／10QMUX ASan/UBSan检查通过，独立QMI编码器
核对PDS88B→LOC98B并完成字段往返。目标编译、完整包及实机仍待。
GNSS032内部温度峰73.6°C、后段八核14.82%、control19／registration1
的回落结果保留，长期接收功耗及外壳温度未验。

GNSS031 complete computer package passed: Native01544705485224/head08ebe72,
846 files,219 byte-identical objects, actual132-page applicator, strict38/
348imports/1062basebandPAC and original7IOSurface routines. Completeboot
f79c92f7fa6840828cd0ca3ff26af1be4255d70f3245fc3861c0b1f4226c3c96, kernel
832841cf2aaf41ac7e3f7b301af7826b50f4acb6aed3e533eff79e97cf331686 and payload
1aff0c758878cf24d0791513cb243ec8cb9b70e83205991f5aee6cf32a65785a are retained.
Single no-map refusal and eight-CPU display/HID/backlight with7 secondary CPUs
passed. GNSS031 has not been booted or queued.

Before phone testing, the original tracking callers exposed another concrete
lifecycle mismatch. The sole original forceReceiverOff caller1773324 sends1
at initialization; later active-state tailcall16fa278 enables service and
16f9bcc enables automatic tracking. There is no forceReceiverOff0 caller in
the complete original text. The bridge kept forced_off until reset/another
force message, preventing that explicit start. The added actual C lifecycle
sequence reproduces113checks/1failure on the old source. GNSS032 clears only
the preceding off when applying a newer explicit service1 or automatic1;
queries and disable requests never clear it, force-off1 still sends realSTOP,
and a new explicit start still requires the realmode/START acknowledgments.
The actual C sequence now passes128lifecycle checks,33protocol and10QMUX
under ASan/UBSan. GNSS031's132ADT/activity/optionalsetup corrections remain.
Only NbGnss.c and its lifecycle test require Native016; all other native
objects, App011/helper009, GPU051, Data and ordinary permissions stay fixed.
Native016/fullpackage/host and physicalGPS/BeiDou acceptance are pending.

GNSS030 completed a protected temporary boot at15:48–16:04 Beijing on
2026-11-20. Native014's actual22B GPS enable request returned success; the
old INVALID_ARG30 is absent. Current App485 has ordinary global enabled,
WhenInUse/fullAccuracy and a start event, but no position callbacks. LOC177
remains Ready with commands2/START0/fix0. The original initialization sends
service disable before the enable response, so it is not evidence of an
error-induced disable. It also sends unsupported LPP0x7e and SV-use0x5557;
the original result blocks route NOT_SUPPORTED to the controller failure
state and repeat initialization. LPP assistance is absent in this standalone
adapter; LOC positions already retain their used GPS/BeiDou satellites.
The main thread at143/203/263/323s waits in the daemon activity callback
0x6ed258 sync:. The original callback block calls0x3b09cc, which reads the
actual notify state and sends the activity update. Peak internal TSENS77.9C
and late eight-CPU mean18.8669percent remain; no physical case temperature
was measured. One normal XNU return verified Android/root/maps-pin, both
135168B Data prefixes, original17 and27 generation1, and collector49212 exit;
released08:04:27UTC. Originalconsole10767633B SHA39d56554b5fb and App485 journal
are retained. GNSS029's160 ordinary locations are prior progress, not a
GNSS030 result or satellite proof.

GNSS031 omits only the two unsupported optional initialization calls at
1773460 and17736b8; their builders and other callers remain unchanged. It
also reuses the measured fitness adapter-lifetime pattern for the activity
notification: retain the adapter, copy a48B block to its original silo,
check the current notifier and invoke the original notify_get_state updater.
Fifty actual original/deferred instruction cases cover logging, busy silo,
state values, failed state read, ended/replaced notifier and stack reuse;
four original Eureka initialization cases retain service1/0, automatic0,
completion blocks and register/stack cleanup while omitting only the two
extensions. Framework calls/PAC are modeled. Eleven new records require132
entries; the sole native change raises121 to132. Original GNSS030 C sources,
GPU051, App011/helper009, receiver protocol/real ACKs, reset-result handling
and existing Data remain. Complete native build, signed-page application,
package/host and physical START/GPS/BeiDou acceptance are pending.

# nezha iOS27 GNSS source delivery

GNSS022（2026-11-19）普通 App 的当前 PID486 记录了真实的
`services_query_pending`，授权返回 denied/reducedAccuracy，但系统总开关查询
没有返回。同次 locationd PID256 的服务初始化线程在 Mach 请求中等待超过
180 秒，原查询在正常关闭前才集中处理；LOC177 保持 Ready，START0/fix0。
内部 TSENS 峰值77.1°C，后期八核平均97.25–98.125%，并有重复 CTL/WDS
请求；完整 CPU 归属仍待验证。正常返回安卓、root/Data 后验与窗口释放已完成。

GNSS023 在 `tools/gnss27/service_query_timeout.py` 给这条原请求的发送和
接收设置2000ms期限，保留原 MIG 回复解析、输出和错误收尾，不改变授权结果。
28个原指令调用场景验证正常回复、错误回复、无回复和端口清理。已有运动通知
复用原系统48字节描述符，执行代码144→128字节，32个原生命周期场景通过；
省下的一条记录用于通信超时，Native011原96条容量和内核可以直接复用。
实机总开关返回、服务持续运行、温度和真实 GPS/北斗坐标仍待验证。

GNSS023实机（19:25:41–19:32:59北京时间）确认347ce0超时修改实际生效，
旧347cf8长等待越过，但同一初始化在第二条Mach请求347754继续等。App480
本轮总开关查询仍pending/denied，内部峰76.4°C与尾八核98.25%，尚无START/fix。
GNSS024为第二条请求保留同样有限等待；42个区域通知场景验证复用原48B
描述符，保持异步管理器引用与完整枚举状态，原Native01196容量继续复用。

This increment connects the existing nezha modem/QRTR worker to the original
24A446 Eureka PDS consumer. The user selected GPU051 for integration and
testing on 2026-11-16. The fixed base is device revision
`76c5952683041a6581224135461ab7c21d47f6a5`, boot SHA-256
`4e4b785ca1bf7bea18d68248ade4cde09a9d9efb5d557fe65e4eaa975dac849a`.
The GPU051 shared ALS resource owner supplies the sole transport archive;
the modem backend reuses qualified baseband038 source. Target build, full
package qualification and physical Core Location acceptance are recorded
separately; source and host checks do not establish a satellite fix.

The original task and acceptance requirements are in the local project handoff
`docs/handoffs/IOS27-GNSS-CODEX-20261116.md`. GPS and BeiDou participate in the
first delivery. Core Location application consumption remains a device test.

## Source and shared owners

| Part | Source | Use and evidence |
| --- | --- | --- |
| Remote startup, GPS memory, firmware, QRTR | nezha NAS020/baseband034 source `e816468`, derived from QcomBaseband `dd0199c` and sheng | Existing worker, PAS/MSS, memshare GPS client 0, `gps/gps_service`, original client discovery and queues. These owners are reused. |
| LOC messages, units, satellites | Qualcomm `location_service_v02.h/.c`, `LocApiV02.cpp` at `1ed86b1d4f7f89faa1d4c441702fcad22c9cda0e` | Protocol definitions and behavior reference. LOC v2 service 16/instance0 was observed on this phone's Android. GNSS library/message capture is still incomplete. |
| QMUX/PDS | QcomBaseband `dd0199cf3300764ed52b23f4a47761f91fab21a2`, libqmi `3454c2c619d516fd46a08fe16c6eebdee061857d` | Standard QMUX allocation/response conventions and PDS flags. There is no upstream LOC-to-PDS GNSS implementation in the checked sources. |
| Original OS consumer | iPhone14,4 / d16ap / 24A446, local `locationd` SHA `d00659f0aef2475c0cb1f5358fff411229cbcbd0883b51fc470d40a87a300b18` | Parsed-position reader, lifetime callers, default1s/255s/400m, Eureka factory and report registration inspected. Private Apple bytes stay outside Git. |
| Same hardware and upstream comparison | UniXNU `4a654d1`, myron-next `f1a1bfe`, sheng `d24651f`, pandora `81b71b1`, Pagani `fcab49e`, fairlady `bb05659`, umi `3eb7257`, Trogdor `7e0a044` | No complete public SM8850 GNSS-to-Core-Location source found. sheng provides `gps/gps_service`; Trogdor has a GNSS channel but no inspected GNSS protocol backend. |
| Linux GNSS, QRTR | Linux `fb2c3f44` | GNSS character streams and QRTR ownership are references. Generic GNSS/NMEA support does not provide the modem LOC lifecycle or iOS provider. |
| QQ archive | Human-designated `M:/QQ_History`, existing SQLite/FTS and saved topics |135181 messages in the available archive. GPS/GNSS/BDS/frequencies/QMI/NMEA/CoreLocation keywords and relevant original contexts read locally. “gps也活了” is an author report; retrieved public issue still says “暂未适配GPS”. Neither is nezha hardware proof. |

The local evidence directory is
`/mnt/i/Code/UniXNU-nezha/evidence/gnss27-20261116/`; larger reverse-engineering
scratch files remain in `/home/hua/nezha-work/ios27/gnss27-codex/research/`.
Sources are new board code under BSD-2-Clause. Qualcomm/public protocol values
are referenced; proprietary Apple and vendor files are never embedded here.

## Implemented increment

`NbGnssProtocol` parses whole QMI/TLV chains, preserving valid masks, status,
original WGS84 degrees, receiver UTC, precision, fix ID, system tick, raw GNSS
signal masks and satellites. Coordinates equal to zero are valid. Missing or
non-finite values do not acquire valid bits. The packed PDS field is exactly
105 bytes: valid mask0, UTC ms15, time uncertainty23, latitude27, longitude35,
ellipsoid altitude43, MSL altitude47, speed51, heading59, circular precision63,
ellipse major67/minor71, orientation75, vertical precision79, speed uncertainty83,
horizontal confidence91 and DOP92/96/100. The calendar timestamp is absent.
Kernel code copies IEEE bits and uses integer conversion for time uncertainty;
it is suitable for the existing `-mgeneral-regs-only` recipe.
`raw_valid` records the receiver fields before consumer-specific validity
filtering: an unknown velocity uncertainty suppresses the PDS speed flag;
an ellipse at confidence other than95 is kept in raw values and omitted from
PDS validity. A valid circular precision can still be delivered.

`NbGnss` uses one LOC client owned by the existing baseband worker. It queries
the real LOC service revision and, following the stock minor177 client, registers
position/SV/NMEA/engine/session events, then handles the existing PDS clients.
The optional legacy INFORM_CLIENT_REVISION request no longer gates startup.
Registration carries the original AFW client metadata and a descriptive client
string; the existing operating-system authorization controls application requests.
The initial provider is standalone: mode4, periodic recurrence1, accuracy3,
intermediate reports off2. Every client retains its requested interval and
accuracy threshold. The modem uses the shortest active interval; delivered
updates obey each client's interval/threshold. No EFS/NV, GNSS engine lock,
factory configuration, constellation masks or assistance caches are written.

Client commands queue across async initialization. PDS replies preserve the
native transaction; START/STOP success requires the real reply, and set-mode
requires the response and async status. Query responses reflect acknowledged
state. Unsupported SUPL/MSB/MSA, measurement, raw sensor, assistance and vendor
commands return NOT_SUPPORTED. Configuring a report that is already disabled
does not start hardware. No success is invented for unsupported reports.

Stops, client close, QMUX release/sync, endpoint loss and fatal command errors
retire the module's own session/clients. START reply timeout results in one STOP
for the same session before new requests execute. STOP failure closes the
provider. Reconnect gets a fresh QRTR source port in0x4200..0x42ff; NAS remains
on0x4180/0x4181. Wire transactions and session IDs are not reused on a live
endpoint. This initial implementation permits255 starts per endpoint before
closing the provider; final package integration must decide the needed port
rotation policy if that limit is insufficient.

Only final successful satellite fixes with UTC, coordinates and circular or
95% elliptical precision usable by the original PDS consumer are published.
Multiple signal records for one satellite are retained and unique GPS/BDS
satellites counted once. Duplicate/backward UTC reports and a
receiver-vs-host delay over10s are rejected. A new current-session first fix
still needs device proof of freshness. A forward receiver clock correction
reanchors monotonic comparison; backward clock reset requires endpoint/session
recovery and never rewrites a stale fix to the current time.

## BeiDou scope

The phone's model specification lists B1I/B1C/B2a. The public LOC IDL defines
B1I0x200, B1C0x400, B2a-I0x1000 and B2a-Q0x80000. This source preserves the
signal masks; actual phone firmware availability and tracks are **unverified**.

The position report's0x36 expanded-used list (or0x2c legacy list) is the usage
evidence. BeiDou IDs201..263 identify constellation6;0x37 signal types align
with that same expanded list. A preceding sky report never becomes used-in-fix
evidence. If signal types are missing, constellation usage can be reported but
frequency usage stays unknown. If only satellites are visible, the result is
“已观测到北斗，参与解算待证”.

The existing USB diagnostic catalogue uses provider9 for MSS/GNSS HISTORY
requests; GPU051 retains its audio provider8. Page0 returns the current
valid fix, IEEE coordinate/precision bits, UTC/receive time, generation, sequence
and usage counts; pages1..176 expose individual used satellites. Page256 is the
sky summary, pages257..432 its tracking records. Subsequent pages must carry the
summary cursor; changed generations/reports return EXPIRED. This is a read-only
extension of the existing diagnostic framework. `bds_seen` counts valid BDS
IDs with tracking state3 and a valid positive C/N0, rather than idle catalogue
entries. Sensitive positions remain local.

`tools/gnss27/corelocation_evidence.py` compares saved receiver pages with a
foreground application's ordinary authorized Core Location sample. It requires
the same boot UUID/candidate SHA, full accuracy, a non-simulated source, fresh
application time and coherent used-satellite pages. Missing signal fields remain
unknown. Its local JSON comparison is prepared; synthetic comparison execution
and real application evidence wait for the selected package.

## iOS27 consumer recipe

`tools/gnss27/consumer_fixups.py` reads the exact local original `locationd` and
prepares two entries for the already-used ADT `text-fixups` parser. It selects the
existing Eureka constructor path at the inspected factory and restricts that
consumer's report registration to parsed position. Original PAC vtables,
callbacks, locationd authorization and Core Location delivery are reused. The
script does not edit donor files or build an image. Original signed source pages
and the existing page-substitution mechanism must be qualified in the selected
package. Actual Eureka bootstrap, GNSS lock interaction, unsupported vendor
commands and Core Location app consumption are still required device checks.

`integration/gnss-locationd.patch` keeps locationd enabled only when the typed
`/chosen/nezha-gnss=1` switch is selected. It uses the existing per-job policy
writer and does not change other jobs or Data. The switch also requires
`nezha-baseband=1`. The selected GPU051 candidate enables both switches.
`nezha-gnss-test-app=1` also mounts its signed ordinary test app and current
LaunchServices registration helper through the existing read-only unions.

## GPU051 qualification and physical acceptance

The fixed compiled implementation is device revision
`586340c4f7cd003878f572853989d961e9cb941f`. Native003 run43884693400,
head1159a6c2d40c9b3644a4a985f0c1507e2dc21984, passed23 baseband/GNSS
translation units,355 external imports and the actual KeyStore/USB checks.
Strict prelink passed38 components and348 original imports. The1062 baseband
and754 GPU/Legacy/HID authenticated slots retain their targets and PAC data.
The final kernel passed the production loader parser, HMAC, guarded60-entry
text-fixup parser and single/eight-CPU QEMU controls. QEMU disables physical
Qualcomm drivers; it establishes no satellite result.

The ordinary Core Location scene app and current24A446 modifydb registration
helper passed run43884264768,9 actual consumer registration fixtures, every
4KiB signature page and actual iPhone cache dependencies. The app has empty
entitlements. Its normal WhenInUse/full-accuracy permission is required for
position updates. The signed launch cache preserves GPU051's729 jobs and adds
one bounded launcher;54 non-cache addon entries and all four original trust
modules remain byte-identical.

Stock LOCv2/minor177 descriptors confirm the selected scalar fields, legacy
80-SV arrays, expanded176-SV arrays and28/29-byte satellite wire aggregates.
The larger reports add optional payloads. Stock Android HAL declares B1I,
B1C and B2a; tracking and same-fix use still require physical reports.

Package `gnss001-GPU051` has boot SHA-256
`a5907a796c920216229d5b9d7d14f88eeb07f1e2a13ac7db1f54c26ecb2f9284`,
payload SHA-256
`b96c1dea7b02bf57e6056c047e07891a52ceb2b4b5767ea02f79e54b9561eeb2`.
Its complete embedded payload preserves GPU051's Data generation1 and root
policy; the existing baseband firmware file is only read. Windows r008's75
files and concrete lifecycle passed. Raw/private evidence stays at the local
project `evidence/gnss27-gpu051-20261116` and package001 directory.

The user deferred manual permission and window/outdoor reception. The first
phone trial checks startup, baseband/LOC ready, locationd and ordinary app
registration. Receiver fixes, GPS/BeiDou use and ordinary callbacks remain
unaccepted until matching physical evidence is available.

The integration and acceptance sequence remains:

1. Create its isolated copy. Apply this source delta onto that version; preserve
   newer NAS/VOICE/UIM/WDS/IPA/device identities instead of replacing its bridge
   wholesale. Reconcile shared-owner pins and build lists.
2. Enable typed `nezha-gnss=1`, retain the original baseband startup and root/Data
   policies, ensure locationd runs, and append the two validated consumer entries
   to that package's existing `text-fixups` property. Do not replace older entries.
3. Run `sh tests/gnss27/check.sh`, applicable existing baseband checks, exact
   target SDK/arm64e compile, strict prelink and signed-page/PAC validation.
   GNSS contributes two C objects and the original bridge translation unit.
   The standard recipe remains32jobs. Mac-only work follows the existing
   minimal-source/encrypted-artifact permission and cannot upload private input.
4. Validate foreground Core Location authorization with an ordinary application
   and record coordinates, accuracy and timestamp against the receiver fix.
   App selection/signing/installation and fixture permissions are part of the
   specified-package stage. Do not bypass location permissions.
5. Use the file-controlled phone schedule with the fixed image. Capture first
   satellite fix, GPS/BDS use and available frequencies, repeated enable/disable,
   two consumers, cancel/client exit, signal loss/recovery and peer-module
   coexistence. Ask for any necessary outdoor/window conditions in that phase.
6. Return normally to Android, complete the required real protection postflight
   and collector exit, then write the run result and integration/rollback handoff.

Rollback uses the prior selected-package boot/payload. The test app and launcher use the existing
addon directory on Data; they require no phone payload write or Data reset. Full system positioning and device usability
remain unaccepted until the real application/receiver evidence is available.


GNSS001 physical startup on2026-11-17 reached root/Data, locationd and both
readonly app/helper mounts. Its inherited provider8 conflicted with GPU051
audio8, so `nezha_baseband_start` returned before creating the worker.
The fix selects unused provider9, keeps the audio diagnostic identity and
checks startup registration against that catalogue. The ordinary application
registration reached original `com.apple.lsd.modifydb` lookup3/NoSuchProcess;
no app registration or satellite fix was accepted. The run returned normally
to Android/root, verified unchanged17 and Data generation1, stopped its
collector and released without a phone payload write.

The second candidate, `gnss002-GPU051`, retains that GPU051 baseline and
baseband038 backend. Compiled device revision
`9255bde3d5cb573ceec600f035ee3b29a8d07f73` changes MSS/GNSS to provider9;
GPU7 and the original audio8 remain. Native004 run43922194416 at
headc818154a19bda5468abc55a582784098ce2a15b6 passed the23 target units,
355 external imports, strict38-component/348-import closure, authenticated
tables, production loader/HMAC, guarded60 fixups and single/eight-CPU QEMU.
No additional shared transport archive is linked.

Packaging revision `21a7328965fed598b09302372f491b44e7232c7d` selects the
original `LSOperationRequestContext.targetUserID=501`. Original24A446 native
code resolves that context to the existing mobile LaunchServices domain.
Run43925409254 at heade5248ad929e5a5b8aef562c4409c59b12babc445 passed
the9 consumer fixtures, signed pages and actual iPhone dependencies. The
ordinary app retains empty entitlements and normal location authorization;
the launcher uses only its existing application-registration permission.
Successful real mobile-domain registration is still a device check.

The fixed package002 identities are:

- boot: `38861ac45627a7f4cc2e5c6d9e705c97c1afdb437d67d120a7b2225fada0e004`
- embedded payload: `967d44d752f6ba31c32b7fe7509a2169cc876785dd83130fa6762c4b2a79e2bd`
- kernel: `075a15bcb6875f107aa46ee56766b77309f82b7ebf59b1e919ad4d05ebc23dfb`

The176168960-byte boot image embeds its167772160-byte payload. It retains
the original BootShim, GPU051 runtime,54 non-cache addon entries,729 launch
jobs, four original trust modules and Data generation1. The candidate adds
the signed app/helper and one bounded launcher. Existing root and modem
firmware inputs remain read-only. The package and actual113 host-r00875
files passed their checks; remote deployment and the concrete lifecycle
were qualified before queueing. The requested113 trial checks real MSS/LOC
startup, locationd and application registration, then returns normally to
Android/root. Manual permission, outdoor reception, fresh Core Location
callbacks and same-fix GPS/BeiDou use remain deferred by the user.

GNSS002 ran on the local host on2026-11-17, after the user moved the phone
back from113. Fresh provider CAPS identified MSS9 as `nezhamss` because the
retained USB serializer removes punctuation; the original audio8 remained.
The initial collector catalogue predates late MSS registration. A fresh
same-session catalogue and read-only provider9 requests confirmed MSS online,
GLINK,61 QRTR services, zero watchdog events and LOC service revision177.
LOC initialization then ended with INVALID_ARG48, before START or a fix.
Three recorded locationd fatal PAC instructions were BLRAAZ and AUTDA; the
original fatal path exits before the existing later exception recorder.
The recovered launcher stderr contains only repeated4099 modifydb lookup
failures, without a fresh process/boot marker, so new registration is not
accepted from those bytes.

The run returned normally through sessionc719aaa40e2a442b/request9097df2f.
Actual Android/root, unchanged135168-byte17 prefix, Data generation1,
protected mappings/pins and collector17988 exit passed before release at
2026-11-17T05:14:11Z. Data's ordinary new allocations added36 pages; no
payload or partition write occurred. Exact USB raw3238853 bytes and the
two launcher files were recovered with5775360 bytes of read-only APFS input.
Private raw evidence is in local project
`runs/2026-11-17-ios27-gnss002-gpu051-local002/`.

The next source increment follows stock direct event registration with AFW
metadata, retains failed-command and ACK/indication state in HISTORY, and
uses the existing bounded QMI header tracer for LOC. The first three locationd
PAC faults are captured before the unchanged fatal handler. The launcher job
runs as the existing mobile user and logs its actual PID/UID/boot/uptime.
Native005 run44028683069 at heade9d4c282d7fb11fdc160cdc2ce7e24a12c2b638c
compiled the23 target units and resolved355 external imports. The216 other
runtime objects match native004; only the GNSS state machine and included
bridge translation units change. Native fatal-capture code is present. Strict
prelink passes38 components/348 imports,1062 baseband and168/379/207
GPU/Legacy/HID PAC slots, and seven full original IOSurface routine hashes.
The final entry/HMAC kernel passes its current production parser, single
no-root-map refusal and eight-CPU display/HID/backlight6 controls with seven
secondary CPUs and zero skipped targets.

The mobile launcher and retained ordinary app passed run44028701894,
head5f3bc06abe1a87cbe8a6d69b6eb6451e03f41783,9 interface fixtures and every
4KiB signature page. The normal app remains byte-identical. Only the helper,
its launch-cache job and detached signature change; all729 original jobs,
54 GPU051 non-cache addon entries and four original trust modules remain.
Package003 uses boot SHA-256
`a50ed09ac5e7d0a2a8975603b636492575245a127933730c4aa9c862d94a0be8`,
payload SHA-256
`98ca0a5ef4227e4996494cc9d192cd054266fea538dca292cd29f49ababa8dd9`,
kernel SHA-256
`cdda3c7a9b6a434238985c196d6edfc65a6a01a09d2caba8e85b1f9851ba96bc`.
The original boot-loader/FD prefix is identical to package002. Current
host-r00875 files and the concrete local lifecycle checks pass before
queueing. Fresh CAPS avoids the stale early catalogue and validates the
actual sanitized provider name, session and reply provider. The third local
trial remains pending its selected file window; manual positioning stays
deferred.

GNSS003 local startup on2026-11-17 confirmed the service-revision query's
reply and indication, then the original REG_EVENTS0x21 reply returned
ARGUMENT_TOO_LONG19. The header trace and failed-command33 fields identify
the exact request. This increment used a16-byte client string although
both inspected LOC IDLs limit that field to4 bytes. The next request uses
`NZCL` and remains the ordinary AFW client. Its complete packet is36 bytes.
No START or fix was produced. This run showed no locationd PAC report in
the recorded window. The extracted launcher file was unchanged from the
previous run and had no new scheduled/startup marker.

The next launcher job uses the already-tested RunAtLoad/LaunchOnlyOnce
condition. The process logs its actual PID/UID immediately, waits120 seconds
for startup, then uses the original45-second registration deadline. It runs
as mobile and retains normal application authorization. The job's initial
KeepAlive condition is removed by LaunchOnlyOnce after its first exit.
The completed GNSS003 run returned normally to Android/root, verified the
unchanged135168-byte17 and27 prefixes and Data generation1, confirmed
collector90388 exit and released at2026-11-17T08:29:52Z. Raw USB2103041 bytes,
SHA42c714f1ac7e8cb81364dfbb3308a0cddc250cb5a30f6534546dacef3e3458ff,
and launcher results remain in local project
`runs/2026-11-17-ios27-gnss003-gpu051-local001/`. New native and application
qualification are required for the4-byte registration and launch condition.


GNSS004 continues from source `770e14435cbcf4e2e3e9bcf933eca7cdc9cb8794`
in local branch `codex/gnss27-gpu051-continue-20261117`. The selected package
remains GPU051. Upstream `630b85a307f19c8d8a4699d5bfc12f6796b874aa` and
myron-next `f1a1bfe` were fetched on 2026-11-17; neither supplies a new GNSS
backend. The upstream registration agent change targets macOS Aqua and
does not change this iOS mobile registration path.

Native006 run `44048805411`, head
`a0040b8c65e40217adcd360b002fa70f3c923f7b`, compiled the 23 target units
and resolved all 355 external imports. All 841 returned file identities
match. The four integration patches regenerate each compiled source hash;
of 219 runtime objects, only `NbGnss.c.o` differs from Native005. The LOC
client identifier is four bytes (`NZCL`), and the subscription packet is
36 bytes. CoreLocation006 run `44048997325`, head
`ee55a42c69a52d26bfddbf77dcda2419493e0336`, passed the nine original
registration-consumer cases and the iPhone target build. The ordinary app
is byte-identical; the mobile helper records its scheduled PID/UID, delays
120 seconds and attempts registration once.

Package004 passed the strict 38-component / 348-import closure, 1,062
baseband and 168/379/207 GPU/display/HID table slots, seven original
IOSurface routine hashes, final kernel entry and POST field, and the current
single/eight-CPU startup checks. Seven secondary CPUs reached display/HID
and backlight stage 6 with no skipped targets. The original ADT, BootShim
and FD remain byte-identical to Package003. All 729 original launch jobs,
54 non-cache GPU051 addon members and four original trust modules remain.
The actual Windows entry fixture and 75 host-r008 files passed.

The fixed Package004 identities are:

- boot: `c416ac00d2a0065228ec82a8b42cdc6c205ccf4fe04bf38a2f1447c635b352c2`
- payload: `4ce004714776deb17dcf9e41eb3e51fcf15ccc12628c455539c31855e46e02d2`
- kernel: `c1f80a006593a52b4564d9120004b26db1c300cf127acbc6de065a597812a743`

The candidate is queued for one local bottom-layer/startup trial. It checks
the current LOC subscription reply, provider 9 and the helper's actual
process/boot markers, then returns normally to Android/root with the
existing postflight and collector-exit checks. Manual location permission
and sky reception remain deferred by the user. A satellite fix, same-fix
GPS/BeiDou use and ordinary Core Location delivery remain unverified.
Rollback uses retained Package003 or the original selected GPU051 image.
Local package and raw evidence: `gnss27-codex/package004` and project
`evidence/gnss27-gpu051-20261116`; physical results belong in the actual run.


GNSS004 physical startup ran at 2026-11-17T09:52:31Z. The LOC177 query
reply and indication succeeded. REG_EVENTS transaction2 was the intended
36 bytes but returned INVALID_ARG48, followed by a second INTERNAL3 reply.
Eight fresh provider9 reports consistently recorded failed_command33,
phase5, no START and no position delivery. The helper output was the same
1,296 bytes with an old timestamp and no new process/boot marker.
Normal XNU request6c769f17/session5c436df2604e0aee returned to Android/root;
protected mappings, unchanged17 135168-byte prefix, Data27 generation1 and
collector7976 exit passed. The window released at09:59:32Z. Raw USB
2,999,803 bytes (SHA6d0f9d1b64f8) and both helper files were recovered with
4,726,784 bytes of bounded read-only APFS input. This is protocol diagnosis
progress; registration, application startup and satellite positioning remain
unverified.

The next source matches the actual stock `locClientRegisterEventMask`
request: client string `HAL` (3 bytes), AFW type1, notification valid1/value0.
The inspected24-byte structure and LOC177 descriptor were replayed through
the existing QMI codec under ASan/UBSan: its body is28 bytes, with no inner
length or terminating NUL in the string TLV, and the complete packet is35
bytes. No different client type or receiver configuration is requested.

The previous runtime pack updated the cached job but retained the old
`daemons/org.unixnu.nezha.location-launch.plist`: root, RunAtLoad=false and
StartInterval120. That daemon/cache mismatch is a concrete packaging error.
`tools/gnss27/launcher_job.py` now applies one source job to both members and
compares their decoded dictionaries before packaging. The job uses mobile
in the System session, RunAtLoad/LaunchOnlyOnce, KeepAlive=false; the
existing helper owns its120-second startup delay and45-second deadline.
The mismatch regression and33 protocol /65 lifecycle /10 QMUX checks pass.
GNSS005 target, complete package and actual startup are still required.


GNSS005 source includes `7cc0fe206ef91216dca484245571d3624464fa63`
(stock LOC request and coherent launch jobs) and
`ee3d0a031f5526071140fe2ba76403566c4b37e7` (explicitly initialized shared
IPCC host fixture). Native007 ended before target compilation on that old
fixture; its source and failure output remain local. Native008 run
`44067720727`, head`39832ccbca24467fc9e24da8f330fbd47f7585b2`, then passed
all current host checks and the23-unit /355-import target build. All841
output identities match; only NbGnss.c.o differs among219 runtime objects
from Native006. The actual NbGnss35-byte registration body equals the
existing QMI codec output for the stock LOC177 descriptor byte for byte.

Package005 passes strict38 components /348 imports, all baseband and
GPU/display/HID table bindings, seven original IOSurface routine hashes,
final entry/POST and the same single/eight-CPU startup controls. Seven
secondary CPUs reached backlight6 without skipped targets. The actual
Windows entry and75 host-r008 files pass. The signed app/helper reuse
CoreLocation006 run44048997325; only the launcher daemon, cache and its
detached signature differ from Package004. Both job dictionaries now
match mobile/System/RunAtLoad/LaunchOnlyOnce/KeepAlive=false.

Fixed Package005 identities:

- boot: `4964ce53a6a64e43726c698d990af48d7c1f7173042043659b13ab227fbd825a`
- payload: `9b0ab11582033aaad7eb528442d1159bc6440b459d1fa275b8b18a2f4918505f`
- kernel: `03fb639c817a33b170d227426746662cd958aa95bf722759dbd957ec9133e874`

GNSS005 is queued for the next local bottom-layer/startup window. Its real
LOC subscription, helper startup and ordinary registration remain pending;
manual location authorization and sky reception stay deferred. Package004
and original GPU051 remain rollback choices. The current pipeline and
qualification inputs are saved in local `gnss27-codex/package005/ready.json`
and project`evidence/gnss27-gpu051-20261116/checkpoint.json`.


GNSS005 physical result on2026-11-17 is progress: at80.597s the real
35-byte REG_EVENTS request completed with error0 after the LOC177 query
reply/indication. Eight fresh same-session reports show phase4/Ready,
failed_command0, errors0. No LOC START, satellite fix or Core Location
callback occurred in this bottom-layer trial. Three actual locationd
exceptions were captured at115.810s/132.139s/132.464s. Exact instruction
matching gives shared-cache slide0x1b88000 and source PC0x1aeafb6b8,
`libdispatch.__dispatch_lane_serial_drain+0xc0`; its continuation object's
metadata pointer fails on the original data-pointer instruction. The frame
chain reaches dispatch lane/workloop and pthread. The cause of that object
state remains unresolved; these frames do not identify a specific GNSS
caller or justify changing the original dispatch routine.

The launcher output remains the same1,296-byte old record with no scheduled
PID/UID or startup/boot marker. Updated daemon/cache dictionaries were
verified in the package, but the live member identities and actual launch
are still unverified. A32MiB/150-second read-only Android entry is queued
for the current helper, daemon/cache and at most six locationd reports.
It reuses the actual GNSS lifecycle, Windows session lock and APFS overlay,
with no boot or phone writes; normal same-boot/Data postflight follows.

Normal XNU requeste1aecab7/sessionc0b3ab248d8457ab returned to Android/root.
Protected mappings, same135168-byte17 and27 prefixes, Data27 generation1
and collector83600 exit passed; the window released at11:50:54Z. Raw
USB2,110,677 bytes/SHA8fbd442d5f5f and both launcher files were recovered
using4,464,640 bytes of bounded reads. Native008/full package checks remain
recorded separately. Manual GPS/BeiDou reception and ordinary application
authorization remain deferred; source is not promoted to the main image.


Android runtime read13:29:44-13:30:14Z retrieved the current helper
SHA9325938c and daemonSHA69bee5c3, identical to GNSS005. The old stderr
is uid0/group250/mode0644 and its bytes are unchanged, so the mobile
helper cannot write that file directly. The read happened after camera085:
its current cacheSHAf586537d contains729 jobs and no GNSS entry, and does
not establish which cache was loaded on GNSS005. Directory enumeration
reached the32MiB limit before complete locationd reports; the next read
uses existing4KiB metadata pages and known report filenames. Same Android
boot/root, mappings, unchanged17/27 prefixes and Data generation1 passed
and the window released. No phone boot or write occurred.

The helper now opens its own
`/private/var/mobile/Library/Logs/nezha-location-launch-mobile.stderr` with
mode0600 before Foundation startup, logs PID/UID immediately and retains
the existing120-second delay and45-second registration deadline. launchd
stdio goes to `/dev/null`, avoiding that old root-owned descriptor. Its
original application-registration and location permission interfaces are
unchanged. The nine actual registration-consumer cases and a new output
ownership case are committed for the next Mac target build.


GNSS006 reuses native008/kernel03fb639c byte for byte. Helper007
run44100550675/heade31d557 passed the nine actual registration cases and
the new0600output case. Packageboot8d6499b2/payload48cbbb55 passed all
current signatures, daemon/cache agreement, single/eight-CPU and Windows75
entry checks. Its actual14:56:51Z startup again gave eight LOC177
Ready/error0 reports, no START/fix. No locationd exception was captured in
this window, but only its startup line was present; this does not establish
a consumer fix. The mobile output file was absent. Normal XNU returned
Android/root; maps, same17/27 prefixes, Data1 and47288exit passed. Released
15:03:49Z, rawUSB2,072,376B/SHA3839ee57 and4,988,928B bounded output read.

The follow-up Android read captured helperSHA801a3b4f and the694B daemon,
both current. It also retained the444B original cache-loader log. Its
`Trying to send bytes ...4096` message prints `_vm_page_size`; the real
copy/mapping uses the fstat length at stack+0x90. The current cache/log were
written after camera086 and are notGNSS006 bootstrap proof. The32MiB bound
read actually used11,542,528B, sameAndroid/root/Data postflight passed and
released15:43:41Z with zero boot/write.

The next local package reuses audio105's measured original
`com.apple.DumpPanic` boot task. That task ran as PID53 onGNSS006, while the
independent added launcher has no observed execution.
`tools/gnss27/launcher_job.py::boot_task` preserves the original RunAtLoad,
boot-mode, transaction and crash-only relaunch fields, changes its program
to the existing signed helper and explicitly selects mobile. The independent
GNSS job stays in the cache but is disabled to avoid a second execution.
The original DumpPanic binary and every other job remain available.
The host regression checks the exact original task and all retained fields.
Actual launcher invocation and normal LaunchServices registration remain
next-package device checks. No new kernel or helper compilation is needed.


GNSS007 (`35d580f0654838092abe5a3f74ad84fdd841eced`) is fully computer
qualified, using the exact native008 kernel and helper007 binaries.
The only payload members changed fromGNSS006 are the addon and trust cache.
The original DumpPanic task selects the signed mobile helper; the added
independent task is disabled. All728 unrelated original jobs and four
original trust modules remain. Current trust single/eight-CPU controls
reach seven secondary CPUs, display/HID and backlight6; Windows75 and the
concrete entry fixture pass before queueing.

- boot: `f23f904d603b3d2fd4718e3c5b4842f45440f8baa27a1a1e91bce2d57cd4f93b`
- payload: `f3b1f254caba4b00f12ff36b400d5b1ea637148b00ff7170a42db9c2fecaa0a6`
- kernel: `03fb639c817a33b170d227426746662cd958aa95bf722759dbd957ec9133e874`

GNSS007 physically ran on 2026-11-18 00:08–00:15 Beijing
(2026-11-17 16:08–16:15 UTC), USB session `3e43c7caa6958406`.
Seven successful fresh provider9 state replies retain LOC177 Ready/error0,
failed-command0 and no START or fix. No locationd fault was captured in
this window; the older consumer object issue remains unresolved.

The original boot task now actually runs the helper as PID53, UID/EUID501.
Its new 494-byte mobile output records process/startup markers, uptime99.043
before the120-second delay and uptime219.044 at registration, with boot UUID
`308B4298-AD3C-4ED1-8A38-CB5E9476C421`. The LS application proxy reports
`installed=1`, `placeholder=0` and `/Applications/NezhaLocation.app`.
Registration is already present, so no registration request is sent.
The ordinary `openApplicationWithBundleID:` request returns false. This
separates the now-verified helper invocation and app registration from the
remaining launch failure. App execution and Core Location are still unverified.

One session-bound normal XNU return was followed by actual Android/root,
maps/pins, matching135168-byte Data17/Data27 prefixes and Data27 generation1.
Collector59968 exited; release is `2026-11-17T16:15:38.977407+00:00`.
No phone payload was written. Output extraction read5,775,360 bytes; helper
SHA-256 is `4b81c83b3c5db774a69afd9d64ca6a81cf6cf42ab488944f5a9204d90210595f`.
Final console is1,971,817 bytes, SHA-256
`40e00448ef66e1eabd739891bca6465e4b3e73f36c21e05261f9898520d8182e`.
The retained process export is1,878,035 bytes, SHA-256
`e1866a2e49507804f0a00516c6b9698e921e05db97c95dd90c62ebc336b7cf62`;
collector final count is1,948,709 bytes. These are distinct snapshots.
All results remain in local project
`runs/2026-11-17-ios27-gnss007-gpu051-local001/startup-analysis.json`.

Next inspect the actual24A446 ordinary application launch method and its error
result. Keep this working boot-task entry and the unchanged native008 kernel.
Manual authorization and satellite reception remain deferred.


GNSS008 source replaces only the helper's ordinary application-open wrapper.
The original24A446 `openApplicationWithBundleID:` discards `NSError` in its
synchronous completion. The `configuration:completionHandler:` entry reuses
the same lsd open RPC, default options and `useClientProcessHandle=0`.
The helper sends one request and waits up to30 seconds within its retained
45-second process deadline. It records native BOOL, error domain/code,
description and underlying error, or an explicit incomplete-request timeout.
An installed app is recorded independently from its launch result.
The existing mobile boot task, registration, normal location permission,
ordinary App and native008 kernel remain the measured baseline. Actual-source
host fixtures cover delayed completion from another thread, request failure
and its nested error, an inconsistent true-plus-error response, unavailable
selector and absent completion. Target and device results follow qualification.


GNSS008 computer qualification completed on2026-11-18. CoreLocation008
run44127071644 / head523401f0a97b59899dfcc908ff054b60b20435db passed all13
actual consumer cases and arm64e iOS27/4KiB signature checks. Helper SHA-256
`65fe480182b23684aadadf44ec3d21d7f1b53746eff41c1b3b41167ccface64a`
is the only changed addon member. The app, launch cache/signature, boot task,
original four trust modules, native008 kernel and kernel arguments are identical.
Current trust single/eight-CPU controls and Windows75/actual entry checks pass.

- boot: `52d6fa1dd8b23f6b67dc5e4bfdbda185bb07f345dcf80ec3c38deb91c3459340`
- payload: `be6a4d962a9cdbe993918ea566ccaab3467b4cf74e11546e8c276916f37496e6`
- kernel: `03fb639c817a33b170d227426746662cd958aa95bf722759dbd957ec9133e874`

The fixed full temporary image is ready for its file-selected phone window.
Actual ordinary launch completion/error is the next measurement; satellite
and manual application authorization tests remain deferred.


GNSS008 physically ran2026-11-18 00:43:39–00:50:38 Beijing. Eight successful
fresh LOC177 state replies remain Ready/error0/failed0; no START or fix.
Helper PID53/UID-EUID501 records boot
`B58957BF-E9D6-49C1-8715-B656CB42B496`, installed1/placeholder0 and no new
registration attempt. The original ordinary launch completion returns
nativeBOOL0 with `NSPOSIXErrorDomain` code60. Its exact error userInfo is
`_LSFile=LSDispatchUtils.mm`, `_LSLine=147`, `_LSFunction=_LSDispatchWithTimeout`.
This is the original LaunchServices wait's timeout, not the helper's30-second
missing-completion timeout. App execution and Core Location remain unverified.

The one session-bound normal return (USB`9e42affb9d0a65d7`) completed actual
Android/root at16:50:22Z. Maps/pins, both135168-byte prefixes and Data27
generation1 passed; collector36448 exited and the window released
`2026-11-17T16:50:38.602497+00:00`. No phone payload was written. Helper output
is895 bytes, SHA-256
`aedbb3a8f4c66263e8c3ccc637f870eb8c97e74e355b4c118d6ae5dbcb87227c`;
output recovery read6,823,936 bytes. Final console is2,016,027 bytes, SHA-256
`2344f535d2fa2c2bbc2f4741cb5fe3dff23a48382f4f4fb05c42b7d359e1d3dd`.
Process export1,919,812 bytes/SHA25c78372 and collector final1,990,724 bytes
are separately dated snapshots. Evidence:
local project`runs/2026-11-18-ios27-gnss008-gpu051-local001/startup-analysis.json`.
Next inspect the original timeout caller and a same-device working application
open path. Manual authorization/reception remains deferred.


GNSS009 uses the original final ordinary FrontBoard launch service. The
measured GNSS008 error is emitted by lsd's `_LSSpringBoardCall` block3, which
waits10 seconds for `springBoardQueue` at0x186f4a784. Its queued request calls
`FBSOpenApplicationService` with the default-shell endpoint. The helper now
uses that same ordinary `openApplication:withOptions:completion:` service
and empty options. The factory selects the existing shell endpoint; the
request automatically supplies the caller's original process handle.
The actual native block signature is`v24@?0@"BSProcessHandle"8@"NSError"16`.
Callback evidence retains PID/isValid, error and nested error. A success
requires a positive valid process handle and no error. There is one request,
a30-second completion wait and the retained45-second process deadline.
The measured mobile boot task, registration and native008 kernel remain.
Actual-source fixtures cover delayed native completion, nested launch error,
absent/nonpositive/invalid process handles, unavailable service/options and
no callback. Target and device qualification follow this source increment.


GNSS009 computer qualification: CoreLocation009 run44132203083 /
head72881929dd4f788964dfd2f0eb528d30e57c6cb1 passed18 actual consumer fixtures,
arm64e iOS27 and4KiB signatures. Helper SHA-256
`e9e9c36cf51d1fc039cee26b69fcc693b79f9f670b01320be4932702c0e6db65`.
All current trust single/eight-CPU controls, seven secondary CPUs/backlight6,
Windows75, actual entry and retained baseband-reference inputs pass.
The app/cache/boot task/native008 kernel remain identical to GNSS007.

- boot: `1acf57d02f77642c85fe08eecb71ff8c5abb645d3b1868d14d49753a6bf53acb`
- payload: `5e6ed8eaa5b885ec267af9e416b9301320c868380975f3e7fd2fd432fd82692d`
- kernel: `03fb639c817a33b170d227426746662cd958aa95bf722759dbd957ec9133e874`

File queueing follows the current actual phone owner. The next measurement is
one ordinary FBS process/error completion; manual positioning stays deferred.


Local GNSS009 paths updated to the project's I-drive location on2026-11-18.
The unclaimed C-path waiter was stopped before changing the entry. The physical
image/reference copies now live at
`I:/Code/UniXNU-nezha/artifacts/host-trials/ios27-gnss009-gpu051-local001`.
The existing75-file collector package is at
`I:/Code/UniXNU-nezha/artifacts/host-trials/host-r008`.
Boot/payload hashes and every device input remain identical. The exact updated
entry passes the same Windows fixture and75-file check. Linux source/build
paths continue on the project's I-backed Ubuntu filesystem. No C junction or
new C output is used. Shared current-owner and queue order remain unchanged.


GNSS009 actual I-drive trial ran2026-11-18 01:32:56–01:39:58 Beijing,
USB`2d86d656f45bcfda`. Eight successful LOC177 replies remain Ready/error0,
failed0/noSTART/nofix. PID53 mobile helper again records actual boot
`1CA2FE1D-E008-488A-9410-FA85C0D1EEE7`, installed1/placeholder0 and one FBS
request. No completion arrives within30 seconds: `launch_completed=0`,
`completion_timeout=1`; no process handle or native launch error is available.
This run verifies the request path executes, not application launch.
Inspect the original FBS callback queue and command-line event processing
before selecting another helper change.

The once-only normal return reached actual Android/root at17:39:40Z.
Maps/pins, both135168-byte prefixes and Data27 generation1 pass; collector29024
exits and release is`2026-11-17T17:39:58.523660+00:00`. Phone payload was not
written; the actual prior disk payload reference now comes from power091,
`be82c3e099f10c6b43b28d96d433790dfe5f65625f75ec38b50119b844fb1542`.
Helper676B SHA`cbc9b77d60989d81c1d3c13302354bc5de85230fb4c6fbe4226428cde60b2f90`
was read with6,037,504 backing bytes. Console2,039,850B SHA
`cffd1364e438f1a4ed2ecc460526763a1d46d0e60b2bc3d6df621c275d379929`;
process export1,943,869B SHAeeba2ee5 and collector1,967,920B are distinct
snapshots. Evidence stays at local project
`runs/2026-11-18-ios27-gnss009-gpu051-local001/startup-analysis.json`.
Manual authorization/satellite reception remain deferred.


GNSS010 source candidate follows actualGNSS009 originalUI stack evidence.
Main-thread captures at174/234/294/354 seconds remain in `ISDefaults
sharedInstance` initialization wait, called from the stock placeholder icon
and dock layout. Its original init makes `MGCopyAnswer` BuildVersion and
UniqueDeviceID queries. The final FBS callback queue is a separate serial
queue, so changing only helper runloop pumping has no established basis.
The selected ADT has the original six-byte `nezha-wifi-mac`, but lacks
`mac-address-wifi0`; the exact24A446 WifiAddressData getter first reads that
standard field before falling back to an asynchronous IONetworkController
query. The original producer holding ISDefaults initialization is not captured.

Reuse `scripts/publish-wifi-address-adt.py` exactly from account source
`f8588049e4a003e07213ea06e3acb38f0b4b5078`, based on sheng `3e679fbf`
`WifiProbe.cpp::publishMac`. It copies only the existing radio bytes to the
standard `/chosen/mac-address-wifi0` field and round-trips every other ADT
byte unchanged. The GNSS010 comparison retains native008 kernel, helper009,
app, addon/trust, boot task and normal app authorization. This is a small
board-property correction with an actual original consumer; clearing the
SpringBoard initialization wait and launching the App remain device tests.


GNSS010 is computer qualified. Six actual property variants pass. Exact
original MobileGestalt getter code SHAdecb4897 executes the old fallback
path without the property and returns the same existing radio bytes with it;
CF/IO objects remain model stand-ins. The unchanged production DarwinBoot ADT
parser passes under ASan/UBSan on the291144-byte tree and rejects truncation.
All eight FAT members are read back; only the ADT changes fromGNSS009. Exact
kernel/trust QEMU evidence is reused, updated I-drive75/entry checks pass.

- boot: `ef2792b53667cbb49c5c6a8d370ca8b251f44fce0685a212ce35a9ea2129aa93`
- payload: `1fb875c0adff57cbcf62ee6a4a8ba7ea26746ecb9d4ac4cd125b535b7c51ae39`
- ADT: `52c1d1b3356d0710628e3b66b2d4df51e41d1ef0f2306277eae153c30f52ae57`

I-drive trial directory is
`I:/Code/UniXNU-nezha/artifacts/host-trials/ios27-gnss010-gpu051-local001`.
The same helper009 request/process/error log will show whether the original
SpringBoard startup wait changes; App and Core Location acceptance are pending.


GNSS010 human-directed113 run completed2026-11-18 02:22:32–02:30:10 Beijing.
The local attempt stopped before collector/boot when the cable was moved;
it is explicitly closed as unstarted. Same-image SHA and the11375-file
host/5066 transport were qualified before one remote temporary boot.
Eight LOC177 replies remain Ready/error0/noSTART/nofix. PID53/UID501 records
boot`C0A6847F-62C7-4C4C-9D61-14DA0C0FE10C`, installed1 and the same absent
FBS completion after30 seconds. The standard Wi-Fi field does not clear the
observed original ISDefaults main-thread wait. The initializing producer is
still uncaptured; do not treat this unchanged result as a proven MG root cause.
Three current locationd captures again resolve to libdispatch serial drain
+0xc0 (runtime1affe36b8, slide14e8000), retaining the unresolved object issue.

One normal return USB`0315d7aca6a60b70` reached actual113 Android/root at
18:29:36Z. Same135168-byte17/27 prefixes, Data27 generation1/maps/pins,
collector3300 exit and tunnel close passed; release18:30:10.885103Z.
No phone payload was written. Helper677B SHA
`38a37733c63ac5a5072404d242bdc572e0220939ba3bf50b33c6139501e0c524`
was recovered with6,561,792 backing bytes. Console2,476,814B SHA
`89d6bd5997a9e1e3841b6ffc29be34bf2cb29b0223f6ca0c6cb51e5d9d2e45b4`;
process export2,378,532B SHA72b0e7f3 and collector2,405,136B are separate
snapshots. Evidence is local project
`runs/2026-11-18-ios27-gnss010-gpu051-113001/startup-analysis.json` and
`ordinary-launch-ui-stack-symbols.json`. Next capture the actual original
ISDefaults initialization producer. Manual positioning remains deferred.


GNSS011 measurement uses the existing full native stack exporter. GNSS010
STKS worker rows at211/271/331/391 seconds hold the original MobileGestalt
metadata request, synchronously calling CoreTelephony
`getMobileEquipmentInfo:` with real peer CommCenter95. Its original query hash
`QZgogo2DypSAZfkRW4dP/A` matches`InternationalMobileEquipmentIdentity`.
DMS bind0x54 and getIDs0x25 already have successful current-run QMI replies.
The remaining request therefore needs the current consumer's actual wait path.
Replace only `boot-ui-focus3=Preferences` with`boot-ui-focus3=CommCenter` in
this isolated diagnostic package. Keep every other command-line token, existing
observer, GNSS010 ADT, native008 kernel, helper009, app, addon/trust and
boot task. No new tracing facility or protocol is added. Read actual
CommCenter thread waits before choosing a driver correction.


GNSS011 computer qualification is complete. Only the existing observer target
changes from Preferences to CommCenter; command line994 bytes. All other
arguments, eight FAT members except configuration, kernel/helper009/ADT/trust,
App/cache/boot task and the whole BootShim/FD prefix remain identical.
Same kernel/trust QEMU and same ADT parser evidence are reused; fixed remote
SHA/113host75 and actual5066 transport/entry fixture pass.

- boot: `ca05a58f3d47b1e54f20396b41005e5fcd2cdd9e999f5d4eb5ab66a065510f7a`
- payload: `8ab9e7f0cfa8a595d3486432781963b076f8ef0af157ff70c93cab7f4ad2ed31`
- source: `ce4b4687146a0e048a01f35b04392cfa147a9e20`

I-drive source is`artifacts/host-trials/ios27-gnss011-gpu051-local001`;
remote113 image is`C:/Users/Hua/NezhaTrials/n27-gnss011-gpu051-113/boot.img`.
The next measurement uses the current file queue and reads actual CommCenter
waits, then one normal return and protected postflight. Manual positioning
remains deferred.


GNSS011 physically ran113 on2026-11-18 02:52:18–02:59:57 Beijing. Nine LOC177
Ready/error0 replies, no START/fix. MobilePID53/Appinstalled1 remain; helper's
normal FBS request again has no30-second completion. The existing full
CommCenter observer now records PID95 synchronously waiting for its own PID95
through `CoreTelephonyClient getMobileEquipmentInfo:` and the original
MobileGestalt IMEI query. Captures151/211/271/331/391s preserve that self wait.
The exact original udid-version reader uses `/product/udid-version`, defaults
to-1 when absent; the current ADT lacks the field. SameOS D16AP donor carries2,
and original branch2 uses UniqueChipID/ChipID. This identifies the next concrete
board-property correction while preserving all existing device inputs.

Normal return USB`62c78051669af201` reached113 Android/root at18:59:20Z.
Maps/pins and both135168-byte Data prefixes/generation1 passed; collector6956
exited and released18:59:57.579738Z. Console2,670,012B SHA
`30b9703af9fafc95928c718b004015fa4ec90f84731fb3b69af446798f756363`.
No phone payload write; no captured locationd exception is not a resolution.
Evidence:`runs/2026-11-18-ios27-gnss011-gpu051-113001/startup-analysis.json`,
`CommCenter-current-self-wait.json` and`CommCenter-current-thread-groups.json`.


GNSS012 restores only the selected original D16AP profile's
`/product/udid-version=2` (4-byte u32). SameOS donor tree SHA
c7fb97191fa4e7d30f1afca7842010c93aa4e33d992a277b266b9be898aec6d1
carries that value. Original MobileGestalt code at0x1af9f2010 reads the
property from `IODeviceTree:/product`, defaults to-1 when absent and caches
it. The derivation branch at0x1af9f16a8 uses original UniqueChipID/ChipID
when the version is2; the absent-field branch requests IMEI through
CoreTelephony, matching GNSS011 CommCenter's actual self wait.

`board.toml` selects the original u32 property. The isolated package updater
`scripts/publish-udid-version-adt.py` uses the existing ADT parser/common
encoder and checks all other bytes remain identical. Existing machine
inputs, radio addresses, Data generation1, GNSS011 observer, kernel,
helper/App, native code and normal location authorization stay intact.
The original property reader and actual derivation instructions need host
qualification, followed by one device trial of self wait and App launch.


GNSS012 computer qualification complete: six field checks and current291184B
productionADT parser pass. The actual original version-reader instructions
SHAbfbaa35f use old default-1/new2, while original derivation SHA827ae013
models old IMEI query/new UniqueChipID-ChipID branch. CF/IO stand-ins remain
explicit, and real target self-wait removal is pending.
All eight FAT members are verified; onlyADT changes fromGNSS011. Current
I-drive/113SHA/75host and exact5066 entry pass; same kernel/helper009/trust/
cache/observer/FD/args and their qualified native evidence are retained.

- boot: `a6edc256cd102dfd579455332966c04a34255f4b3fbb904263a30138d1a929c5`
- payload: `f8879ba5773dd137ccd31ed1ec9493a957dbeedd81a5298a14b1c134664b5c64`
- ADT: `178b734167ed83e98b15d8d880226460854e820931a5ec87a0f42089ca135245`
- source: `3317a6f3ff497a66561f0ac1b39edde8e4c5cd8c`

Same complete image is staged from
`I:/Code/UniXNU-nezha/artifacts/host-trials/ios27-gnss012-gpu051-local001`
to113's independent trial directory. File queueing precedes one normal
startup/metadata/App test and Android postflight. Manual location stays deferred.


GNSS012 physically ran113 on2026-11-18 03:24:07–03:32:42 Beijing. The original
D16AP udid-version2 removes the observed CommCenter metadata self wait and
SpringBoard ISDefaults main-thread wait. Complete native thread groups70 at
152/212/272/333/393 seconds contain no prior selfequipment query. SpringBoard
mainthread resumes its normal message loop. NezhaLocation actually starts
PID432 at226.660s; mobilehelper53 records boot
`00B74C40-1BCE-4539-9E65-F1674E4D4415` and native completion
`process_pid=432 process_valid=1 error=0 launch_request=1`. This is a verified
ordinary App launch. No manual location authorization or START was requested.
Nine LOC177 state replies remain Ready/error0; coordinates/fix deliveries are0.
Three locationd dispatch continuation-object faults remain a separate issue.

One normal return USB`d712128682593250` was requested; the first60-second
Android observation timed out, then the existing bounded wait reached real
Android/root. No request was replayed. Maps/pins, both135168-byte Data prefixes
and generation1 passed; collector3520 exited, own tunnel closed and release
is`2026-11-17T19:32:42.408097+00:00`. Phone payload was not written.
Helper727B SHA`dc207812127341f3659c6c3979e795b1ca373a65c9ffd84ee2d873e5748fb34d`
was recovered via6,561,792 backing bytes. Console2,621,314B SHA
`85bc4f8293cffc4d1c734546da219207dd1f9e9d6f5027b9b448eb0ce598b9c6`;
process export2,520,680B SHAa8be5e7b/collector2,585,732B are separate snapshots.
Evidence:`runs/2026-11-18-ios27-gnss012-gpu051-113001/App-launch-qualified.json`
and`startup-analysis.json`. Keep this working startup baseline. Next read
same-boot locationd reports to identify the remaining continuation-object
caller. Real Core Location position and same-fix GPS/BeiDou remain deferred.


GNSS013 physically ran on local5037, 2026-11-18 17:23:41–17:30:41 Beijing.
The fixed boot `efe509136ed988cc0c4019143510b06011858436edfabd1c91fde172cf483a67`
uses Native009 kernel `1ec34b457dff639aba28e52bf3bdeb5536fbc7dcf0197105ba566b5099e32a0e`
at source `9bd9af69b1292188b2c82120b4110203e2872a20`. Eight fresh LOC177
state replies remain Ready/error0/failed0; no START or fix. Ordinary App447
executes and helper53/UID501 records valid1/error0 in boot
`037BCCB8-A46F-43E2-B1D1-BACF57F296A1`, retaining the GNSS012 launch baseline.

One locationd70 object exception at111.209s yields item `0x116d825e0`, first
word `0x10000000000`, lane `0x1040de000`, and nearby waiting thread861.
Five return PCs and original call relations uniquely resolve the main image
slide `0xb2b000`. The peer is in CLClientManager database initialization,
calling `initWithActiveAntiEntropy:forDatabase:backedBy:withSettings:`.
The item lies at SP+0x200 in that sampled active constructor frame. These
records identify the current stack use; the original enqueue call and first
word writer remain unproved. A lower exception count does not prove a fix.
Evidence: `runs/2026-11-18-ios27-gnss013-gpu051-local001/`
`object-context-analysis.json` and `locationd-peer-callchain-analysis.json`.

One normal USB return `d31241c31a734a0da2c7da6ce47a7ab5`, session
`1c66508ab8bee402`, reached actual Android/root at09:30:22Z. Maps/pins,
both135168B Data prefixes, generation1 and collector26388 exit passed;
release09:30:41Z. Phone payload was not written. Console2455047B SHA
`c170798c676c1138900d6ccab6b48ae65074c4cd03cf915d4e97eb13fb7b5ba4`
and helper726B SHA
`afb3b37d493314993e97ef5f6a361ac9aacbbf25d024371d7ba59b6d3dc20a2e`
are retained. The user had not completed clicking before normal return.
Manual authorization and reception therefore remain untested. Future actual
boots must be announced promptly; a requested manual action must finish
before trial closeout. Same-boot reports are queued through the existing
bounded Android-only reader, with no restart or manual action required.


GNSS013 Android-only report read002 completed 18:30:01–18:30:52 Beijing.
All25 locationd8192B report prefixes belong to older boot UUIDs; none matches
`037BCCB8-A46F-43E2-B1D1-BACF57F296A1`. The directory-record reader read
1257472 backing bytes/241pages with no error. Same Android boot/root, maps,
pins,17/27 same135168B prefixes and generation1 passed before release.
The first read001 used the old005 reader path and rejected the new claim
before reading Data; the exact invocation is corrected and tested in002.

Manuallocal002 reuses this complete qualified GNSS013 image. Its computer
entry fixture checks advance notification, run/claim-bound manual completion,
consecutive observation indices and no automatic return after observation.
One60s observation slice preserves the phone for the user's ordinary app
actions. Only direct completion of those actions enables normal return and
release. The result reader uses original MCM container metadata and the
existing APFS directory records; the app, helper and kernel are unchanged.
The manual test is file queued after GPU055 and has not started.


GNSS013 manuallocal002 ran18:58:30–19:14:35 Beijing on the same qualified
image. The user pressed start; no original system prompt or UI status
transition followed. Raw down/up contacts and successful HID returns were
observed. OrdinaryApp458 exec and helper53/UID501 nativevalid1/error0 are
verified in boot`096C8BFC-AA28-472D-9482-98F37559DCE3`. Six successful
LOC177Ready/error0 states, no START, position or fix; no capturedlocationd
exception is accepted as a stability fix. The ordinary app could not read
kern.bootsessionuuid, while the existing helper's UUID is available.

One49c1743a8aeb4ca9b3795b12232f68cb normal return onUSB
`0d975b273371dc50` reached actualAndroid/root19:13:07 and released19:14:35
after maps/pins,17/27same135168B/gen1 andcollector75104exit. No phonepayload
write. OriginalMCM appjournal recovery read2342912B and recovered two helper
files; corelocation.json is absent. Console3285539B SHA
`959714fe46c953a290f04e809066c444706d8b301af45eebb992e8f21d171316`
and screenshotSHA
`83bba0d2bf5fedbe5d0b450e4e99ac8ffb268af035aa7d116ff02e60566fc9aa`
are in`runs/2026-11-18-ios27-gnss013-gpu051-manual-local002/`.

GNSS014 changes only the existing observer's boot target to
`boot-ui-focus2=NezhaLocation`. The existingboot-ui-follow1 reads the
application's waiting IPC peer. All other effective parameters, including
console2/overlay0, reset0 andData1 remain. Native009, App/helper009, ADT,
trust, LOC protocol and original dispatch instructions are retained.
The package needs this argument update before the next manual trial.


GNSS014 computer qualification complete at`7f75de85fe46e417ed81985e2746700d7cded47d`.
The997B effective commandline changes one existingobserver option. EightFAT
members and wholebootprefix are checked; onlyunixnu.toml differs. Native009
compiledfocus2parser/32Bbuffer/role-slot andoriginalfollowfunction are verified
in the packagedkernel. Exactsamekernel/ADT/trust/archive/signature inputs
retain strict/PAC/QEMU/loader qualification. CurrentIstage75host andactual
manualentry fixture pass, with notice/runcompletion and normalGNSSpostflight.

- boot: `5bbea694e98bbaf556536b7c12bcec5bb18b42d995679a56a80c8dcb610b0e63`
- payload: `f87b6596d82b4efaf3fa360dc747e833bdbb6550ff47635dd389ee482115a9df`
- kernel: `1ec34b457dff639aba28e52bf3bdeb5536fbc7dcf0197105ba566b5099e32a0e`

Filequeue precedes one manualclick andactual App/IPCpeerstack observation.
GNSS014 is not yet device tested.


GNSS014 ran20:15:31–20:42:27 Beijing. AutomaticHID closed the informational
activation dialog and pressedstart; each action completed9commits/2dispatches
witherror0. This is a separate automatic button test; physicalhumanpositioning
and originalpermission handling remain deferred. App464/helper53/UID501
nativevalid1/error0 are verified inboot`F9C9CEAD-1A6A-4052-97CC-C14980F81882`.

The actualAppmain frames confirm the button callback atoriginalApp
`0x100004c58`, CLLocationManager.authorizationStatus at`0x1a7906768`,
CLLocationInternalClient query at`0x1a7919128`, then a synchronousXPC reply
wait. The encodedexistingMAIN32rows supply these lateframes. Peer464/e5
is the replyreceive side; the serverPID is unproved and originalfollow did
notselectlocationd. ElevenLOC177Ready/error0, noSTART/fix; noobjectexception
doesnotresolve the priorissue. The currentkernel andAppremain unchanged.

OneUSBrequest`dd4b81a22ab545128affaf2b7b7af946` on
`3788f0a9c5a290f5` returnedactualAndroid/root after the firstboundedwait.
Maps/pins,17/27same135168B/gen1 andcollector18196exit passed before20:42:27
release. No phonepayloadwrite. Console13687969B SHA
`60fb18e55f12e74c3b9ddc74f4937693abc6c43748fc9a84c6fc278817c38baf`
andhelper726B SHA
`8bfa339c020a6b70cc0b48573f06a0ace60ac191bdfa2d150a4b885ebcbd3c28`
are retained. Journalread585728B timed out on anAndroidpage; absence is
unproved. Evidence:`runs/2026-11-18-ios27-gnss014-gpu051-manual-local001/`.

GNSS015 directlyselects`boot-ui-focus2=locationd` and
`boot-ui-focus3=NezhaLocation` in the existingobserver. Followselection is
omitted and the unusedinput-test=0 default is preserved by absence, keeping
the line992B. ExistingNative009/kernel/App/helper/LOC/ADT/FD/trustinputs
are identical. The automaticbutton action runs soon after theAppappears;
any normal locationprompt remains for thehuman. Source/API waits are
measured before adapting the server.


GNSS015 fullpackage andcurrentIhost qualification complete atsource
`a1142313d75ecc021a001ac30cdf556f2c7c6224`. EightFATmembers andbootprefix
match; onlyconfig changes the existingtwoobservedroles and retainszero
input-test default. Compiledfocus2/focus3/namebuffers/slots andzeroBSS
defaults were verified ontheactualNative009kernel. Samekernel/ADT/HMAC/
App/helper/archives/FD/trust retain their earlierstrict/PAC/QEMU/loader
results. CurrentIstage75host andactualnotice/claim/observation/normal
postflightentry fixture pass. Filequeued, notyetstarted.

- boot: `ff24bb830801401ef511b6d5ddad7bd249d653c27c1ab607f20fc4501912e440`
- payload: `bc90a560dcf0c1c453c581e11d3eecc550c31bb391254ec75fc50dd59f87d3c9`
- kernel: `1ec34b457dff639aba28e52bf3bdeb5536fbc7dcf0197105ba566b5099e32a0e`

AutomaticHID actions andhumanlocationprompt results are recordedseparately.


The015 Androidresultreader nowfirstchecks the ordinaryMCMidentifier in
the savedGNSScontainer`990C9E88-46EE-4786-9C5E-DD1EF5C3C185`. It reads
the existingappjournal directly when that identifier matches. The exact
reader exercised on retained013Data pages uses651264B and recovers two
helperfiles withnoerror; no phoneaccess orcurrentappchange was needed.
The selectedlaunchcache has730jobs andretains the original
`com.apple.locationd.synchronous` MachService declaration. Original
CoreLocation bindsCLInternalService tothisservice. A missingdeclaration
wasnotobserved; theactualserverwait stillneeds015deviceevidence.

GNSS015 completed on the local host at 21:52:03–22:05:15 Beijing on
2026-11-18. The ordinary App ran as PID433. The automatic start-button
callback reached the original synchronous authorization-status query and
waited for a reply. Locationd PID70 remained in initialization: its main
thread's PC is the load of the original dispatch-once block's invoke slot,
before the once-token check. Several initialization threads have the same
load PC. This is a data-page access wait; the user stack alone does not
identify its kernel page-in caller. Eight fresh LOC177 snapshots remained
Ready/error0 with no START or position. One normal return reached Android
and root, the same-length 17/27 Data and generation1 checks passed, and
collector80632 exited before release. No payload write. The final console
is in `runs/2026-11-18-ios27-gnss015-gpu051-auto-local001/`; ordinary
authorization, same-fix GPS/BeiDou and Core Location delivery remain pending.

GNSS016 reuses the source-page release correction from UniXNU-sheng
`0215f34a6d5307dab53e9a81dff9ae0fd047f3be`, exactly as retained by
account27 `6f00cc304fae49ea6cbb3cffa70ad47135c7c5a3`. The GPU051-derived
kernel lacked this correction. Its 16KiB fixup assembly kept the current
4KiB source page busy while it read sibling pages; a file verification
block can wait for that busy page. The correction copies and releases
that source page and its shadow placeholder before sibling reads. The
existing original-page validation, fixup processing and destination UPL
metadata stay in their normal path.

The exact source blocks pass the retained ASan/UBSan before/after harness:
44 cases per version cover every 16KiB part, shadow/no-shadow objects,
sibling-read and fixup failures, balanced cleanup and the 4KiB path.
The existing stack observer now also retains its already captured kernel
wait for locationd's main thread, so a remaining data-page wait is visible
in the same bounded rows. Native build, full-package qualification and
real positioning are the next steps; the source result is not hardware
acceptance. App/helper, GNSS protocol, GPU051 runtime, read-only root and
Data generation1 continue from GNSS015.

GNSS016 ran at 23:23:10–23:54:11 Beijing on 2026-11-18. The original
dispatch-once data-load wait is absent from the observed initialization
path, and locationd70 reaches its normal run loop. The same App/helper
launch succeeds as PID468. The ordinary authorization query returns
`denied`/`reducedAccuracy`; the App stays responsive and its stop journal
is recovered with the current helper boot and process timing.

The next service wait is now specific: the main run-loop call at
locationd `0x100074d68` synchronously sends a one-way technology-status
notification to its silo. That silo is executing
`CLSeparationAlertsService::onDataProtectionManagerNotification:data:`
and its `CLLocationManager.monitoredRegions` query waits for a reply from
locationd70. Eight full snapshots and later main snapshots retain the
cycle. This is a distinct later stage of the same initialization flow.
No LOC START or fix was produced; eight LOC177 snapshots remain Ready.

The human reports the iPhone status alert repeatedly reappearing after
closing. A normal close action is not a resolution. GNSS016's retained
baseband038 path also repeatedly registers clients: its final snapshot
has 4952 activation registrations and 221/26 dropped frames, versus one
registration and no drops in GNSS015. The exact state-notification cause
must be confirmed in the same-boot native logs; no account or activation
result is inferred or modified. Android/root, protected mappings,
same-length 17/27 Data and generation1 passed, collector63308 exited and
the window released at 23:54:11. Evidence is the actual GNSS016 run.

GNSS017 prepares only the measured one-way controller notification as
an additional existing text-fixups entry. It uses the original `async:`
dispatch instead of `sync:`, preserving the controller capture, original
invoke and normal permission/query results. Eight original-instruction
models cover idle/busy queue and activity branches; external framework
ownership and PAC are modeled. The exact original-page applicator also
passes ASan/UBSan, including wrong file/page/source and repeated apply.
Native010 and the GNSS016 kernel are reused; the new tree/full image and
actual service progress still require qualification. The same-boot log
read is file-queued to diagnose the repeated state alert.

The same-boot GNSS016 system log read completed on 2026-11-19 at
00:16:54 Beijing and released Android with zero boots/writes. Six files
were recovered from 25,509,888 backing bytes. The existing native parser
decoded 1,713,774 records and uniquely corrected the timestamps of all
163,845 selected rows. The retained segment shows 172 new
`SBActivationFailedAlertItem` requests and repeated CommCenter calls to
the system recovery service. The original SpringBoard caller is
`SBTelephonyManager::_provisioningUpdateWithStatus:slot:`; status2
increments its existing failure count and creates a new alert every time.
The exact backend cause of the repeated status remains unqualified.
Locationd also records BigSwitch=0; the ordinary App's denied result is
real and must be changed through the normal system location controls.

GNSS018 prepares two existing page entries for that specific status2 UI
branch. The first failure alert per process launch remains. Repeated
status2 updates return through the original cleanup, preserving the
current presentation instead of dismissing and creating another one.
The failure counter is recorded only when the original UI-ready gate is
true. Status0/1, backend requests/replies, system permission and account
verification are unchanged. Fourteen original-instruction models plus
the first/repeat/new-process sequence pass, and both exact source-page
entries pass ASan/UBSan. With the GNSS017 one-way notification correction,
the inherited table uses 63 of its existing64 entries. Hardware behavior
and functional GPS reception still require the next candidate.

GNSS018 ran on 2026-11-19 at 00:56–01:14 Beijing. All three new entries
were applied in the actual console. The human saw the first status notice
and selected Ignore; no later occurrence was reported, and subsequent App
screenshots remained unobstructed. The service still failed to start:
nine complete locationd70 snapshots retain the main run loop inside
`CLDaemonStatus::onNetworkReachabilityNotification`, at the original
`sync:` call `0x1006ec608`. Several service initialization and authorization
queues wait for silo dispatch. Five LOC177 snapshots remain Ready with
two initialization commands, no START and no fix. The recovered App
journal belongs to the earlier PID468, whereas this helper launched
PID467; its earlier authorization result is not this run's result.
Normal Android/root and mapping/Data checks passed, generation1 remained,
collector82552 exited and the window released at 01:14:46 Beijing.

GNSS019 adds only that observed void reachability callback's original
`async:` dispatch, retaining the original 52-byte block and invoke.
The block carries the daemon's long-lived status object, reachability
reference and flags by value; the invoke reads the status object and
flags. It does not capture a producer stack pointer. Thirty-two original
instruction cases cover busy/idle queues, activity logging and four flag
values; the original invoke preserves those values after the producer
frame is reused. The literal Native010 page applicator passes ASan/UBSan
on the exact source page and mismatch/repeated cases. The optional
consumer entry uses the last of the existing64 slots and reuses the
current kernel. Real service progress, ordinary system permission and
GPS/BeiDou reception still require the new candidate.

GNSS019 ran at 02:22:07–02:44:28 Beijing on 2026-11-19. The new
reachability entry and prior technology entry applied. Locationd70/167/194/245
exited at the original libdispatch queue-item authentication; locationd270
then reached its run loop. The current ordinary App469 and helper boot
AA993479-D8C7-4F23-A2AD-D12CB5B63228 are verified. Authorization is
denied/reducedAccuracy, and LOC remains Ready with two commands, START0/fix0.
The human could not find Location Services in Privacy & Security. The
original Settings provider publishes that entry only after its initial
global enabled/icon query completes; the retained Settings log enters
CLClientIsLocationServicesEnabled and has no completed initial fetch.

The human reported severe heat. From506.405s,34 retained eight-CPU samples
average at least98%. Internal TSENS peaks at105.1C with stage5/5 limiting;
this is not a case-temperature measurement. The same boot records34058
WDS0x4d requests,9283 WDS0xa2 requests,40030 CTL0x2a no-action replies,
and repeated misd/netrb1020/XPC activity. There was no GNSS START. The
initial repeated-request trigger and full CPU attribution remain unverified.

One normal XNU return completed at02:43:38 Beijing. Android/root,17/27
135168-byte prefixes, mapped/pinned extents, Data generation1 and both
collectors' actual exits passed; release was02:44:28. Final console is
42280027 bytes, SHA8559329c7f8607bbe5736e055378b7f3af6155d7724249a2f7c6a939ca6d0187.
The six same-boot log files were recovered read-only and released at02:56:31
with no boot or phone write. The parser process has exited with no saved
terminal result;196857 complete JSON rows and the original trailing bytes
are retained. This prefix is useful evidence, not complete-parse acceptance.
GNSS019 is closed and must not be restarted. Service-query and network
lifecycle repairs precede a new qualified candidate and unplugged reception.
No fresh Core Location or same-fix GPS/BeiDou position has been verified.

GNSS020 retains the same GPU051 and ordinary App/helper and prepares a deferred
region query from the observed GNSS019 service wait. At204/264/324s,
`CLSeparationAlertsService::onDataProtectionManagerNotification:data:` reaches
its original `monitoredRegions` call at0x100a1b558 and waits synchronously;
the tracking service separately waits for the same daemon's internal service.
The normal data notification now captures and retains its current manager on
the service queue and submits the region read to the existing system utility
queue. The producer can finish before the read requires the internal service.
The worker uses the original `monitoredRegions` and `requestStateForRegion:`
APIs and balances the manager reference after processing every batch. It does
not produce authorization, enabled-state or coordinates.

`tools/gnss27/region_notification.py` emits22 guarded entries:328bytes in the
original zero executable padding plus one original branch. The existing
signed-page applicator capacity becomes96 to retain all64 inherited entries
and these22 additions. Its record length, byte guard, file identity and page
boundary checks retain the same implementation. The42 original/deferred
instruction cases pass with explicit framework/queue/PAC models, including
immediate producer-frame reuse, manager replacement and0/1/16/17/33 regions.
Native011 run44479589077 succeeded and all846 encrypted files were verified. All219 runtime objects match Native010. Strict38/348import closure,1062baseband PAC slots, packaged HMAC and single/eight-core startup pass; device service progress remains pending. The final host input follows CommCenter, locationd and misd using the existing four trace slots.

The configuration read completed on the current local cable connection at
2026-11-19 14:26 Beijing, with1,392,640backing bytes, three configuration files,
zero phone boots/writes and protected Android/root release. The mobile
preferences contain CLSilo.Version1 and no custom graph/cohort overrides.
All25 retained locationd report prefixes belong to older boots; none is GNSS019.
The cache boot identifies GPU056 and may postdate GNSS019, so this read does
not prove a GNSS019 runtime queue grouping or repair the older queue-item fault.

The original CTL0x2a is SetPowerSaveMode, whose native response accepts only the
result TLV. Its empty-success payload is therefore not a missing query field.
The original backend's IPv6cause43 completion is available in measured
baseband042/061, but GNSS019's retained failures contain other actual reasons;
that source difference has not been identified as this boot's request-loop root.
The same-boot misd log retains9885netrb1020 publications and repeated connection
creation/cancellation. Complete CPU attribution and the first triggering
network dependency still require evidence. GNSS019 remains closed.

A final worker review found that the earlier fixture modeled only24bytes of
NSFastEnumerationState. The actual state is64bytes (three words plus five
extra words), so the initial code left32bytes uninitialized and overlapped
the item buffer by16bytes. The worker now clears the complete64bytes and
starts its16-item buffer after the state. The updated existing42 instruction
cases use a provider that writes and checks all five extra words, poisons
the worker stack and fills the actual caller buffer for every batch. The
previous code fails that fixture; the corrected code passes. It remains328
bytes and22 page entries, so the same Native01196-entry kernel is reused.
No phone has started the earlier GNSS020 package. The final new ADT and host
package must replace those unstarted inputs before queueing.

GNSS020 completed one full temporary boot on2026-11-19 at16:19–16:33
Beijing. Locationd70 had one original queue-item AUTDA exit; locationd195
then reached its run loop but from202s through743s its main queue waited
in the original CLFitnessTrackingNotifier::fitnessTrackingStateChanged
callback at0x1003d0380. The new region hook was not reached. App511 and
helper boot37BEA6E8-C301-41E3-B4AF-77370E766D35 are current; the recovered
ordinary stop event reports denied/reducedAccuracy and LOC177 remains
Ready/commands2/START0/fix0. After early peak77.9C and transient94.125%
eight-CPU mean, the later17 samples are mostly below1%, with one44.875%
sample; end internal temperature50.2C. The019 request loop did not recur,
but the blocked service and unmatched physical UI mean this is not stable
heat acceptance. One normal XNU return verified Android/root and released
the protected maps,135168B17/27 headers, generation1 and collector62252
at08:32:56UTC. The final13776322Bconsole SHA is
2b199d7af32aaea8cd2d0e6d19d22fa106e1347c8eae8d0c64c048b5bd92129d.

GNSS021 defers only that observed void fitness callback on the original
adapter's silo. The C++ notifier retains its Objective-C adapter at+0x20;
that adapter owns the notifier, and deleteAdaptedNotifier is serialized
on the silo. The callback retains the adapter, submits a48B copied block,
and returns through the original epilogue. The worker queries the adapter's
current notifier on the same silo and executes the original updater only
when it is still the captured notifier; explicit end/replacement finishes
without dereferencing the removed notifier. Adapter retain/release balance
is explicit, and the original state computation, virtual calls and client
notifications are unchanged.32 original/deferred instruction cases cover
busy queues, logging, both states and same/ended/replaced notifier lifetime.
The new144B code adds10 entries after the corrected328B region code, for
exactly96 entries using the already-qualified Native011 kernel. Original
4KiB source pages match their CodeDirectory slots. Actual96-table parser,
full021 package and real service progress remain pending.

GNSS021 completed one protected temporary boot at18:11–18:21 Beijing on
2026-11-19. The actual fitness hook at0x3d0380 applied, and restarted
locationd267's main thread remained in its normal Mach receive loop at
173/203/233/263/533s. The previous fitness notification sync wait was not
seen. The corrected region hook was not observed, LOC177 is Ready with
commands2/START0/fix0. The actual helper boot is
86835278-B3F9-4204-BD55-5C6AE99AB784 and launched valid App468. The recovered
corelocation.json still describes old App511 and is not current evidence.
Late communication repetition recurred:5384CTL0x2a,4024WDS0x4d,856WDS0xa2;
at503.677s four retained CommCenter threads each used150–158permille of one
core and switched1532–3081times/s. This does not account for all eight CPUs.
The final retained internal temperature was76.0C; heat is not repaired.
Final console6090444B SHA0770392d1384b82e607b427f88599b8106ee28bf527e055e3e81b45f675818cd.
One normal XNU return verified Android/root, maps/pin, unchanged135168B17/27
headers and generation1, collector3308 exit, bounded own journal read and
release at10:21:41UTC. GNSS021 is closed.

The next ordinary App increment writes startup/currentPID/session before
constructing its manager. A background public locationServicesEnabled query
records pending and returned events, followed by normal authorization and
accuracy queries. The existing journal writer now serializes concurrent
writes and uses observed cached values; it performs no implicit framework
query, so an incomplete query can retain an honest unqueried state and
startup identity. Authorization-change callbacks also record their ordinary
results even before the user presses a button. No permission or global
state is assigned. The GUI gives steps only after the actual query returns.
The App010 target/signatures and resource package passed. GNSS024 later
verified the ordinary global/auth/accuracy queries below. Helper009 remains
the measured startup entry.

GNSS024 completed one protected temporary boot on 2026-11-19. The ordinary
App483 global-location query returned false in 43ms; authorization returned
denied in 64ms and accuracy returned reducedAccuracy in 4ms. Both original
Mach requests have finite two-second waits, and locationd190's six captured
main-thread groups enter their normal message loop. LOC177 remains Ready
with no START and no fix. The current helper boot is
1FAF4DF6-F733-4675-AA6C-7EC77325E4A3. One normal XNU return verified Android/root,
maps/pin, both 135168B Data headers and generation1; collector64432 exited,
and the window was released at13:21:14UTC. Later CPU mean98% and peak internal
93.4C show that communication repetition and heat are still unresolved.

GNSS025 follows the retained same-boot logs and original reset listener.
At313.962s BootModule reports Booted, registers reset=0 and then reset=1;
the latter immediately signals another recovery. The exact initiating
state has not been measured. A concrete cleanup defect is independently
confirmed: ResetDetection passes nonzero completion0xe020e003 when replacing
or removing a handler, but BootModule's reset completion ignores its int
result and calls requestReset again. The original std::function adapter
forwards that same int in w1. The new16B entry checks w1 before retaining the
callback owner: unsuccessful completions return, result0 continues the
entire original reset path. Detection and recovery for a real event remain.
Twenty-four original/corrected completion cases, five original scalar
queries and six real parameter-forwarding cases pass with platform calls
and PAC modeled. They do not prove phone heat or GPS acceptance. Two
existing signed-page records require98 total entries; the sole kernel
change raises the applicator capacity96 to98. Original system pages,
App010/helper009 and all GPU051 runtime inputs are retained. Native012 run
44553906616/headcd8d804 completed successfully; all846 files were received,
and219 driver objects match Native011. Strict38/imports348, baseband1062PAC,
GPU168/display379/HID207, three resource bindings/HMAC, actual98-page
application and single/eight-CPU checks passed. GNSS025 boot1e3e48f242d0 is
retained as the complete computer baseline; it has not been booted.

App011 adds two explicit settings buttons using UIApplication openURL: the
original system LocationServices route and UIApplicationOpenSettingsURLString
for this App. The returned boolean is journaled; successful URL submission
is not treated as a permission or service change. Returning to the scene
refreshes the ordinary global/auth/accuracy queries, with one query in flight.
Normal authorization remains on the system UI. App011 run44565081223/head
56b9c856 succeeded, with36 files,25 App signature pages and18 helper pages
verified; helper009 is byte identical. GNSS026 boot1b70fc4a2fda/payload
ee94320d09ce uses the exact GNSS025 kernelced68f7e185d and ADT98, changing
only addon/trust members. The I-drive host75, actual lifecycle and bounded
journal/service readers passed before026 replaced the unbooted025 queue
entry. Phone service/reset-completion/heat/settings-page behavior and real
GPS/BeiDou positions remain unverified.

The saved024 console shows sustained late CPU mean94.7%, peak internal93.4C
and19327 board control calls at490s. Baseband061's separate saved run has
late CPU mean17.45% and only19 control calls, but is not a same-scene or
same-candidate performance comparison. Exact061 boot query, control header,
NbBoot and bridge glue match the GNSS source; its newer WDS failure
completion differs. No unsupported control change was copied. If026 still
recovers repeatedly, inspect its original ResetDetection initial query and
same-boot notifications first. The service initially caches unknown2 and
updates only from the real controller query or event; its actual initiating
state is still missing evidence. Current access follows PHONE-WINDOWS.md.

## GNSS035 reclaimed page result and GNSS036

GNSS035 built on Native019 (44768351599, 806f6ef) and retained142entries,
App012 and GPU051. Its first boot stopped before any START; the human
restarted to Fastboot and requested the exact same image again. The second
boot returned only services_query_pending in current App528, with no
START/fix/reference command. One normal return, current Android/root,
inodes/extents/pin and both135168byte Data headers passed at17:17:06UTC;
the second collector10644 and first69196 both exited. Full console and
ordinary App evidence remain in runs/2026-11-21-ios27-gnss035-gpu051-auto-local002.

The original10d6fc hook was applied early, but subsequent locationd609
again waited at original10d888. Native019's compiled bitmap-hit path
(vm_page_validate_cs_fast) returns a newly read validated page without
cs_text_fixup_page; only full validation applies selected byte changes.
The vnode bitmap survives reclamation. GNSS036 reapplies the existing
original-byte matching table after a successful bitmap check, before
mapping, and synchronizes instruction cache only on actual writes.
Tainted/writable pages and failed bitmap checks keep their original paths.
This repairs a demonstrated host re-read gap; the same-phone runtime
benefit and the earlier dispatch object lifetime issue still require
actual verification. No existing GNSS callback or reference fix is removed.

Source: existing XNU validation path, original UniXNU text applicator and
current GNSS035 same-boot snapshots. Upstream8542980/myron-nextf1a1bfe
were fetched; their text applicator has the same missing bitmap-hit call,
so no complete replacement was available. Tests extract the actual
vm_fault and ubc source:11 validation/cleanup scenarios and a local
142entry/12signed-page/60case re-read comparison passed ASan/UBSan.
The actual native kernel and full phone location chain remain pending.

## GNSS037 lock-state notification

The full saved GNSS035 stacks independently identify onLockNotification
(original6ecc70) in locationd264, before later CTsync in609 and the old
technology sync in707. The original callback ignores notification inputs
and invokes original6ed2c0 through a72B copied block; the status is allocated
once and stored in the original process-global slot. GNSS037 retains this
original block and uses the same async silo entry, preserving the lock
notify_get_state and event11. Sixteen original/deferred instruction cases
passed; parameters and callback stack can be reused before delivery.
Native020b's reclaimed-page fix is built and qualified, and all219driver
objects match019. One additional table record needs143capacity; GNSS036
remains an untested142entry intermediate. The next complete phone candidate
is GNSS037 with both repairs. No GPS result or stable service claim yet.

## GNSS047 fatal waiter snapshot

GNSS046 retained early faults still consume queue items inside a caller's
reused stack. The saved current sync context and the fault item differ by
1744 bytes; the current context contents and kernel waiting request were
not recorded. Reviewing the actual Native030 workloop continuation, syscall
return and pthread workqueue stack setup did not identify a concrete early
return defect. A static review does not exclude a runtime race.

Extend the existing locationd-only, at-most-three fatal recorder to retain
waiting thread registers, its actual wait event/result/hint/continuation,
the original syscall arguments and the typed kevent wait continuation.
The BSD helper copies only scalars under the existing task and scheduler
locks, rejects other continuation/union states, and never dereferences a
knote. User stack/TSD/current sync-context and dyld-info reads remain outside
those locks. Current TSD context pointers are sampled twice; neither pointer
stability nor static checks establish object lifetime correctness. Terminating
and runnable threads are rejected. Original fatal handling is preserved.

Source: the existing GNSS013 fatal recorder, Apple XNU12377's original
EVFILT_WORKLOOP and libpthread539.100.4, checked against Native030 linked
instructions. The actual sampler/helper host fixture passed ASan/UBSan for
256 thread states, typed continuation/union gating, failed reads, context
bounds and reentrancy. This change adds missing evidence; it does not yet
repair the queued object. The176 page records, all GNSS046 notification
changes, GNSS043 real measurement/NavIC compatibility and App012/GPU051
remain unchanged. First-start stability and repeated satellite fixes still
need phone verification.

Native031's first target build reported four compilation errors: the Mach
waitq field is a transparent union, and BSD's thread type is opaque. Read
the actual wq_q member and pass the already locked wait hint, continuation
and parameter as scalar arguments to the BSD helper. Its original typed
kevent gate remains identical. The host fixture now uses the real waitq
union shape and also compiles the BSD helper with only a forward thread
declaration, so it cannot hide private-thread accesses. The original failed
build and its 62 verified CMS files are retained. This corrects target
compilation; it is not a new queue lifetime fix or satellite acceptance.
