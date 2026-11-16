# nezha iOS 27 基带与蜂窝数据：源码交付与静态审查

任务：`docs/handoffs/IOS27-BASEBAND-DATA-VOICE-CODEX-20261107.md`。分支 `codex/baseband27-20261107`（基于 `codex/main-ios27@675500a`，仅本地，push 已禁用）。
第1–9节保留原构建前审查的硬件与来源。人类已在
`docs/handoffs/MSS-AUTHORIZATIONS.md` 第197条选择 GPU024 并授权持续适配；
当前接续进度见第10节起及私有 `research/CHECKPOINT.md`。



## 038：IPv6-only拒绝回复补齐原生IPv4收尾（2026-11-16）

037主卡IMS已真实建立IPv6承载并取得两个P-CSCF。IPv4 Start回复为
QMI_ERR_CALL_FAILED/14，原reason1、verbose type6/code51，即3GPP的
“IPv6 only allowed”。原iOS27 `handleActivateResponse`对3GPP失败等待
PacketServiceStatus indication；本机这次失败只在回复中出现，因此IPv6
已经Active后，汇总上下文仍等待IPv4处理到420秒。

`bridge/NbWdsCompletion.h`只把这条真实失败投影成原WDS0x22断开完成事件。
必须匹配同slot/channel/cid的pending IPv4 Start事务，绑定/mux/family已稳定，
并同时具有原result/error、call-end和verbose字段。先转发原失败回复，再补
status1/reconfigure0、原reason、原verbose及family4；不产生连接成功、handle
或地址。成功回复、其他原因、IPv6、已active、错事务和畸形/重复关键TLV
均不投影；原Start清pending后相同回复不会再次投影。

来源为已有QcomBaseband事务/队列接口、libqmi0x22字段，以及037实际同启动
日志。新宿主测试在ASan/UBSan通过，覆盖原回复及同mux已active IPv6保持。
原24A446消费者指令模型覆盖失败先到、IPv6成功先到以及错误family三组：
原parser保留type6/code51，原错误映射得到CSI95；IPv4原状态经ActivateFailed
收尾到Idle，IPv6保持Active，原aggregator随后调用IPv6 activateProcedure并
清除等待。日志、TLV查找、共享所有权、部分清理副作用和observer边界建模；
没有把该电脑模型当作实机PAC、IMS注册或通话验收。

本轮再次fetch上游UniXNU4a654d1、myron-next f1a1bfe、QcomBaseband dd0199c、
Trogdor7e0a044及data-a-live d2c7089，无可替换该IMS链的新增实现。038需新
原生编译；封包继续GPU024、037全部12项fixup和原033 helper/cache/signature。
当前尚无038成品或手机预约，构建、严格闭包、PAC及单/八核检查后再按原文件
队列安排一次具体IMS验证。

## 037：IMS使用的通用上下文票据选择器（2026-11-16）

036完整同UUID日志已经证明蜂窝HTTPS，并把IMS首个阻塞定位到上下文未分配。
两卡VoLTE功能最终均为ON、Pref/SimFiles/MSISDN/DeviceID为1，Connectivity及
DataContext为0。原IMS APN由运营商配置读取，主卡IMS、次卡ims，均为类型
S/0x20000和IPv4v6；没有猜测或覆盖APN。117.559/117.563秒两卡IMS偏好已经
从Any切回BB/Voice+Sms，但ConnectionAvailability仍为kDataNotSupported，
没有IMS承载或注册成功。两卡BB短信原生Ready已到达；系统自动VVM RawSend
142.367秒真实返回QMI_ERR_NETWORK_NOT_READY/0x35，普通短信收发仍未验收。

原DataConnectionIM的认证vtable `0x1022f5bd8+0x428`，原值
`0x80092a8900096228`，以PAC标签0x2a89调用通用选择器`0x100096228`。
该函数的第二处票据条件`+0x9635c`与034互联网专用`+0x934d0`相同。
两个原getter仍读同DataServiceController次对象+0x31a/+0x104，分别为
原Apple postponement ticket与postponement状态；无票据且postponement为真
时原选择器将context id写为-2。034只修了互联网入口，未覆盖通用入口。

037配置`integration/native-generic-postponement-context.json`固定原24A446
CommCenter SHA/UUID，在原11项chosen表之后追加
`CommCenter,9635c,60000036,01000014`。这只选择现有无票据继续路径；原
上下文树、BB/Tech选择、setting可用性、activation/provision、APN和WDS
错误/结果保持。原033内核/helper/cache/signature、035双实体SIM与036无C2K
profile修正沿用；源码修改不包含新的射频或持久配置写入。

原caller、通用guard、真实字段getter及树选择指令执行9组通过：036无票据
场景从-2继续原context id0；缺失setting和不可用setting仍不可用；票据已有、
无需postponement、Tech原bypass及BB/Tech共存保持原规则。PAC与外部setting
接口明确建模。原编译signed-page解析/应用器恰好4字节更新及拒绝路径在
ASan/UBSan通过。只读`context_ticket_fixup.py --config`输出与第12项一致。
这些电脑检查不证明IMS承载、注册或通话；037封包及真机结果随后记录。

本轮刷新UniXNU `9931232`、QcomBaseband `dd0199c`、Trogdor `7e0a044`、
ShueiTsuki/data-a-live `d2c7089`、myron-next `f1a1bfe`和myron-device `d9b6be4`。
QcomBaseband与data-a-live未新增IMS源码；data-a-live的pandora仍采用无蜂窝
CommCenter路径。Trogdor增量仅启动后DMS低功耗切换，本机036已注册/联网，
不将其混入此次修正。既有GitHub全量相关检索及fork范围保留在私有
`research/github-search-20261114`；新证据在`research/upstream-20261116-IMS`。

037封包源码`7869f99062b6af723ce9be68d7429cc733d547a5`、编译源码仍为
`6a2eef8`；boot SHA
`2fe1fee237c002ee04aac1a0af6259f22c8d694b506b40b9338e8492841ff8f2`，
payload SHA `91ecc1d31c273ffbf6f634ca4f145710bd97522672e49cdf5fe0143821d4dfad`。
八个payload成员完整读回，只有ADT与菜单标题变化；原11项后追加第12项。
内核02245d45、addon/trust/Data seed/helper/cache逐字节保持；固定reader仅更新
对应摘要，其余28个固件模块与shim保持。Linux/Windows具体入口、原9场景
执行链、本机75个host-r008文件以及原APFS与只读领取/释放检查通过。

local001于10:20:21 UTC领取未使用预约，首次安卓命令被本机5037未监听拒绝
（10061），执行器57088/唯一等待31531退出1并撤销预约。没有payload写入、
collector或fastboot boot，失败原run保留。随后在原Windows backend短锁内
核实际无拥有者/无手机客户端/原执行器已退出，只启动缺失的主机ADB server；
真实c7fea50c以device状态枚举，未重启手机。新local002入口0720b67d及9场景/
只读/APFS/本机75项检查重新绑定同037成品后排队；local001失败保留。
local002实际于10:33:47.957506 UTC接受一次临时启动，执行器57408、collector
57172、session bfd351d4fe42771c。唯一正常XNU返回后，安卓/root、Data与映射-pin
及采集器退出后验通过，10:41:24.934581 UTC实际释放；内核最终424.140460秒，
CommCenter退出事件为0。没有重放失败local001。

独立只读窗口取回17,920,000字节，10个日志文件共16,468,400字节，
10:56:17.618349 UTC同安卓boot/root/Data/maps后验释放。原017 metadata目录
消失曾使第一次电脑解析失败；沿原receipt恢复735个文件并逐项SHA核对后，
第二次解析完成。531308条全部同boot UUID
162DC2E8960C40879B35A7BFC3A42D76，75740条相关记录全部唯一625/12校正，
consumer_errors为空；原失败flow保留，未再读手机。

蜂窝HTTPS再次通过：216.153912秒一次required-cellular请求，原TLS校验保持，
216.355218秒发送106字节，216.541138秒收到203字节/HTTP204，随后明确
HTTPS_cellular_complete=0、cellular1/pdp1，helper正常exit0且未重启。
主卡IMS119.598292秒可用，119.621479秒实际startDataCall APN=IMS；IPv4
120.346020秒被IPv6-only/type6/code51拒绝，IPv6 120.414273秒成功、
120.416553秒Read2Proxies、MTU1500，120.416580秒变Active。汇总IMS仍
Activating/DataContext0/Proxy0并等待IPv4到420秒，此为038具体修正目标。
次卡未附着/provision条件独立保留。两卡原生短信Ready不等于普通短信送达；
本轮没有普通短信发送、IMS注册或真实语音验收。原037试机、只读采集和
完整分析均已结束，无本模块手机拥有者。

新增只读`tools/baseband27/ims_diagnostics.py`把同启动的原IMS功能/mask、
上下文ID/连接可用性、注册gate、原IMS APN与短信Ready/发送结果整理成证据。
全局IM缓存行没有slot字段，工具保留slot未知；不会按前一行猜卡号。
5个必要场景覆盖Ready与注册/送达的区别、两卡及关机前mask、无上下文、
异启动/未校时拒绝及空日志。实际036的76342条相关原记录解析得到两卡
无IMS PDN，3次自动VVM发送及3个真实error0x35；没有普通短信/通话验收。
本工具不启动手机请求，不改变已排队037镜像。

## 036：C2K能力与本板profile范围一致（2026-11-16）

035已实机解决两实体卡subscriber Ready及主数据服务Provisioned传递，下一首个
失败是3GPP2 profile创建0x51后同时重置两类profile并重复申请。原
`QMIDataContextIPAggregator::createProfileIfNeeded`的stub0x250098e60实际跳到
`capabilities::ct::supportsCapabilityC2KDevice()`，不是相邻eHRPD能力函数。
本机原映像精确读回stub和入口，libTelephonyCapabilities UUID40dce84e…、
cache.19偏移0x4fc649c、原文`7f2303d5f44fbea9fd7b01a9`已核定。

原C2K=false分支把3GPP2 profile状态置0xfe（本类原“不支持”哨兵），不调用
3GPP2创建block；3GPP创建、原索引/错误及IPv4/IPv6路径保持。本板现网与此前
eHRPD fallback未支持的记录一致，036在原能力入口保留PACIBSP，用mov w0,0和
RETAB返回，选择现成分支。`integration/native-C2K-capability.json`声明该本板
修正，只接到原10项chosen表之后。文件/root/Data不写，不回假profile或地址。

6个原指令场景验证原C2K=true/false、本轮能力返回、已有profile、请求进行中及
driver未就绪；只有未建立且就绪时调用3GPP block，0xfe由原代码产生，栈和
3GPP索引保持。原profile异步send、PAC和日志外部接口明确建模。原签名页面
解析/应用器对12字节入口和拒绝场景在ASan/UBSan通过。固定033 kernel、helper/
cache/signature/GPU及035两卡状态修正均沿用；新的payload、boot与本机入口/后验
部署资格随后记录，真机WDS Start、地址/DNS/TXRX/TLS与短信/IMS/通话仍待验证。


036成品已完成：源码配置`3c16ed271e2e996fbd42f4a1c42af34cd84f3b65`，
仍复用编译源码6a2eef8/kernel02245d45；boot SHA
`2331436e99f260be8a91ec4918b6d1b21bfc534a0e2bf41ac43aade427ea7163`，
payload SHA `51338703e47194d9ce436195da6cf981c8c7f3af73203e29f54b1243dcd52496`。
所有八个payload成员读回、原十项表保留并追加第11项，其他28固件模块/shim保持。
Linux/Windows入口f45a0639、本机75文件部署、9个完整执行链场景及APFS/只读窗
资格通过。所有相同kernel/helper原生、导入/PAC、HMAC及控制资格沿原摘要复用。

035完整只读释放后，036 sole229254/session54542按原共享Windows锁领取；
执行器63408/nonce7cd9d5d5核原映射、Data和授权payload写入，采集器46724启动。
08:16:56.114412 UTC一次fastboot boot接受，finisher236250沿同一400秒观察→
正常USB返回/机器后验→独立只读targetUUID→完整分析链运行。新能力修正、
3GPP profile配置/WDS启动及蜂窝HTTPS仍待实际结果，不升主包。


036本机实机已完成425秒观察与正常返回，最终kernel430.049970秒；完整控制台
1840712字节/SHAf2959c2d，CommCenter零退出，双卡/MSS/IPA与Data保持。第11项C2K
能力修正在101.896503秒应用。只有一次3GPP profile创建，117.576888秒真实error0；
IPv4/IPv6 Start分别在118.117119/118.177296秒真实成功，GetCurrentSettings在
118.119722/118.180912秒成功、分别含地址字段。pdp_ip0 family-mask3、TX5225、
RX55446、drop110，其他口无流量。035的3GPP2错误/重建循环在此控制台中消失。

helper自己的TCP也有实证：cellular-demand0:52在pdp_ip0进入ESTABLISHED，
完成4698/1728字节收发。单此TCP摘要不能证明TLS认证或HTTP204；仍以原生日志
和helper请求结果单独验收。一次正常USB请求ff8f456b/sessionff98d949/seqc2后，
08:24:36.080998 UTC安卓/root、Data17/27同135168字节/generation1、映射-pin、
当前payload513387和46724实际退出后验通过释放。随后独立只读036日志窗已排队，
唯一等待240854沿原camera071-GPU051下一窗口，当前相机在使用手机。
本轮实际UUID、地址/DNS、helper TLS/HTTP和IMS状态仍待完整读取；不重复036启动。


036复用源码交付增加 `tools/baseband27/c2k_capability_fixup.py`：只读检查本机
cache.19的dyld映射、原libTelephonyCapabilities Mach-O UUID和12字节能力入口，
输出第11项NUL结尾表项，不拷贝或修改Apple程序。原UUID/入口和显式映射模型
核验通过，错误UUID/原文拒绝；实际cache文件4fc649c位置另有原精确读回回执。
工具与本轮成品表项逐字节一致，已上机成品保持，不另封包或重编。


036只读日志001于08:46:56 UTC领取并唯一选中
`39A17A98E916449EA958DCBD37D6930E`；10186752字节原始页中已完整保存timesync和
Special02fc/02fd/02fe，目标Persist读取ADB超时，57956退出1。其Data后验也超时。
沿原nonce45422077完成一次本机ADB连接/空闲server恢复，未重启手机；原安卓boot、
17/27 Data和映射复核后，08:55:53.058021 UTC实际释放，失败capture保持false。
这三个Special经长度/SHA/UUID核对，解析152410条同UUID、910条唯一校时；部分
结果不代替全启动网络验收。

002补采集复用已合格033的Special精确复用逻辑，11项原APFS函数保持；以实际
036的三文件验证零设备读取复用，原wall/finalclock再次选中同UUID，Windows
领取/后验/保留其他拥有者检查通过。随后读者55068正常退出，新增14049280字节
只读页，10份文件合计18205592字节（含三Special复用），完整Persist0185及实际
cache/signature/helper和UUID元数据取得。08:59:38.019906 UTC同安卓boot、同长度
Data、映射及读者退出后验通过释放。完整原生日志和HTTPS/IMS结果随后电脑解析；
本轮boot/kernel/helper配置不变，没有重复036启动或Data初始化。


036完整补采集的608832条原记录全部同目标UUID，76342条相关日志全部625/12唯一
校时、consumer_errors为空，JSONL SHA
`bf6d7dbc121bc1f0cd9fb3017cc44e9d7e0429f3f758c847b1cb0e742838af5b`。
实际三份cache/signature/helper再次逐字节同033。helper UUID7F5FFE9A、PID52/euid0
在94.644546秒进入，chosen4；215.956364秒发一次required-cellular请求，
217.161658秒在pdp_ip接口发送106字节，217.387431秒收到203字节HTTP204，
217.387444秒明确HTTPS_cellular_complete=0/cellular1/pdp1。原TLS默认验证路径
未改，217.391858秒正常exit0，无重启；所有旧guard/channel/DNS Mach、本地
EPERM、ServiceNotRunning和NoNetworkRoute为0。第一次实际蜂窝HTTPS通过。

两卡SIM和主数据Provisioned保持，一次3GPP profile及双栈WDS Start/运行设置
成功，PDP实际收发维持。此结果证明本轮原生数据可用；持续时间约7分钟，
断开重连、长期稳定、短信收发、IMS注册及双向语音仍单独验。IMS活跃mask的
SimFiles/MSISDNPresent/DeviceID为1，Connectivity/DataContext仍0，下一适配
聚焦IMS上下文和配置服务。036试机、两次日志窗口及完整分析均已结束，
手机窗口释放，无本轮未完成进程；WiFi023保持原驱动，不升级稳定主包。

## 035：双实体卡的原生 subscriber 状态传递（2026-11-16）

034真实UIM响应已将两卡报告为Ready；subscriber却从114.557秒起一直发布
Detected，PP的SIM-settled检查持续等待。原PP模型的SIM信息树因此不能形成
可用的每slot服务映射；原数据服务+0x118配置检查返回-5，Internet尚未开始WDS。
必要SIM文件均有完成结果：两卡ICCID/IMSI、主卡GID1/GID2已收到，次卡缺失GID
按原not-present成功收尾。完整034记录保留，没有重复试机或重置Data。

原subscriber有效状态函数在+0xb1的启用/禁用状态尚未知时，走+0x98 ICCID检查，
非空也返回Detected；已知启用则继续原真实卡状态、必要文件完成和交叉映射检查。
实际data.sub只得到当前slot kOne，反复记录No requested ICCID set，尚无可用槽位
配置结果。原数据订阅又要等subscriber的SIM稳定；本板双实体卡没有Apple槽位
配置服务作为读卡前置。035候选只把unknown条件分支的目标改到原读卡完成路径：
`CommCenter,474a0,a1000054,e1010054`。原driver Unknown/Detected/Locked/Blocked/
Dead/Inactive等状态、已知禁用、PIN/PUK与必要文件等待都继续原逻辑；没有把
有效状态函数改成恒定Ready，也不写SIM启用、运营商配置或射频参数。

来源为034同UUID完整日志及24A446原subscriber、PP和DataSubscription接口，
沿用QcomBaseband的实体卡读卡/激活桥。`integration/native-physical-sim-state.json`
固定原CommCenter完整SHA、UUID、四字节指令和本板范围；复用现有
`tools/baseband27/context_ticket_fixup.py --config <native-physical-sim-state.json>`
只读生成第10个chosen表项。保留034原9项。22个原指令场景执行通过，覆盖
unknown槽位＋真实Ready、已知启用/禁用、各原卡状态、必要文件的0/1/2/4等待、
交叉映射及无driver状态；原字段与栈保持。PAC和外部getter/枚举明确建模。
033原签名页解析/应用器对第10项的准确四字节、错误vnode/页、重复应用及跨页
拒绝在ASan/UBSan通过；这两类电脑验证不代替真机Ready→Provisioned→WDS。

035预定继续使用033已合格的内核、helper、cache/signature/trust和GPU024，
只封包新增ADT表项与菜单标题。WiFi023保持原驱动。成品摘要、真实页应用、
SIM发布、数据服务Provisioned、WDS Start、地址/DNS/TXRX/蜂窝HTTPS及正常返安卓
由本候选分别验证；数据通过后再安排人工短信、IMS与双向通话测试。


035已封包：源码配置提交`9ae859a8814f5f102bc420a5fca581206046f12b`，
编译源码仍`6a2eef8bc8b0a374d80f86a1e356310243af9d84`，kernel02245d45同033。
boot SHA `7ded71d335413e57d5e7c121ea363cfa408d2d33751118108c1070ccb55b9149`，
payload SHA `132ad037f4a474dee2fec5352cccd4f4fceadbed98f6e36337a97beb8bd300d7`。
八个payload成员完整读回，原9项fixup后追加第10项；其余成员及28个固件模块/
shim保持。原033 strict38、275导入、1062基带PAC、HMAC和一/八核控制按相同
kernel摘要复用，无新CI。Linux/Windows具体本机入口a9c74a13、9个完整链场景、
本机75文件部署以及原APFS37块/100页和只读领取/释放检查通过。

第一次035本机窗口在07:25:45 UTC尝试安卓入口，ADB明确报告设备c7fea50c
不存在；原执行器60516已撤销nonce0c5209c8预约，唯一等待181124正常进程结束
码1。此run只保存host-stage、引用、claim及失败命令；没有payload写入意图、
collector、Fastboot启动或正常返回请求。已有电源082的真实后验引用保持。
一次只读电脑诊断07:27:26 UTC确认Windows PnP无手机、ADB列表为空；没有
重启ADB或复放启动。035人工连接需求后移，共享记录保留其他模块的下一窗口。

额外20个原指令场景验证PP每slot映射查询及原DataServiceController通知，
经次对象+0x18/slot1a0→原thunk→BBDataDriver+0x118；原raw attach和另一卡字段
保持。无SIM映射仍返回Unknown；两槽重映射保持对应服务值，只有Provisioned
状态的实际变化才通知bool。registry、上下文保存及PAC/外部回调明确建模。
这些结果补足035的预期传递链；实机Ready、Provisioned及蜂窝数据仍待连接恢复。


原subscriber Ready→PP映射又完成5个原指令场景：Detected2不写模型树，Ready5
通过原isSimReady/isSimSettled后调用slotb8，将硬件slot与SIM状态写入+0x2b0；
原模型slotc0读回和服务查询保留每卡Activation值。单卡尚未稳定不填另一卡，
交换逻辑/物理映射也保持对应值；后续Activation流程外部调用明确建模。
联合前述22和20场景，已完成47个SIM状态到数据服务接口检查，无额外内核修改。

手机恢复连接后，音频105于07:37:45 UTC实际接受临时启动。035另建local002
run以保留未操作的001失败证据，复用同一成品；具体入口e5b55c35在Linux/Windows
验证，独立9场景执行链及本机75文件部署通过，只读窗口a6aa98d3/reader5ac70bb0
保留原APFS算法。唯一等待197268/session12170按共享文件排在音频105实际后验
释放之后；没有重复001或干预音频拥有者。035真实SIM/Provisioned/WDS效果待测。


035实际local002已完整上机及只读结束。07:45:04.460670 UTC一次fastboot boot接受，
425.753232秒完整控制台3237081字节/SHA8815481d；MSS/IPA双卡保持、CommCenter零
退出。票据+934d0和本轮+474a0分别在107.396409/110.312794秒应用。一次正常USB
返回后，07:52:53.964499 UTC安卓/root、Data17/27同135168字节与generation1、
映射-pin/当前payload132ad037和16268退出后验通过。独立只读窗07:53:58.031618
UTC完成同安卓后验释放，读者45536 exit0；原始29986816字节、11文件28119448
字节。1491840条均为UUID743AD800B0D04618A85CAF79B5CBA25A，969953条相关记录
全部625/12唯一校时、consumer_errors为空；JSONL SHA
`6e46f4063a69d90a4a51093c1f8183c83536ede88e302f4b3912650f800a161e`。
实际cache/signature/helper仍逐字节同033。完整控制台有helper自己的exec，
本次原生日志没有helper UUID/BB21入口请求退出，不能据零错误行称helper验收；
后续保留这个可见性限制。所有035执行/返回/采集/解析进程已结束exit0。

真机推进已证：主卡122.902359秒、次卡122.906017秒发布Ready；PP分别写入两卡
SIM信息映射，主数据服务123.184552秒Unknown→Provisioned，次卡NotProvisioned
单独保留。Internet.1没有context-unassigned或Data-is-not-provisioned拒绝，WDS
开始创建profile；IMS原SimFiles/MSISDNPresent/DeviceID三字段均已从0变1，
Connectivity/DataContext仍0。这些控制进步尚未建立蜂窝数据、短信或通话验收。

新的第一个失败是124.253177秒3GPP2 profile返回0x51/QMI_ERR_EXTENDED_INTERNAL，
原callback同时重置两类profile，随后密集重建：已捕获2620个Create请求/2618
回复，16个真实3GPP成功、2602个error81；原生日志有2602个3GPP2失败以及
2587个后续3GPP失败，不能把首个3GPP2失败与后续容量耗尽混作同一原因。
没有ModifyProfile、StartNetwork或GetCurrentSettings，PDP TXRX0/HTTPS未通过。
下一步核原C2K能力查询与原3GPP2可选分支，使能力范围匹配本板LTE/NR硬件；
不伪成功、不猜APN，保留3GPP的真实profile创建结果与数据接口。

## 034：复用上游的 Internet 上下文票据兼容分支（2026-11-16）

033完整原生日志确认网络权限拒绝已消失，但Internet自130.396秒起仍未分配
上下文，没有WDS Start。沿原24A446 CommCenter上下文选择调用链
`0x100092bdc → 0x100092a94 → 0x100093490`，对照QcomBaseband
`dd0199cf3300764ed52b23f4a47761f91fab21a2`的
`platform/coachz/tools/coachz_cellular_compat.py`复用非Apple基带的
postponement ticket兼容逻辑。上游17的原分支继续正常上下文选择；034固定
27原CommCenter UUID `3e3091d1-178e-3f0b-99da-5382317ea990`及SHA
`aa96ad9197abf1bbb53e053803ea01eb4c724941dcaaafe2f0f6f49a63fe628c`，
在`integration/native-postponement-context.json`声明唯一四字节修正：
`CommCenter,934d0,60000036,01000014`。保留原PACIBSP、RETAB、栈、
上下文输出、APN及WDS结果，不生成ticket、激活成功或网络地址。

原17、原27及兼容分支的四组原指令执行通过；PAC和外部虚拟getter明确建模。
033已编译的原生产`native-text-fixup.patch`解析/页面应用函数提取后，
恰好四字节应用、错误vnode/页、NULL、重复及跨页拒绝在ASan/UBSan通过。
电脑结果只证明对应分支及原应用机制，不证明实机网络成功。

034封包在原8项chosen/text-fixups之后追加上述第9项，只更改ADT及菜单标题。
原033内核`02245d455f29501bfc32d6c45ec2d6671e6a2284daa3f28cf5e78dfcdeac8dc9`、
helper、729任务缓存/签名、trustcache、GPU addon、scratch Data、根映射和
boot args全部逐字节复用；strict38/275原始导入/1062PAC、六项IOSurface/HMAC
和单核/八核控制按同内核摘要沿用，不重复Mac构建。新boot
`bcf1e1440fb0f14c7a1360f4c571d0a03559984b20d56722e43555766b166eaa`，
payload `81b590cf7978c3cd204769b8a7e247fefa446b2fd328f093be317bea1a05a154`。
固定reader仅更新payload校验摘要，其他28个固件模块和启动shim一致。
私有工具`research/tools/pack-cellular034.py`生成并核对全部八个FAT成员，
`research/cellular034-package/original-context034-executed.json`及
`signed-page034-qualified.json`保留原指令和页面检查结果。
设备源码`tools/baseband27/context_ticket_fixup.py`只读核对原CommCenter的
SHA、UUID与四字节原指令，输出原签名页面解析器所需的35字节NUL结尾表项。
本机原输入生成与034第9项逐字节一致，错误版本、UUID和原指令均拒绝。
调用方式：
`python3 tools/baseband27/context_ticket_fixup.py --commcenter <local24A446CommCenter> --output <localEntry>`。
输出接到原8项之后；Apple原程序保持本机私有输入。

动034前已更新来源：UniXNU `9ad4697`、Trogdor `7e0a044`、
QcomBaseband `dd0199c`、myron-next `f1a1bfe`；没有可直接替换的同版蜂窝实现。
原完整同SoC/data-a-live调查保留，不重做搜索。113入口SHA3778623e已通过
Linux/Windows具体参数和原023生命周期检查；本机/113各75项工具、固定boot
部署及113原Toolhelp实现SHAeacaa4ff通过，实际203进程中仍检测到电源15468。
按最新camera062实际后验绑定，22:43:57 UTC登记034共享队列，保留当前
电源081拥有者，无034领取或启动。唯一文件等待流程在轮到034后接一次
400秒观察、正常返回/机器后验、冻结控制台解析，再排独立只读日志窗口。
新只读入口81da24a3和reader7b2796复用033原APFS读页/校时函数；Windows
实际领取/释放/保留其他拥有者检查通过，读取总240秒、解析600秒有界。
033的568134条同UUID日志已用于对比分析，034目标UUID按实际时钟选取，待实机。

补原指令执行覆盖`0x100092a94`调用者、设置树选择器以及控制器真实字段
getter：次对象+0x40的31a/104对应完整对象35a/144，前者来自原
`/cc/props/is_postponement_ticket_available`，后者由原postponement事件更新。
四组场景通过：票据缺失时原上下文-2变为原选择器结果；设置拒绝仍不可用，
设置方法调用、原字段和栈保持。PAC、类型及外部设置方法明确建模；不以模型
代替实机连接。原QMIDataContextDriver→IPAggregator→IPBase→profile创建/
配置→StartNetworkIface响应处理已留本机对照，硬件效果仍待034实际日志。
只验第9项实际应用、Internet上下文、真实WDS Start、
地址/DNS/TXRX/TLS及正常返安卓后验。WiFi023保持原已合格驱动；
短信、IMS和通话仍单独验收，人工操作按无人值守授权后移。

034电脑诊断交付`11e4da8`增加`tools/baseband27/wds_diagnostics.py`，读取原桥
已有控制台字段：按channel/client/message/transaction配对WDS头，保留呼叫
结束原因的present位及有符号verbose code，分别记录IPv4/IPv6地址字段、
每个`pdp_ip`的TX/RX及trace预算。无client的生命周期行单独保留，避免双卡
相同transaction被错误归属；缺请求头时保留未配对响应。Linux与Windows各
六个场景通过，覆盖内部client、两通道、缺请求、缺原因、失败设置、接口交叉
计数与空/截流日志。实际033完整1773255字节控制台/SHAe54190ec输出77条WDS
头、37组配对和18条生命周期记录；绑定5次、IP族设置4次成功，Start/profile/
runtime settings和TX/RX为0。结果已接入034上下文分析，外部TLS/HTTP仍由原
helper实证独立判定。

人类随后明确“请继续，人类已苏醒”及“在本机，处于安卓”，共享记录已沿原
Windows锁更新为local5037。原113文件等待进程779094已不存在，未留下034
claim/write/collector/boot记录，保留中断事实，不称正常完成。本机入口
`trial-baseband034-local.py`/SHA77b11b70沿原023的root exec-out和共享生命周期，
原034只读camera参考兼容沿用；Linux/Windows实际参数/错误身份检查、本机
75文件host-r008及固定boot部署、9个完整链场景、只读claim/release/queue检查
均通过。只读reader/SHAf8dbe857的10个原APFS函数只替换host条件，沿用033
37块/100页真实输入覆盖；仍按034实际启动/释放墙钟与final clock选择UUID。
04:55:28 UTC已登记034本机run，唯一文件等待30293/session33059等待原电源081
实际后验释放。当前成品bootbcf1e144/payload81b590与编译源码6a2eef8保持，
没有034手机启动结果。人工测试恢复安排，数据、短信、IMS、通话验收分别记录。

034本机已于05:01:47.192925 UTC接受一次临时启动；完整控制台3124906字节/
SHA40214b2c覆盖429.122918秒。第9项CommCenter修正在102.103533秒实际应用，
helper52在93.970896秒走自己的temporary-sandbox执行路径，MSS/IPA与两卡控制
保持，CommCenter零corpse。WDS绑定5次、IP族设置4次成功；Create/ModifyProfile、
Start及GetCurrentSettings均为0，PDP TX/RX0，蜂窝HTTPS未通过。原8项的最后
APN修正没有对应取指日志，033同样如此；不能据此判定页面应用错误。
一次正常USB请求a9be719453f83fb1/c2后，05:09:28.783185 UTC实际安卓/root、
Data17同135168字节、27generation1/c968ea、映射-pin/当前payload81b590与42596
退出后验通过释放。返回ACK为queued，实际安卓后验另存，不把ACK当完成。
原日志只读local001已文件排队，等待当前电源081本机窗口释放。WSL重启后按
原017元数据回执4b1ab4d4恢复735文件/288842541字节，全部长度及SHA核对，
解析工具与日志UUID选择规则保持。

独立DataSettings的原`0x100093ca4`/slot4c0会查原上下文属性和blocker；slot420
`0x100094234`遍历原24字节项并消费slot98。其false在`0x100094010`产生
`ActivationStatus failed in DataSettings`/-3，区别于票据分支的上下文-2。
七组原指令执行覆盖缺属性、原控制器true/false、blocker0/1和上下文类型0/1/2，
栈保持。PAC、外部属性/所有权/复制/控制器值明确建模；034没有改该检查，
本轮原生日志仍是决定下一步适配的实际依据。


034独立只读窗口也已完成：05:31:59 UTC按原共享队列领取，唯一选中实际启动
UUID `ED3F3F1B2B464A659B65552D341B3690`；取得19382272字节原始页，10份文件
合计17915848字节。读者41256正常退出，同安卓boot、17/27 Data和映射后验
通过，05:32:37.089585 UTC释放。全部626212条原记录均为目标UUID，64222条
相关记录全部按625/12唯一校时，解析consumer_errors为空；完整JSONL SHA
`f69a8afc31cfb3caeaa57be02a5249ffaf04f37aa2a87eb1c70ab589ec768a78`。
实际安装cache、signature和helper逐字节与033一致。helper PID52/euid0在
94.101874秒进入、chosen4，216.659325秒发一次蜂窝请求，408.655705秒正常
exit11，无relaunch；guard/channel/DNS Mach/本地写入EPERM及ServiceNotRunning
为0，NoNetworkRoute仍有7条。034的启动、返回、日志采集和解析链全部结束。

主数据卡 `DATA.Connection.Internet.1` 的实际拒绝原因已推进：033有23次
“context is not assigned”，034为0次，转为21次“Data is not provisioned”。
缓存中configuredByMask[BB]=true；不能单用pdp=0判断进步，033晚期也曾出现0。
原 `0x100094c0c` 在BBDataDriver字段+0x118不是1时沿 `0x100095080` 返回-5；
+0x119的raw attach状态由另一条通知更新。实际两卡Registration已Provisioned、
rawAttached=true，但DataServiceController只收到Unknown→Unavailable→Unknown，
没有后续Provisioned，WDS两卡IPv4/IPv6仍INITIALIZED、无Start。

下一步追踪原 `/cc/props/baseband_service_provisioned_states` 的每slot映射，
从Provisioning模型的SIM信息树+0x2b0、原SIM-settled/产品能力选择，到
DataServiceController每slot消费者、次对象+0x18/slot0x1a0及
BBDataDriver::handleServiceProvisioned_sync写+0x118。该表项经原thunk
`0x1002da368`回到`0x1002da0a8`，不能误用primary vtable的同偏移。
原CardProvisionedState、ServiceProvisionedState及Activation TLV4f/50/51枚举
已从原parser逐项核对；当前wire2确实映射为CardProvisioned1，保持真实响应。
尚未创建035。蜂窝HTTPS、短信收发、IMS及双向通话仍未通过，WiFi023沿用。


## 033：蜂窝程序自己的过滤身份与网络客户端权限（2026-11-16）

032实机已明确选中temporary-sandbox，216秒仍由Autobox拒绝guard/channel、
DNS mach-lookup及本地mDNS网络连接。只读001已完成18:49:42 UTC后验释放，
因031之后两个结束时钟匹配而拒绝选择；002按实机启动/释放墙钟唯一选中
`A1C91F396C384B5E94B160404753B55D`，随后遇无关最新启动的未完成catalog，
18:57:24 UTC正常后验释放。003先比header UUID再解析对应catalog，电脑检查
通过；19:01:50 UTC首次Android查询设备不在场，未成功读/写/启动手机，
19:04:08 UTC撤销未用预约并保留最近实际后验。人工连接恢复后另窗补原日志。

本机按同24A446原profile载入代码解出1033个按程序名选择的Autobox表：
DumpPanic原unix表不含444/510，DNS mach-lookup表也不匹配普通网络客户端。
原系统的temporary-sandbox在syscall channel、network-outbound和dnssd服务
分支明确读取`com.apple.security.network.client`。原非容器命名profile的
dispatch实际执行，保留已匹配DumpPanic附表；自己的cellular-demand021名
不在原表中，走原无附表分支。模型只证明原分支与字节选择，网络效果待实机。

033保留原DumpPanic缓存任务的Label、RunAtLoad、KeepAlive/Crashed和其余字段，
只把ProgramArguments[0]改为`/usr/libexec/cellular-demand021`，原DumpPanic
系统程序和FDR保留。helper仍是028原请求源码/TLS/180秒期限，仅在032的
temporary-sandbox签名字段上增加network.client=true。唯一helper43752040779/
attempt1、head `c675cf42ea0f0de6f08685f8385d4ec650d02ab6`成功，30项CMS成品
核验通过；helper SHA `bc789472bc35470aa6187f03cc9fa140044e9dfa784093128c1252672359e795`，
CDHash `d9bbe1e10aa94f3d1b4ac296fac414246e357c3c`。4KiB页、两类签名权限槽、
原依赖及七个实际入口场景通过。新缓存2482134字节/SHA60e0fe8a与新签名
完成页核对；未改动的iOS27解析器按实际key/value缓存返回729项、零字段差异。
精确MAC谓词只更新helper摘要，chosen29/21、parent1、system-service、CS_VALID、
identity与spawn/SETEXEC条件保持，29个ASan/UBSan场景通过。唯一native
43752811275/attempt1、head `367fd3a7293b2f00fec1ca0cf43c1cc865bdd20b`
成功取回409项CMS验证产物，编译设备源码`6a2eef8bc8b0a374d80f86a1e356310243af9d84`。
实际MAC28场景、strict38零missing/stub、310原生/275原始导入、1062基带PAC、
六项IOSurface/HMAC通过。新单核/八核均到独立root映射拒绝点、policy2，非超时。
最终kernel `02245d455f29501bfc32d6c45ec2d6671e6a2284daa3f28cf5e78dfcdeac8dc9`，
boot `8050329254f953c2953d5da7c67cfa7336aa2f6ed11a94550ef7c2946c835606`，
payload `e4c1b983c62d63bfb08cd2fc141fc61262a8e0247ba1c1ff3b7b27ac9a8a7f6f`。
本机与113入口、原cache/helper精确配套和两台主机75文件部署通过。
113入口与本机、113的唯一进程检查均通过，按音频102最新释放完成一次
20:35:59.141232 UTC临时启动。cache/bin在66.690837秒5/0；100.936495秒
原exec明确cellular-demand021[52]选中temporary-sandbox，且程序名没有原
Autobox附表。双卡MSS/IPA保持444.119231秒，CommCenter零退出；完整控制台
1773255字节/SHAe54190ec仍无WDS Start/PDP流量或TLS成功。helper_started
按原exec实证为true，完整入口/请求/退出和网络错误待原生日志。一次正常
USB返回后，20:44:02.111436 UTC安卓/root、Data-map-pin、当前payload及11820
退出后验通过释放。只读001于20:50:49 UTC领取，按实机墙钟与结束时钟唯一
选中`04FD4CEA36104E6CBB71B71D9672033E`，已取timesync和Special 02c8/02c9。
扫描Persist旧0160/0161零字节文件时停止，原capture保留complete=false；
20:52:14.685630 UTC同安卓boot、Data、映射和读者37208退出后验通过释放。
电脑解析两份Special得到111545条同UUID原记录，1811条相关记录全部唯一校时；
尚无helper请求/退出记录，不用这部分日志推断网络结果。补采集002跳过零字节
文件，按inode、长度、SHA和目标UUID复用原两份Special，继续读取目标Persist
与实际安装cache/signature/helper。真实APFS 37块/100次页读、4099字节对齐尾读、
空文件零读取、两份实际033日志复用及Windows领取/后验/队列检查通过，
20:59:10 UTC文件排队。音频102实际释放后，002于21:43:40 UTC领取，
12800000字节只读取得目标Persist、timesync、实际cache/signature/helper、
helper uuidtext及cacheloader日志，Special按原SHA复用；读者53492正常退出。
21:45:30.297385 UTC同安卓boot、17/27同135168字节Data、映射及读者退出后验
通过释放。568134条均为目标UUID，58481条相关记录全部625/12唯一校时。
实际安装三文件与033成品逐字节一致；helper UUID仍7F5FFE9A，原uuidtext仍保存
旧DumpPanic路径，因此身份按UUID、PID、实际exec行和安装摘要联合核对。
PID52/euid0在101.317412秒进入、chosen阶段4，222.851394秒发一次蜂窝请求。
原DNS Mach服务明确接收PID52；033没有031的guard、channel、DNS Mach、
本地写入EPERM或ServiceNotRunning拒绝。路径仍无网络路由，414.769216秒
正常exit11，无重启。CommCenter数据开关为true，两卡WDS已INITIALIZED；
Internet从130.396秒起仍为context is not assigned，未发WDS Start。权限进步，
实际联网尚未通过，下一步沿原生上下文选择/回复定位。SIOCGIFMEDIA的
EOPNOTSUPP在原IPConfiguration兼容分支四场景通过，不能仅据该日志推定根因。
人类21:41 UTC要求无人值守，已沿Windows共享锁记录；人工测试后移，无询问。
032完整同UUID原日志补采集仍待，不重复032或033启动。

## 032：原系统签名 profile 选择（2026-11-16）

031已完成一次实机和独立只读日志窗口。12:58:54.847596 UTC临时启动，
MSS/IPA与双卡维持427.406891秒；13:06:35.711844 UTC正常返回安卓/root、
Data/map-pin和35936退出后释放。随后读者7948正常退出，取回18,055,168字节，
13:09:29.052850 UTC同安卓boot、Data和映射后验通过释放。449170条均为
`2B99D127F7E54822B4EADFBD1ECA4459`，59807条相关记录全部625/12唯一校时。
实际安装helper与031签名成品逐字节相同。PID52/euid0在94.573秒通过
chosen阶段4，在215.465秒发出一次蜂窝请求；签名no-sandbox仍未解除
Protobox，change_fdguard_np、channel、DNS mach-lookup和sendto_nocancel
被拒，NECP/DNS为EPERM、无路由及DNS -65563。407.380秒正常exit11，
无重启、WDS Start、PDP流量或TLS成功。启动保持，数据持平。

032参考同一24A446原Sandbox exec代码与362个原任务程序的本地签名扫描。
其中79个程序使用字符串
`com.apple.private.sandbox.profile:embedded = "temporary-sandbox"`，
包括cameracaptured、audiomxd与uarpd。原选择段及OSString解析器已在本机
执行：无字段、正确embedded字符串、错误类型和直接profile四种场景通过；
导入指针沿原chained fixup重定位，OSString等外部原语与PAC分别建模。
该结果只证明命名profile选择路径，完整profile权限效果仍需实机。
032已用此字符串替换已证无效的no-sandbox签名字段。唯一helper构建
43742046532/attempt1、head `540fb94a686bdc1dd75c848317af697c570c41ea`
成功并取回30项CMS验证成品，helper SHA
`7cad337cc31d3afa6a750282b6ac583f0415f00d8011291f98315ef877d42fba`，
CDHash `b28eb5cc629cc9112bf70134a01e4d57c2ee3b41`。4KiB签名页、
两类权限槽、原依赖和七个实际入口场景通过。028请求源码、TLS和期限保持，
精确MAC入口只更新新摘要，29个宿主场景通过。唯一native构建
43744004846/attempt1、head `93f48c6be2804c76618320184149ff53bc49ee0d`
成功并取回409项CMS验证成品，编译设备源码`16242ecee7b54732bb46a8be9ce74cb8cb9537b1`。
实际入口机器码28场景、strict38零missing/stub、310原生/275原始导入、
1062基带PAC、六项IOSurface与HMAC均通过。新单核/八核控制各到真实
独立root映射拒绝点，两项policy已核，均非超时。最终kernel
`cffe25a8ec8dea6fe5c0f6773b9c64a7d8c6265c7a089eda1f03b76908e1be04`，
boot `e7821c3b7e0fc950d8657b1861151972ac25b94573c05bfa866bde65cf14f362`，
payload `aa3bb58130b579f2e680b4bb069bca69459235a80ad1347791104d01534755f7`。
原729缓存/签名与新helper精确配套，Linux/Windows本机入口通过；
18:18 UTC最新GPU050后验绑定与75项主机部署通过，文件排队后
18:22:06.721980 UTC按相机053实际释放临时启动一次。cache/bin在59.100919秒
为5/0，原DumpPanic[52]明确选中temporary-sandbox；216秒请求期间仍有
Autobox syscall444/510与DNS mach-lookup拒绝，另有mDNS本地network-outbound
拒绝。双卡MSS/IPA保持418.259642秒，CommCenter零退出，无WDS/PDP/TLS成功。
一次正常USB返回后，18:29:38.889281 UTC安卓/root、Data/map-pin、当前payload
与49328退出后验通过释放。helper_started已按原exec日志纠正为true，
不能按USB BB21缺失判断未运行。完整同boot原生日志只读窗口已排队，
等电源079实际释放；请求/退出及网络错误细节仍待原生日志。17:29 UTC刷新UniXNU `4dd26bc0b415dfb87ecdbaf9479e1d14c00ece74`、
Trogdor `97cf27acbdae8498119cbc78a98742e4ed9d92a8`，QcomBaseband仍`dd0199c`。
Trogdor这五项更新未改变蜂窝目录；固定GPU024构建输入不升级。

## 023：蜂窝请求程序的原生服务入口（2026-11-15）

029原生日志已补取并解析：002只读窗口21,065,728字节、读者54700正常退出，
11:32:55 UTC同安卓boot、17/27同135168字节Data、映射/pin后验通过释放。
568016条均为`63F1937A23554967B09E8036F2C6E552`，59134条相关记录全部
625/12唯一校时。实际helper UUID `7F5FFE9AADBD3BFB83672C72D3A09CF6`、
PID52/euid0，93.519秒入口/chosen阶段4/接受，214.870秒发出一次
Network.framework蜂窝请求；本轮USB BB21流缺失，不能据其判定helper未启动。
原请求报告NECP fd guard `EPERM`、`No network route`，DNS客户端本地写入
`EPERM`及`DNSServiceCreateDelegateConnection ServiceNotRunning(-65563)`；
406.817秒请求期限退出，原launchd在406.821秒确认`exit(11)`，无重启。
这是启动进步、数据功能持平。安装cache/signature/helper均逐字节等于029，
没有WDS Start/PDP/TLS成功，不能宣称蜂窝或短信/通话完成。

031将原声明放入有限helper的签名entitlement，验证029已证实的本地网络权限
缺口；源码、Network.framework请求/TLS校验和180秒期限与028逐字节相同。
唯一helper43713600352/attempt1、head `7b589612e780bc8ef46ff85d71e39f62b127a614`
成功并取回30项加密成品，新helper SHA
`f5b87c247e096bbd948aa6c86d0590975c4da652f1d3d53bc033ff0c769fd316`、
cdhash `69d9b2c582467a0319fe8a3a102e48e7404840d0`。4KiB签名页、原依赖、
两类entitlement槽和七个实际入口分支通过。只更新精确MAC predicate中的
expected cdhash；29个真实predicate宿主场景通过。新内核及控制/封包已完成，
030同boot日志已完成，实际缓存声明未解除Protobox及DNS/NECP权限拒绝，
确认031签名权限修改所针对的缺口。030只读22,151,168字节、读者34448正常退出，
12:35:24 UTC同安卓/root、Data与映射后验通过释放；621846条同
`2CB270A735EF4F85B680E57772F8E24A`，59349条相关记录全部625/12唯一校时。
helper在215.249秒发出一次蜂窝请求，DNS XPC/channel/guard及本地write被拒，
407.018秒原launchd确认正常exit11，无重启。031固定358个公开输入/29单元
重构通过，唯一native43714036708/head
`06ffcf2692bc775f776c62406b552c774455575b`已成功并取回409项验证成品。
新精确helper MAC机器码28场景、strict38零missing/stub、310原生/275原始导入、
1062基带PAC、六项IOSurface/HMAC通过；新031单核/八核均到真实独立root
映射拒绝，两个policy已核，非超时。最终kernel
`715b9ef2269dc934997c8a2946d8451ec7107f83a1b0020ae524cf20c1330f09`，
boot `4e0afe2ee47bc4f916ece02976c7a22394653942bb97e8bcb2e5b8b9910c2b99`，
payload `cfdcad12bd202851c1de1913119497dc7e3e8c89c83a85c064f1437e3df63580`。
原cache/signature与新helper配套核对、本机Linux/Windows入口和75项r008部署
通过，12:54:20 UTC排队，随后完成实机与原生日志；结果见032节。

030只修改原DumpPanic任务的一项权限声明，复用同一24A446原缓存replayd的
`com.apple.private.security.no-sandbox=true`，及Trogdor
`2cbd3d7f2a997cbcf798dc2794ea52ffb7a0842d:tools/coachz_apps.py`的同字段用法。
原729项数量、其他任务、helper字节、chosen29/21、精确MAC gate、029内核、
GPU024和基带协议保持。原对象及四字节ref/offset格式保留；新缓存签名/trust
和payload需重封。Linux单任务差异/原添加任务合同/签名页拒绝通过；未改动的
原iOS27解析器在实际key/value字符串缓存路径完整返回729项、零字段差异。
该任务权限的实际效果待新封包和实机，不改变内核全局MAC/AMFI或modem配置。
030封包boot `7f6c5e34aa3c827a460b189fb4344d818d8db90fe20432d828c92b81eb294b9b`、
payload `a411421101e3c55348dedd2f03af08ae6a7121734a54d5a3172ee3efd1f87865`，
内核与helper均等029；原始单核/八核和strict38/PAC按精确SHA复用。
11:58:31 UTC按GPU048最新实际释放完成一次本机启动，59.015秒cache/bin5/0、
429.864秒MSS/IPA与双卡维持，控制台仍见DumpPanic Protobox unix83/MIG44，
没有WDS/PDP/TLS成功。一次正常USB请求后，12:06:18 UTC实际安卓/root、
Data/map-pin、当前payload和46236退出通过释放；只读原生日志已电脑就绪排队
等电源076。缓存字段是否实际解除DNS/NECP阻断仍需本轮原生日志确认。

2026-11-15再次fetch UniXNU `b6e123a40311035cc8584e13af78887966ca1b5d`、
QcomBaseband `dd0199cf3300764ed52b23f4a47761f91fab21a2`、Trogdor
`2cbd3d7f2a997cbcf798dc2794ea52ffb7a0842d`。Trogdor新增启动后DMS低功耗处理
解决其驻网前等待；nezha已实测双卡驻网，本轮不引入该额外模式切换。

029复用本机音频096在同一iPhone27/24A446上已实机执行的原缓存诊断入口。
028已于07:54:14 UTC单次临时启动，MSS/IPA/双QMUX保持432.335174秒；
一次正常返回后完成安卓/root、Data、映射/pin和采集器退出，08:01:59 UTC释放。
随后独立只读窗口取回21696512字节，08:06:09 UTC完成同安卓boot后验并释放。
三份Special和一份Persist共610013条，boot UUID
`257604F4067E46ADA1EE7264F932E417`，58937条相关记录全部按625/12唯一校时。
原FDR[97]确实运行，但没有helper UUID、BB21或owner任务事件；
实际缓存2482276字节、签名和任务均精确一致，无旧尾。主机执行原launchd
索引/序列化/IOtype0选择和导入前置条件能到达构造入口，外部XPC/域/分配及
PAC仍为明确模型，没有确定实机最终跳过点，也没有HTTPS失败的证据。

新方案使用音频`setexec096-device`的
`zzzzzz-nezha-audio-record-entry.patch`、
`zzzzzz-nezha-audio-record-launch-identity.patch`和
`zzzzzz-nezha-audio-record-setexec.patch`。
原729任务缓存及其原签名逐字节恢复，固定cache SHA
`f9e009500651792dffabb6bc218efcd02b0b960b5b645bbe8c0a0b7be5863ef3`。
`cellular_cached_runtime.py`将完全相同的028已签helper放在现有只读
`/usr/libexec`测试视图的`DumpPanic`入口；原FDR程序与任务保持。
原DumpPanic是RunAtLoad、KeepAlive/Crashed=true。helper每次执行只发一条
标准蜂窝TLS请求，180秒请求期限和400秒alarm经正常exit/_exit收尾；
正常失败退出不会匹配Crashed，仍须在真机记录任何信号崩溃或再次启动。
不将该原任务称为LaunchOnlyOnce，也不把模型当一次执行实机验收。

029两个晚序补丁将挂载marker设为29，并只允许chosen launch29/demand21、
原父PID1、typed system-service spawn或SETEXEC、非脚本、CS_VALID、
精确helper identity和028 cdhash的组合通过原MAC launch-constraint入口。
其他执行沿原策略。真实predicate的29个ASan/UBSan场景及继承XNU上下文
应用已通过；新MAC代码需要独立native029及新单核/八核控制，尚未上机。
029唯一native43702934379/attempt1、head
`291d8735139c75595fd0384d1007b918f03cdf4d`已成功，409项加密成品取回并验证。
实际编译MAC入口28个机器码场景通过，原addon文件长度修复也已核；严格38项
零缺项/零stub、310个原生导入/275个原始分支、1062个基带PAC、GPU六项
IOSurface及HMAC通过。新029单核/八核控制分别到达独立根映射拒绝，两个
root policy已确认，均非超时。最终kernel
`236883c7836ec17a86f3ae032b82eef2cf6caadab6d407a614e0dcd6eda94528`，
boot `73ae8dfdac78a68dc9d2fe601752d11cff732343b07c805e0f45114e196177bb`，
payload `576cc6fdaf56108737d4f9bb892222df405cf97d2f6e08d05d41910b4a1bcc1a`。
原缓存/签名、DumpPanic helper和无独立任务的addon精确核对通过，本机入口
与75文件host-r008已部署，10:35:33 UTC排队等电源075实际后验释放；尚无
029手机claim、写入或boot。之后使用当时最新释放绑定，不沿用028后验。
029于10:54:53 UTC按相机038实际释放完成一轮本机启动；两视图58.969秒
均5/0，MSS/IPA/双卡保持433.495秒。完整1714762字节USB控制台SHA
`9ac1697b9776a75045c43193eee477c2d7cda41a92908221d2a3d7d462a11ba8`，
DumpPanic52记录Protobox拒绝unix83/setitimer及MIG44；没有BB21流、WDS Start、
PDP收发或TLS成功。原程序的最终执行/退出需同boot原生日志确认，不把
这些拒绝日志直接当网络失败或唯一原因。一次正常USB请求后，11:02:43 UTC
实际安卓/root、Data/映射/pin、当前payload与51592退出后验通过并释放。
只读001已唯一识别boot `63F1937A23554967B09E8036F2C6E552`，取到timesync
和130页目录元数据；遍历旧Special inode时ADB超时。原失败保留，重插后仅
补原后验，11:23:08 UTC同安卓boot、同17/27 Data及映射/pin/读者退出通过释放。
电脑从原缓存提取66个Special、19个Persist的目录名字/索引，002改为定向
按路径和inode核对读取，不遍历无关旧文件；APFS与Windows生命周期检查
通过，已按原队列等待音频099释放。没有重复029启动或重新派发CI。
原基带桥、NAS/UIM/WDS/PDP、GPU024和八条RAM text fixup保持。
既有Wi-Fi023沿已验最终集成引用；本轮只推进蜂窝请求启动。

028仅为已签名021蜂窝测试程序增加入口诊断。027实机保持421.363364秒、
两个视图59.604秒挂载5/0，launchd/CommCenter无退出；同轮只读实际缓存
2482276字节与025成品逐字节相同、无227字节旧尾，签名和任务plist也一致，
原cacheloader验证并成功发送。499664条同boot原生日志完整解析，仍无owner
任务记录或BB21启动证据，未观察WDS拨号和数据收发。
旧helper在第一条输出前执行`geteuid()!=0 || !selected_payload()`并静默返回，
故日志缺失不能单独判定未spawn。`tools/baseband27/cellular-entry028.patch`
只记录进程入口uid、chosen查找/属性/类型/4字节标记检查阶段及最终接受/拒绝，
不输出身份值；原chosen仍为21，原请求/TLS/pdp_ip/204、一次请求和退出路径保持。
独立macOS helper构建必要，027内核、基带协议和GPU/PDP接口不重编，实机仍待。
028唯一helper构建43689118984/attempt1、head d66f30ee已成功，30个封装成员
验证取回。92352字节helper SHA `34edc272b82a580a5c30d490190772afaa5223977fb039b37000408c40b67d1a`，
cdhash `bdba7ce9161f4ef88b28d1a221d7b3f53c391016`；4KiB签名页、两种entitlement
槽、原依赖及七个实际入口机器码分支已核。新bundle仅替换helper文件，
027缓存/签名/任务、内核和新单核/八核控制均按精确SHA复用。
最终boot `6f86ea35e0cd86baadcbcfa5aa1d02c7ad20167c7fde66d23cb59932d557717f`，
payload `5a689678400959072adfab82b7a15d11562d7cfaabf5996287e0d9d36b53eb46`；
本机入口和75文件工具部署通过后，028已按队列完成上述实机与后验。


027复用上游UniXNU
`94a70b14671aae3d7c3c8a70b8c19776e464dcf1`的addon文件长度修复：
旧框架`87bb1c5`的`native_gpu_addon_write`只设置文件mode，内核
`vnode_open(O_TRUNC)`在非compound路径不缩短原文件。024缓存2482503字节，
025缓存2482276字节，后者缩短227字节，可能留下旧尾部；把该真实旧尾拼到
025缓存后，未改动的原版iOS27解析器立即拒绝，而精确025缓存完整通过。
`zzzzzz-nezha-cellular-addon-size.patch`只加入`va_data_size=size`，
错误、fsync、unchanged-file检测和原runtime服务保持。原上游缩短/增长/空文件/
短写/属性失败/fsync失败模型在实际选中函数下通过ASan/UBSan，实际补丁应用通过。
本步骤需要新内核，351个公开输入和29个基带单位的独立原生构建准备完成；
026诊断窗口用于验证实际长度，027真实服务启动仍待新构建/严格封包/实机。

026实机72.023秒两视图5/0，105.938秒四项原加载器诊断已应用，
106.060秒仍退出`No service cache`；一次正常返回和保护后验、
10756采集器退出，22:58:06 UTC释放。后续只读取回3100672字节，
38000reader退出0，23:05:55 UTC完成安卓/root、Data和映射后验释放。
实机安装缓存实际2482503字节；前2482276字节逐字节等于025缓存，
余227字节逐字节等于024旧尾。整文件SHA
`faf7d0e5f4efbebffcda1a600504ab2bb8d48f7bafb33fc94a78864c8ebc9b15`。
这条实际证据和原生解析器拒绝旧尾模型共同确定addon截断缺失，
不再继续调整任务启动条件。027唯一原生43650666401/attempt1，
CI head `372b0fa62dcdb751edc1cde3bec631ba1fc16b88` 已派发，后续严格封包和
新内核控制顺序接续。027尚未实机验收，真实蜂窝请求和IMS/SMS/通话仍待。

027唯一构建已成功，409个加密输入逐项验证取回；严格38项依赖、275原始调用、
1062基带PAC槽和既有GPU六项IOSurface/HMAC检查通过。新内核单核与八核控制
分别到达真实独立root映射拒绝，两个策略已核，均非超时；没有复用旧023内核控制。
实际027 writer机器码执行传入`va_active=0x210`与`va_data_size=2482276`，
旧023对照仅`0x200`、长度0，代码编译修复已确认，文件系统实机结果仍待。
最终boot `140bc5d4805e6d48affa9c073a7e65418ac88772329dd01b5e8f4f1768293e95`，
payload `b4d26b6fe37fb6f80bb4b7bc88370a3248c850edc2d304aad373eb246f4522ca`，
kernel `bb930a8b6471deadfc7c5615bc614a532a8ab2afc063fb1283bcf722ba82c9c2`。
人类2026-11-15再次确认本机接线及人工测试；027本机入口、75文件host-r008部署
和最新释放参考已合格，当前有序等待GPU047实际保护释放，没有提前操作手机。

电脑已把原IMS属性发布链定位到`CarrierEntitlementsImpl`：其`device_account_id`
回调消费原生`entitlements::ControllerImpl`的账户ID与realUniqueID getters。
原控制器按订阅读取原始身份，并可生成原生UUID；单独UUID生成日志不证明
按PersonalityID的属性已发布或IMS已消费。继续按实际同boot日志核对，不用
IMEI替换该账户属性，不强设IMS就绪位；该分析没有改变027实际编译源码。


025实机在100.253842秒原版launchd退出，namespace7/code1，
description `No service cache`。MSS/IPA和USB控制仍保持，但CommCenter和
helper未运行。本轮单次临时启动和payload写入已正常收尾，21:29:13 UTC
安卓/root、Data、GPT/映射保护和8152退出后释放。025在logd启动前退出，
后续只读timesync没有该轮匹配UUID；采集保留失败，并于22:14:24完成后验释放。
不能把这轮解释成蜂窝请求或IMS失败。

电脑已实际执行原版iOS27 `_xpc_create_from_plist_with_string_cache`
`0x1805d64f0`及其原始解析/边界函数：原729任务缓存、025完整730任务缓存，
以及025带key/value字符串缓存的路径均完整返回，内容逐项等于Python读取，
零字段差异。外部仿真仅提供分配、字符串和集合原语，PAC指令单独标明；
这不是实际launchd签名/传递通过。原helper、内核和缓存解析代码未改。

026在`cellular-cache-loader-diagnostic.json`固定原加载器SHA下，只把已有
日志文件名改成该轮独立文件，并将原日志里误打印的页面大小改为`fstat`
实际文件长度。四项仅占原文件对应字节，在既有验签后的text-fixups应用，
原签名、trust查询、有效性检查、mapping和发送指令保持。两处修改指令
已在Unicorn执行，读取2482276字节长度正确；下一次实机收其保留日志，
用于区分实际签名缓存、文件长度和launchd传递阶段。

025接续024实测的启动缺口，只修改owner任务的启动需求：
`KeepAlive=true`由原版`0x10000a220`设置任务需求位，原
`0x100020f2c`在没有进程时选择启动原因；同时保留
`LaunchOnlyOnce=true`，原退出路径`0x1000564ac`将该服务标记移除，
不会持续重启请求程序。025移除024的IOKit事件依赖；023原生内核、
021签名helper和chosen的23/21两个标记继续复用，无新内核/helper构建。

024实际临时启动于20:25:29 UTC，到430.219703秒，66.283077秒两视图5/0；
没有helper/WDS/PDP事件或CommCenter死亡。一次正常返回、安卓/root、
Data17同135168字节、Data27generation1、GPT和映射保护后验通过，
15048采集器退出，20:33:18 UTC释放。一次SCP取回失败保留，完整冻结日志
于释放后取回；后续正常收尾与日志复制分离，不占窗口等复制。
024原生UUID `9013B664AC7B43D5A06C451A5051CDDB` 的三份Special和一份Persist
共551161条解析、55736条相关记录全部唯一校时，无owner任务事件。原先两份
Special选择漏掉0232，已仅补读缺少文件并合并证据，没有重跑有效解析。
21:07:24 UTC的只读窗口实际取回安装缓存、签名和任务plist，三者摘要与024
成品一致；另有对应0232文件，同UUID确认。真实请求和IMS条件仍未通过。

023已使用原生43628201650、内核`6889d5b6c0c3264656d2c7a5503655b0e56f954ba00b734ef4751cbb09ad706e`
完成一次实机启动。64.063382秒缓存和程序视图均挂载成功（5/0）。
同次UUID `5A89E9AAEBFB40B3AD77C2D054984B66` 的522345条原生日志中，
新任务在433.266425秒被移除，随后PETRIFIED；没有该任务的spawn或helper
进程事件。这证明原版接受并登记了新增任务，但RunAtLoad没有启动它。
96.541649秒原版launchd进入ondemand模式。MSS/IPA/双QMUX保持，CommCenter
没有死亡事件；没有实际蜂窝请求或上下行。该轮正常返回和后续16752640字节
只读采集均完成安卓/root、Data、映射保护后验和进程退出，分别于
19:46:31和19:57:28 UTC释放。

024只调整这个owner任务：沿原缓存`com.apple.network.latticed`的
`com.apple.iokit.matching`/`IOMatchLaunchStream`格式，在现有
`AppleBasebandPCIControl`发布时触发；原驱动已在MSS就绪后发布该类。
`LaunchOnlyOnce=true`限制一次启动，保留`KeepAlive=false`和原程序自身期限。
原729个任务、原对象格式和helper保持。024复用上述023内核，chosen启动标记
仍23、请求标记仍21；只重封缓存签名、trust模块、payload和固定摘要reader，
原生构建和内核保护控制沿相同字节复用。024真实事件触发和联网仍需独立实测。
同次023仍有DeviceID缺失；成功DMS GetIDs的36字节回应不足以确认真实IMEI
已转交，这条身份链继续根据原始消费者分析。

021实际临时启动到422.599777秒，双QMUX/MSS/IPA保持，但请求程序没有执行。
同次启动UUID `2E22E18BD2784C6E94FD8855AAD8A0CF` 的454578条原生日志，
54597条相关记录全部按625/12校正；94.321945秒实际启动的仍是原版
`IOMFB_FDR_Loader`，99.135133秒其服务退出。不能把未启动的程序算成
HTTPS请求失败。该轮正常返回及只读采集均已完成实际安卓/root后验并释放。

原版27从签名的`/System/Library/xpc/launchd.plist`加载729个服务；
仅在`LaunchDaemons`覆盖同名plist不改变缓存内的ProgramArguments。
复用UniXNU-sheng的`sheng-wifi-launchd-cache.patch`和UniXNU
`build-boot-runtime.py`已存在的缓存/系统执行路径接入方式，023只给
蜂窝测试建立独立`org.unixnu.cellular-demand021`任务。原729个任务保留，
原FDR任务也保留。这里复用的是服务入口，现有已验Wi-Fi023源码和成品
继续沿其最终交接，不重新适配或复测Wi-Fi。

`zzzzzz-nezha-cellular-launch.patch`只在chosen的4字节
`nezha-cellular-launch=23`下，用现有只读bindfs union把本机缓存和
签名挂到`/System/Library/xpc`，把既有程序挂到`/usr/libexec`。没有新
线程、kernel exec、服务假成功或缓存校验绕过；挂载返回值直接记录。
`cellular_runtime.py`从本机固定原缓存增添一项，保留原对象字节和
4字节引用格式，生成独立SHA256/4KiB detached CodeDirectory并检查
完整覆盖与单字节变更拒绝。原IPSW缓存与签名从未写回，也不进Git/CI。

已签名helper `73a53ddc7724db130cf538a53ccb5cad0f3ff204a7aa791cce25e9fbd7329eca`
逐字节复用，chosen需求标记仍为21。一次Network.framework请求仍要求
cellular类型、pdp_ip路径、默认TLS认证和实际HTTP204/上下行字节。
022被动DMS身份、原NAS/UIM/WDS/PDP/VOICE/WMS、GPU024、USB和Data1保持；
本步骤需要改变kern_exec挂载实现，新的原生内核与实际服务启动/联网
验收独立进行，不能把原022构建当新023内核资格。

## 022：只读设备身份转交（2026-11-15）

固定020/b5db101的完整实机日志已证明双卡驻网、NR承载信息、原生WMS短信
就绪和NITZ校时。IMS注册仍缺DeviceID/SIM文件/连接/DataContext等条件。
当前BSP激活通知只带ICC ID/IMSI和卡状态，没有设备IMEI字段。

`NbDeviceIdentity.h`被动观察原生DMS请求/回应：成功的BindSubscription
0x54（TLV01/u32，primary1、secondary2）确认每个客户端的订阅；已有
GetDeviceSerialNumbers0x25的成功回应仅在TLV11带十五位十进制真实IMEI时
缓存。相同事务号按客户端/端点/代际区分，绑定变化、失败、未知或缺失
IMEI、远端消失不会发布另一订阅的旧值。没有新增手机查询、写入或radio命令。

`NbSimBridge.inc`沿现有锁和发送队列把已确认的两卡设备IMEI加入原激活通知：
卡1 TLV12、卡2 TLV55，各8字节。格式采用原版移动身份编码；两个合成身份
已通过未改动的`ActivationCommandDriver::parseImei`实际指令解码，原SP和
canary检查保持。设备身份不进入console、Git或构建上传。

来源为原厂`libqmiservices.so` DMS v1.97的service object/IDL表、
固定libqmi的Get IDs定义、24A446原版激活派发/8字节解析器及IMEI转换函数。
wire-os的同名头文件只作订阅枚举名称参照。对应原字节和SHA留本地
research/checks-identity022及commcenter013/rfs021，不把Apple/厂商文件
加入源码仓库。宿主ASan/UBSan整套和受影响C++预编译已通过；
唯一原生43617168519/5982d0a已成功，严格38组件零missing/stub、
1062个基带PAC槽、原GPU六IOSurface/HMAC及1/8核保护控制均通过。
023内核已携带022实现并上机；原生仍报DeviceID缺失，真实身份/IMS转交尚未
通过，不称真实联网或通话完成。

## 1. 固定来源

| 来源 | 版本 | 许可 | 用到的部分 |
| --- | --- | --- | --- |
| UniXNU/QcomBaseband | `dd0199c`（components.toml 固定的版本；2026-11-09 fetch 后无新提交） | BSD-2-Clause（HoshinoYumeka） | `baseband/*`、`patches/`、`ipa/*`、`platform/coachz/coachz_modem.c`、`tests/coachz_bb_model_test.c` |
| UniXNU-sheng | `4601795`（经 audio27 `abde8b4` 拷贝） | BSD-2-Clause-Patent | `QcomGlink/Smem/Smp2p/Qmp/Mdt/Scm` |
| power27-codex | `26254a8`、`e8d31f7` | BSD-2-Clause-Patent | SMP2P v1/v2 协商；GLINK 允许远端 intent ID 0（SM8850 真机已验证） |
| Linux 主线 `drivers/net/ipa` | v6.12 系列 | GPL-2.0（只参照取值，不拷代码） | v5.x 的寄存器字段、status 布局、表初始化顺序 |
| OnePlus SM8850 下游 `dataipa`/`datarmnet` | `ref/oneplus-sm8850-modules` | GPL-2.0（只参照取值） | IPA_5_5 端点和通道表、SMP2P 时钟投票、QMI INIT_DRIVER 字段 |
| tqftpserv / rmtfs（linux-msm） | 上游 | BSD-3-Clause（只参照语义） | TFTP 选项、会话、rmtfs IOVEC 语义 |
| OctoDevStudios/ios-popsicle（小米 17 Pro Max，SM8850，iOS 16） | `a4ef4ed`（2026-11-06） | 未声明许可，只读参照，不拷贝 | 同 SoC 在 XNU 下启动 modem 的实测：SMP2P item 方向、PAS 元数据低于 4 GiB、DSM assign 时的 SHM bridge、rmtfs 4 MiB 共享缓冲分配给 HLOS+MSS、TFTP 前缀和 servloc root_pd 180。它直接读安卓分区，与本项目只读钉住文件的边界不同，这部分不采用 |
| 本机安卓原厂（只读采集，2026-11-07） | 仅存放在本地 `research/phone-readonly-20261107` | 厂商私有，不进 Git | 固件版本和布局、DT、模块、运行状态、QRTR 服务表 |
| Apple 24A446 iPhone14,4 | 本地 IPSW 和解密卷 | Apple 私有，不进 Git | kernelcache kext、dyld cache、CommCenter、ADT、Mav21 bbfw |

上游原样拷贝放在 `*/upstream/`，各目录的 `SOURCE.txt` 里记录 SHA-256。27 派生文件全部由生成器做"计数式精确替换"得到：`tools/baseband27/derive-ipa55.py`、`derive-bridge27.py`、`derive-bridge27-test.py`、`gen-nb-ipa-v55.py`。原样拷贝不手改。

## 2. 硬件、固件与协议身份（安卓只读实测，另有标注的除外）

- SoC：SM8850（`canoe`，soc_id 660）；modem 是集成的 MSS `qcom,canoe-modem-pas`，不走 PCIe/MHI。
- 固件：`MPSS.DE.9.0-e64d0ee8fe`。modem 映像 PAS 4（`modem.mdt` 加 b00–b27，约 67 MB）；modem_dtb PAS 0x26；`ipa_fws` PAS 15（由 AP 加载，日志为 "xbl loading No"）；GSI 固件 v41。
- 内存：mpss 0x8aa00000/0xe100000，dtb 0x99500000/0x80000，DSM 0x85000000/0x4f00000 和 0x89f00000/0x100000（分配给 MSS_MSA）。SMEM 0x81d00000/0x200000，hwlock 3。
- 门铃：IPCC 0x1106000，modem 客户端 2；信号 0 是 GLINK，信号 2 是 SMP2P。SMP2P `qcom,smem = <435 428>`：入站 435、出站 428（按 smp2p.ko probe 的存放位置和两处使用确认；ADSP 边 audio27 也是这个规则），条目为 master-kernel、ipa、smem-mailbox。GLINK SMEM item 478/479/480。
- **SMP2P 版本**：原厂 `smp2p.ko` 的 `qcom_smp2p_alloc_outbound_item`（已反汇编）发布版本 1；如果远端 item 已经存在且版本更高，就用远端版本。之后等双方版本一致，不改写自己的版本。SOCCP 边在真机上是 v2（power009）。ios-popsicle 只支持 v1 的 SMP2P，在同 SoC 上 modem 仍能到 ready 并启动服务，说明 modem 边按 v1 协商；本实现在冷启动时也发布 v1。
- QRTR：AP 是节点 1，modem 是节点 0；实测 64 个 modem 服务（`qrtr-services.txt`）。AP 需要提供 rmtfs 14、memshare 52、servloc 64、TFTP 4096、IPA 0x31。
- IPA v5.5（hw_type 24），GSI v12，EE 0。AP SMMU CB 是 SID 0x4a0（uC 是 0x4a2）；可用 IOVA 窗口 0x20000000/0x20000000；`dma-coherent`；额外恒等映射 0x14683000/0x2000。
  - 端点按下游 IPA_5_5 表，**不是**主线 `ipa_data-v5.5.c`：CMD ep14/ch12，LAN_RX ep17/ch14，MODEM_TX ep2/ch11，MODEM_RX ep24/ch1。
  - 实测开启哈希（`ipa_fltrt_not_hashable` 打印为 False，DT 也没有 not-hashable 属性），所以 INIT_DRIVER 带哈希表 TLV 0x1b–0x1e，共 189 字节。
  - SMEM_IPA_FILTER_TABLE 是 item 497，大小 0x19000，位于 modem 分区。
- EFS：modemst1/modemst2/fsg 各 8 MiB，fsc 128 KiB。这是设备身份数据，**只留本地**。
- Apple 侧（24A446 静态分析）：
  - 原版 ADT `/baseband`：compatible `baseband,n41`，class 3；`/product baseband-chipset = mav21`。根 compatible D16AP 属于有基带的硬件型号。
  - **当前已部署的 27 ADT（beb99e3c…，persist001–020 共用）没有 `/baseband`，也没有 `baseband-chipset`。**
  - BBUEUR20Loader 的 PK hash 为 48 字节，默认值 `d40eee56…15ac87`；ChipId 0x001440E1。
  - **27 根卷自带 `/usr/standalone/update/Mav21.Release.bbfw`**（96967983 字节，与 IPSW 中的同名文件同长），所以不用像 Coachz 那样另外打包 Apple 固件。
  - AppleBasebandPCIControl 用 `setProperty("bootstage", v, 32)`；libTelephonyBasebandDynamic 把值小于 2 视为下线。
  - libIOABP 的 channel/control selector 与 21H461 相同。

## 2.1 同硬件与公开实现检索（2026-11-09）

| 范围 | 结果 |
| --- | --- |
| UniXNU 官方设备列表与指南 | 基带只有 QcomBaseband/Coachz（SC7180，iPadOS 17）。指南 docs/porting/baseband.md 已逐段读过 |
| GitHub 关键词：hackios、HackiOS、UniXNU、QcomBaseband、sm8850 xnu/ios、kaanapali modem、xnu qrtr/ipa | 唯一的基带桥是 UniXNU/QcomBaseband。在 SM8850 上，ios-popsicle 已让 modem 带服务完整启动，但"未接入 iOS"；data-a-live（17 Pro）、pudding、onelineage、k90 的蜂窝都未开始。hackios 系列仓库（K20 Pro、dada、dodge 等）没有蜂窝实现 |
| Linux 主线与下游 | 主线 ipa v5.x 和 qcom_q6v5_pas；下游 dataipa/datarmnet 与 OnePlus SM8850 内核（canoe_mpss_resource）已对照 |
| QQ 历史 | 187 个关键词，1122 条精确命中，日期 10-22 至 11-06。线索见本地 research/QQ-LEADS.md（不进 Git） |

## 3. 复用表

| 阶段 | 硬件/协议 | 来源 | 已有验证 | 复用范围 | 27/nezha 适配 | 缺口 | 验证方法 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| modem 启动 | PAS/SCM、MSS、DSM 分配 | sheng QcomMdt；audio27 NezhaScm PAS 补丁形式 | audio ADSP PAS 真机 | MDT 解析、PAS 命令序列 | `NbPas`（有界、每步 64 KiB）、`NbMss`（ELF32 映像，proxy/assign 回调） | DSM 的 hyp assign_mem 调用尚未接 NezhaScm | 上机看 PAS 返回、SMP2P ready 和 crash-reason item 421 |
| 控制传输 | SMEM/SMP2P/GLINK/IPCC | sheng + audio27 + power27 | SOCCP 与 ADSP 真机 | 全部 | modem item 号；SMP2P 版本选择；intent ID 0 | 无 | 宿主模型；上机看 GLINK 版本和 IPCRTR 打开 |
| QRTR/服务 | QRTR、rmtfs、servloc、TFTP、memshare、IPA QMI | 新写（参照 Linux 语义） | 无 | — | 全部本机化；rmtfs 只在内存中保存 | memshare 是否必需待上机确认 | 上机记录服务上线顺序和每类请求次数 |
| 数据面 | IPA v5.5/GSI v12、QMAP | QcomBaseband v4.2 实现，派生到 v5.5 | Coachz v4.2 真机 IPv4 | 命令、RX、GSI、QMI 结构 | 寄存器生成器、Qtime 聚合、8 位端点、v5 status、哈希表、端点 2/24 | IPA 的 SMMU CB 和 IOVA 由包内提供者负责；时钟/ICC 投票未接 | 宿主模型；上机看 INIT_DRIVER/INIT_COMPLETE、TX/RX 计数 |
| iOS 桥 | ABM/BBUpdater/CommCenter、SAHARA、QMUX、PDP | QcomBaseband baseband/ | Coachz 21H461 真机 | 全部 | Mav21 ChipId、48 字节 hash、端点、纯 QMAP、bootstage | 27 PDP 是 Skywalk，而桥用 BSD ifnet `pdp_ip` | 上游宿主模型（派生版）；上机看 ABM 引导、QMUX 客户端、SIM、注册 |
| 系统任务 | launchd 策略 | 设备树 BootWatchdogPolicy | — | — | 在 ADT 开关打开时保留 CommCenter（集成补丁） | — | 上机看 CommCenter 进程 |
| IMS/通话 | Apple IPTelephony；modem 语音 | 第 8 节 | — | — | 设计阶段 | 依赖数据通路、ISIM、音频 | 后续阶段 |

## 4. 原厂逆向对照（摘要）

| 原厂函数/消息 | 输入/输出 | 状态 | 27 对应 |
| --- | --- | --- | --- |
| `qcom_smp2p_alloc_outbound_item` | 版本 max(1, 远端版本)、features 1 | 出站 item 发布 | `NbMss.c setup` |
| `qcom_smp2p_intr`（negotiate） | 版本一致才合并 features；v2 SSR 时把入站 last_value 清零 | 协商完成 | power27 的 `QcomSmp2p.c` |
| `ipa3_freeze_clock_vote_and_notify_modem` | 入站 "ipa" 时钟查询，回应出站 bit0（响应）和 bit1（投票） | modem 等待 IPA 时钟 | `nb_ipa_clock_query` |
| `ipa3_qmi_init_modem_send_sync_msg` | 带哈希 TLV，189 字节 | INIT_DRIVER | `ci_init_driver_req` |
| `ipa_smem_init`（主线） | item 497/0x19000，在 modem 分区 | 过滤表 | `allot_smem` |
| GLINK intent（SOCCP 实测） | ID 可以为 0 | intent 表 | `QcomGlink.c` |
| BBUEUR20Loader | PK hash 长 0x30 | SAHARA 读寄存器 4 | `cbs_default_pk_hash` |
| AppleBasebandPCIControl `bootstage` | SInt32，值 3 表示运行 | 上线/下线 | `coachz_baseband.cpp` |

## 5. 已实现与待实现

已实现（提交 1911770 起至本文所在提交）：
- QRTR 路由；
- QMI 编解码；
- rmtfs（内存）；
- servloc、TFTP、memshare；
- PAS 加载器与 MSS 生命周期；
- IPA v5.5 全部派生源；
- IPA 板层；
- baseband 工作线程；
- 27 桥派生与胶水；
- SMP2P v1/v2 与 intent ID 0；
- modem SMP2P item 方向修正为入 435/出 428，PAS 元数据限制在 4 GiB 以下（均已按原厂确认）。

待实现，需等指定测试包，依赖包内提供者：
- 启动 caller（`integration/NbBasebandStart.c` 草案见集成文档）；
- SCM PAS/assign_mem；
- IPCC 中断分发；
- IPA 时钟/ICC 投票；
- IPA SMMU 租约；
- 固件读取端；
- IOKit 类初始化；
- launchd 策略补丁；
- ADT 节点。

## 6. 生命周期与错误处理

- 只有一个工作线程，轮询有界。顺序：IPA 电源 → SMMU → ipa_config → ipa_fws（PAS 15）→ ipa_setup → modem PAS/启动 → 服务。任何一步失败，本次开机就停止基带，并打印阶段和错误码；系统其他部分不依赖它。
- modem 崩溃（SMP2P fatal 或看门狗中断）时：桥上 bootstage=0，`coachz_baseband_down`，IPA 进入 down。第一阶段不重启 modem，IPA 端点和 DMA 内存保留到下次开机，不做没有证据的释放。
- PAS shutdown 返回 -EINVAL 时，按 Linux 解密关机的规则重试（50 次，间隔 100 ms）。只有 PAS 证实停止后，才撤回 DSM 分配。
- 所有服务输入都做长度和 TLV 边界检查；未知请求不回应（与 rmtfs 一致）；TFTP 会话有超时和数量上限；QRTR 发送队列有界（32×8 KiB），满了就计数丢弃，不阻塞。
- 桥：ABM 每个通道最多缓存 1024 帧，前 32 次丢帧记录头部；QMUX 客户端在 DEL_CLIENT 或 modem 下线时作废。
- EFS 写入只留在内存，**从不写回**任何分区或文件。

## 7. 测试输入与用例（宿主，`sh tests/baseband27/check.sh <QcomBaseband>`）

| 文件 | 覆盖 |
| --- | --- |
| `ipa55_values.c` | 端点寄存器写序列、mem_setup 4 条和 table_setup 8 条命令、过滤位图 0x94e6、INIT_DRIVER 189 字节 |
| `ipa55_model_test.c` | 纯 QMAP 出站/拆包（含 pad、命令帧、截断、未知 mux、只有 pad 的帧）、v5 status（8 位 src/dst、tag、miss、异常、越界）、TRE 预算与门铃 |
| `bridge27_values.c` | ChipId、48 字节 hash、DPM 端点 2/24、WDA 只发一次纯 QMAP |
| `coachz_bb_model_test27.c` | 上游 836 行桥模型（QMUX/CTL、SAHARA、方法、通道、数据口与激活、eSIM），只改 27 的预期值 |
| `transport_model_test.c` | GLINK intent ID 0 收发与 RX_DONE、零大小判为错误；SMP2P v1、预先存在的 v2、v2 SSR、版本 3 拒绝 |

2026-11-09 全部通过（gcc，ASan/UBSan）。上机用例见集成文档第 9 节。

## 8. IMS/语音设计（后续阶段）

- 本地24A446的 `libIPTelephony` 有真实 SIP、AKA、IPsec 与 IMS 传输实现，
  CommCenter 的 `EurekaCallCommandDriver` 创建 VOICE 客户端；
  `QMISMSCommandDriver` 实现 WMS 初始化、路由、接收确认和发送。
  原先仅凭 CoreIMS 的名字把它列为电话 IMS 来源不准确，已纠正。
  函数边界、原版字节和调用链只保存在私有 `research/abi-next002/`。
  首选设计路线 A：AP 侧 IMS 跑在 modem 提供的 IMS PDN 上
  （WDS 的 IMS APN 与独立 QMAP mux，当前桥为 `pdp_ip1`）。
  鉴权经 UIM 的 ISIM/USIM AKA，媒体依赖 audio27 的麦克风、听筒和扬声器；
  上述系统消费者与本机传输、IMS 注册、媒体均待真机验证。
- 备选路线 B：modem 侧 IMS，同原厂安卓。语音 PCM 经 modem ↔ ADSP 语音图，需要 audio27 提供语音 DAI/图，并把 BasebandVoice.driver 的通道映射过去，工作量更大。
- 依赖：
  - 数据通路稳定，IPv4/IPv6 双栈；
  - 运营商捆绑包里的 IMS 配置；
  - UIM 的 ISIM 应用和逻辑通道；
  - audio27 提供的已验证播放/录音；
  - 人类指定的测试号码。
- 禁止拨打紧急号码，紧急能力按实际实现上报；不猜 PIN/PUK。

## 9. 验收标准与阻塞

数据阶段（按顺序、分别取证）：
1. modem 运行；
2. QRTR 服务上线；
3. IPA INIT_COMPLETE；
4. ABM 引导完成；
5. QMUX 客户端；
6. SIM 状态和 ICCID 读取（只本地记录）；
7. NAS 注册在 LTE/NR，含 NSA 锚点和 SA 状态；
8. WDS 会话、地址/DNS/MTU；
9. `pdp_ipN` 收发计数；
10. DNS 回包、TCP/TLS/HTTP，以及系统浏览器真实访问；
11. 断开/重连、飞行模式、重复开机。

5G 要另外拿出 NR 承载与同一次数据流关联的证据，状态栏图标不算数。

阻塞：
1. 固件和配置树的交付方式需要人类决定，见集成文档第 6 节（新钉住文件方案）。
2. 27 PDP 是 Skywalk，桥用 BSD ifnet，CommCenter 是否接受要上机验证；备选是适配 PDPQMAPSkywalk 的接口。
3. CommCenter 的推迟票据（postponement ticket）条件：27 里仍有 `/cc/props/is_postponement_ticket_available`，可能需要 27 版兼容副本。
4. memshare 是否必需。
5. EUR20 的 SAHARA 映像 ID 清单。
6. IPA SMMU 和时钟投票在指定包里的提供者。
7. 测试用 SIM、运营商和测试号码由人类提供。

## 10. 独立副本接续与上游复查（2026-11-13）

人类要求保留原工作区，在完整独立副本
`/home/hua/nezha-work/ios27/baseband27-codex-next-20261113/` 接续开发。
本设备分支为 `codex/baseband27-next-20261113`，起点 `0341339`，
保留原先指定的 GPU024/source `673cde8`。原工作区五个仓库的提交和工作树未变；
副本 Git 对象独立，208 MiB 本地镜像已逐字节比对。

已重新 fetch UniXNU/main `2366e33`、myron-next `f1a1bfe`、QcomBaseband/main
`dd0199c`、Trogdor/main `1df7f75`，并检查同 SoC 公开设备、内核、模块、分支、
QcomBaseband 的 fork 和已关闭 PR。GitHub 的 SM8850 两页仓库索引及按
`canoe-modem-pas`、`AppleBasebandPCIMAVPDP` 检索的原始结果留在 Git 外的
`research/github-search-20261113/`。带 fork 的宽泛 README 检索有大量重复构建器，
这里只按实际驱动和协议源码评估；没有将仓库描述当作功能验收。

| 来源与固定提交 | 核实的范围 | 接续采用方式 |
| --- | --- | --- |
| UniXNU/QcomBaseband `dd0199c` | main 和唯一公开 fork 提交一致；Coachz/iOS17 的控制桥、IPv4 与 PDP 重连已有作者实测；IPv6、长会话仍待验 | 保留既有 BSD 派生基础和计数式生成器 |
| chiba233/Trogdor-ipadOS `1df7f75` | `ffbda28`、`1df7f75` 新增卡端点映射、选择和 eSIM 清理；未同步到 QcomBaseband/main | 参考 UIM 请求和代际处理；eSIM profile 操作不混入本机双实体卡阶段 |
| OctoDevStudios/ios-popsicle `a4ef4ed` | 同 SM8850，MSS 带服务启动、RAM 存储；尚无 iOS27 数据/通话桥 | 复核 PAS、SMEM、SMP2P、rmtfs/TFTP；无许可的文件只参考事实 |
| lx200332/popsicle-ios-port `31e0626` | 有 modem/板级资料和源码，但不能由文件存在推定联网或通话 | 对照原厂地址与初始化边界，不作为真机功能基线 |
| lolipuru/android_kernel_xiaomi_sm8850-modules `73e69d3` | dataipa 的 IPA_5_5、datarmnet/QMAP 与音频 UAPI | 对照本机原厂和既有派生；保留许可，寄存器与协议事实不作为 Linux 代码拷贝 |
| lolipuru/android_kernel_xiaomi_sm8850-devicetrees `e942232` | `qcom/canoe.dtsi` 的 MSS、IPCC 和 rmtfs | 板级范围由本机 N0/原厂资料最终确定 |
| OnePlusOSS kernel `fc30e54` / modules `5ab2a68` | canoe PAS、下游 IPA/GSI/rmnet 与 DTS | 对照 ELF/PAS、内存、服务和数据面 |
| map220v/sm8850-mainline `b9e356d` | IPA v5.5 / GSI v5.0 寄存器源码 | 检查生成器使用的寄存器事实，不整体升级框架 |
| linux-mobile-broadband/libqmi `3454c2c` | CTL、UIM、WDS、VOICE 消息布局 | 核对线上字段与测试输入 |
| myron-device `d9b6be4`、songyuan-device `b489567`、UniXNU-pma120 `06d8772`、公共 SM8850 SoC `01dc053` | 未找到可直接采用的 iOS27 蜂窝/电话实现 | 只采用有源码或证据支撑的相关部分 |
| pandora/data-a-live `d2c7089`、fairlady `bb05659`、cas `8e2d37a`、umi `3eb7257` | 复查源码和现有接口，未找到可替换本机蜂窝桥的实现 | 保留硬件和组件参考 |
| Pagani_SM8750 `fcab49e` | 缓存源码无基带实现；新副本 fetch 遇到 unresolved delta | 只记录已读提交，不宣称最新远端已检查 |

已有原厂只读表含 64 条 modem 服务公告，其中 52 个不同服务可由 QMUX 的
8 位 service 字段表示。`coachz_bb_qmux.h` 的 CTL GetVersionInfo 目前最多返回
32 个服务；按该表的顺序，后面的 UIM、WDS、NAS、DMS 会被漏报。
这是接口清单的可复现缺口，尚未把它称为实机启动失败的根因。

接续修正：由 `derive-bridge27.py` 生成包内的 `bridge/coachz_bb_qmux.h`，
版本清单覆盖已有的 64 个 server 槽；未修改 vendored 上游原件。
`qmux_version_test.c` 对原版复现 52 项漏报，对修正版验证完整服务成员、
VOICE/IMSS/IMSA、64 项边界及不足一字节的应答缓冲。
该回归用 `-Werror`、ASan/UBSan 通过；使用恢复的固定 SMMU 输入后，
`sh tests/baseband27/check.sh ../research/upstream/QcomBaseband` 全部通过。
此结果为宿主源码资格，iOS27 原生编译与实际注册、流量和通话仍待验证。

2026-11-11 的 CI 快照 `7cd6502` 已存在，旧 CHECKPOINT 对“快照未做”的记录
滞后；本轮仍需核原生构建、依赖闭包和真机结果。原宿主测试用的 SMMU 路径已经
缺失，固定 `ArmSmmu4K@83969cd` 已恢复到副本 `research/owners/ArmSmmu4K`，
通过现有 `BB_SMMU_SOURCE` 接口选择。REA 当前 Windows provider 未配置 Ghidra，
其 Windows 深分析范围也不覆盖 ARM64 Mach-O；本地二进制事实继续用已有
WSL 解析/反汇编工具核对，不能把普通脚本输出称作 REA Evidence。

## 11. 端点绑定与电话、短信控制通路接续（2026-11-13）

本次已通读“新基带”会话、上述交接和现有实测资料；仍在独立副本开发。
新 fetch 的 UniXNU/main `4796a8a` 仅更新设备记录；myron-next `f1a1bfe`、
QcomBaseband `dd0199c`、Trogdor `1df7f75` 不变。GPU024 的选择和现有
共享控制器来源保持。

原 QMUX 接收函数丢弃 QRTR 来源端口，只按节点和服务最低 instance 查询。
由 `derive-bridge27.py` 生成的修正版为每个客户端固定实际 node/port；
同节点其它端点、短包、非回应/通知及尾随字节被拒绝。后到的较低 instance
只影响新分配的客户端，已有客户端不被静默迁移。DEL_SERVER 撤销实际端点
的客户端，生成 CTL RevokeClientId，保留对应槽直至通知入队及 DEL_CLIENT；
已有队列发包前再次核对该端点。原上游文件保持原样。

来源是 QcomBaseband 的公开桥接口、QRTR node/port 契约与 libqmi
`3454c2c` 的服务定义。VOICE `9`、WMS `5`、IMSA `33`、IMSS `18`
均由现有 QMI 通路保留请求、回应和通知内容，不把 Android RIL 二进制移进 XNU。
`qmux_endpoint_test.c` 的319项检查覆盖两条通道、16位 transaction、
完整内容、不同实例、撤销与重分配、错误端点和帧边界；全部使用合成数据，
ASan/UBSan与`-Werror`通过。完整宿主检查和29单元Linux/17头文件编译检查
通过；它们不代替 iOS27 原生 ABI 或真机结果。

电话/短信最终仍须验证 CommCenter 对本机服务的实际消息和能力：
VOICE 呼叫状态、IMSA 注册、WMS 收发及系统入口分别验收。通话音频依赖
音频专项的真实播放/录音与 voice graph。当前没有拨号、短信发送、手机预约
或 iOS27 蜂窝真机成功的记录。

## 12. 首次实机阻塞与低地址内存修复（2026-11-13）

Native002/run `43302261690`、源码 `8849f74` 已真实 arm64e/iOS27 编译，
严格38组件闭包、PAC、固定GPU024配套与临时镜像检查通过。
208MiB带索引资料镜像已在批准的独立钉住文件中安装一次并全量读回；
616条目、66段extent的映射保持，镜像和本机专属内容只在私有路径。
第一次 B002 实机于415.793秒通过归档读取/校验，参数页分配返回 `-2`；
没有 PAS 调用、MSS 启动或 QRTR 服务，数据/电话/短信未验。
一次正常 XNU 返回后的安卓/root、文件映射/头与采集器退出通过并释放。

本机 XNU 只拥有 `0x8c0000000` 起的高RAM，旧
`inTaskWithPhysicalMask(...,0xffffffff...)` 无低4GiB页可分。
独立 DarwinBoot 从 UEFI 实际空闲页分配5MiB `EfiReservedMemoryType`，
将物理地址填进 `/chosen/nezha-baseband-low-memory` 的16字节占位。
开关缺省或0时不分配；没有固定物理地址后备。
`NbLowMemory.h` 在该保留池中放参数4KiB、三份元数据各64KiB、
rmtfs共享4MiB，XNU只建立一份WC映射。

缓存同步复用音频 `NezhaAudioAdspInputs.cpp::na_native_pas_sync` 已实测的
映射语义和既有GPU/Pandora PAS loader的DSB：
低池与三份固件WC映射只在真实物理范围和CPU别名一致时发布写入；
SCM共享拥有者也通过同一低池检查发布参数，不再对低地址调用高RAM physmap
缓存清理。原高RAM SMMU表缓存清理、SCM锁/WAITQ、原厂固件地址保持。
宿主测试实际走过三份PAS拷贝/清零/同步、低池边界、错误CPU别名、
越界和SCM参数注册/终止失败。另修正两处已有原生编译警告的明确初值/计数写法。

必要上游复查：UniXNU/main `dff2765` 为新APFS/macOS存储改动，
myron-next `f1a1bfe`、QcomBaseband `dd0199c`、Trogdor `1df7f75` 不变；
固定GPU024框架未升级。下一候选为 Native003；低地址池实际分配、
PAS/DSM/MSS ready、QRTR、IPA和系统蜂窝仍按阶段上机验收。
普通测试号码已由人类指定，仅存在私有文件，尚未拨号或发送短信。

## 13. 双实体卡的激活身份（2026-11-13，独立004源码）

003已全部电脑合格并文件排队，完整boot/payload和入口保持固定。
等待手机期间，在独立 `sim004-device` 分支 `codex/baseband27-dualsim004`
修正现有桥把两张卡的EF_ICCID/EF_IMSI都填入slot1激活记录的缺口。
原 `upstream/` 字节保持，修改仍由计数式生成器导出。

`NbSim.h` 和 `NbSimBridge.inc` 只被动读取CommCenter已有的UIM请求/回应：
按TLV1的真实会话类型、TLV0x10的四个provisioning index与完整卡/应用/AID表
定位物理卡；真实primary/secondary subscription各自读取该卡的身份。
端点、local port、transaction和mapping generation一致才收下EF回应。
卡移除、端点撤销、mapping更新丢弃对应旧请求；新ICCID不混入旧IMSI。
两张卡分别填入slot1的0x42/0x43与slot2的0x46/0x47，并在slot2实际身份
已读到时填0x51 provisioned。没有发新卡请求、切槽或PIN/PUK操作。

来源：QcomBaseband现有EF读取/BCD和BSP激活语义，libqmi固定`3454c2c`
的qmi-service-uim.json/qmi-enums-uim.h，24A446原版
QMIMav13ActivationCommandDriver的两卡parser与consumer函数。
实际ICCID为10字节、IMSI为8字节，两卡getter和consumer字节只保存在私有
`research/abi-next002/dual-sim-activation-callers.json`。
不再沿用iOS17记录里ActivationInfo内部字段偏移去推定iOS27布局。

新增宿主模型使用合成双卡，覆盖同transaction不同client、两卡不同身份、
provisioning反向映射、删除卡、失败和错误端点、旧代回应、所有短帧、
未知会话和新ICCID等待IMSI。ASan/UBSan/`-Werror`、完整host及29单元
Linux17头编译检查通过。实际arm64e/iOS27与双卡运营商匹配仍待验证。

## 14. 原生PDP flow映射（2026-11-13，独立005源码）

003仍按文件队列等待，004原生双卡候选已封包/部署完成；两份成品保持固定。
独立 `data005-device` / `codex/baseband27-pdp-flow005` 补现有
AppleIPAppenderUserClient的addFlow/removeFlow/getFlow零回应缺口。
这三项只保存/返回系统请求的traffic class→flow映射，不发WDS或选择mux。

来源：QcomBaseband的既有PDP服务与用户客户端、24A446原版AppleIPAppender
21项dispatch表及其原生方法，ApplePDPHelper实际调用。
selector0输入是8字节结构（两个little-endian u32）；1是一个scalar class；
2是一个scalar class和一个scalar输出。原版table只接受class0–9，
add写该格、remove清零、get返回该格。`NbPdpFlow.h/.inc`直接实现这段语义，
使用现有PDP锁，错误输入不改变原映射。其它selector保留当前行为。
没有把原版enable的零输出视为支持启用：Apple默认实现自己拒绝true，
具体媒体/数据能力仍需后续拥有者与真机消费者。

字段/dispatch/方法字节留私有`research/abi-next002/original27-appender-*`。
全十格设置/查询/删除、完整输入输出形状、短结构与越界及错误不改变状态
使用ASan/UBSan和Werror通过；完整host、29单元Linux17头screen通过。
005实际arm64e/iOS27与数据流尚未验证；不更新003手机排队资格。


## 15. 原生电话音频格式观察（006，2026-11-13）

本机原厂 `libqmiservices.so` VOICE v2/minor162 的IDL表确认了
BindSubscription0x44、AllCallStatus0x2e/GetAllCallInfo0x2f，及
SpeechCodecInfo0x53/GetSpeechCodecInfo0x6e的实际TLV、枚举和长度。
通话数组为1字节计数、每项7字节、最多7项；查询回复用TLV10，通知用TLV01。
codec TLV11为4字节、encoderHz TLV12为4字节、callId TLV13为1字节。
公开wire-os voice_service_v02.h只作字段名称和枚举注释参考，最终格式核原厂表。

Apple原版24A446 `QMIAudioCommandDriver::queryVocoderInfo` 查询AUDIO(e1)
message2f；VocoderTypeInfo通知为30，TLV01为codec/rate两字节。
实际codec映射、rate解析及两个方向的写入例程已核字节和边界。
`NbVoice.h` 只被动观察已获QMUX验证的原VOICE流量，不新增拨号、SMS、注册、
切卡或modem请求。明确成功的订阅绑定和当前通话才能发布；卡2/相同callId隔离，
结束/撤销/新绑定清除旧格式。未知codec/rate返回原NOT_SUPPORTED。
`NbVoiceBridge.inc` 沿原锁与发送队列回答真实格式查询和格式通知；硬件
AudioRateSwitchComplete及路由/静音/音量仍保留原不支持，不能替代媒体验收。
UEFI低内存、PAS、IPA、双卡身份和PDP flow沿005保持，003–005固定成品不动。
UniXNU/main dff2765、myron-next f1a1bfe、QcomBaseband dd0199c已fetch，
未在现有同SoC参考中找到该Apple/原厂VOICE转换，沿现有QMUX bridge补必要部分。


## 16. MSS已有SMEM寿命修复（007，2026-11-13）

003真机60.895s在check_cold因已有SMEM项返回OWNERSHIP(-5)，此前
真实低池/归档-RAM/SMMU/IPA三PAS/GSI已通过，MSS主固件/DTB的PAS0。
因此存在项不能作为基带已启动的证据；XNU不能凭空重置这些项。

按同SoC音频clock071 `AdspLink.c`（源6733d4a，原生/实机路径）的
initial proxy→PAS主/DTB stop合同，以及Linux `qcom_glink_smem.c` 的
EEXIST→确切尺寸检查/仅重置AP索引，007先确认PAS4和0x26实际停止。
任何非decrypt停止失败均终止，不触DSM/旧SMEM/固件。modem主停止
仅沿既有qcom_pas_shutdown_poll_decrypt的-22/100ms/50次处理，不将-22当已停止。

随后两DSM赋权、重用340字节OUT并在共享锁内发布SMP2P；GLINK的32字节
descriptor/16KiB TX在新的主认证和fresh ready后才建立。RX未绑定前不poll。
保留remote TX-tail/RX-head；caller先接IPCRTR/DS再进行第一次FIFO处理。
watchdog基线同doorbell取在主固件认证前，旧事件不代表新固件故障。
诊断已有stage编号保持，新initial power/stop追加18–20。

007包含独立006原VOICE→AUDIO格式观察，003–005成品不动。
UniXNU/main新7d85ede仅文档记录、myron-next f1a1bfe，固定GPU024框架不升级。
宿主实际MSS状态机已验成功/两停止失败不写/繁忙/-22延迟/错尺寸/
旧ready/新watchdog/remote索引；必要Mac原生构建及实机结果待验证。


## 17. 原始PAS返回与PIL只读前检（008，2026-11-13）

007已实机到MSS proxy完成；PAS4和0x26停止均被包装为NB_IO(-2)，
原始安全返回未记录。旧MSS/SIM/IPA与成品不改，008补这一个真实诊断缺口。
按同SoC音频 `NezhaScm.c::nezha_audio_pas_call/query` 及已实际使用的
PIL helper，在原SCM/WAITQ拥有者记录实际a0..a3后再返回原状态。
QUERY7为支持状态，QUERY9为允许固件内存范围，a1是查询数据不能作为setter错误。
公开tz_syscall_pub.h的GET_MEM_AREA_ID=9确认语义，它不能证明remote stopped。

MSS在proxy完成后且第一次stop之前执行固定4查询（4:7/9，0x26:7/9），
每条提交前记录query_mask，失败和后续tick不再执行。查询不修改停止路径，
任一实际stop失败仍阻止DSM/旧SMEM/基带加载；rawreturn保留未知TZ码和边界。
无新SMC/锁/USB任意选择器，无猜测PAS ID、NV、运营商写入。
宿主实际wrapper和MSS已验查询data非setter错误、a0失败、实际-22解密路径、
proxy/查询/stop顺序、失败不绕过、繁忙/后续tick不重复。29Linux17头编译screen通过，
真实iOS27原生对象及下一实机rawreply待验证。

## 18. IPA统计SMEM页表映射（009，2026-11-13）

008真机在63.660s收到MSS fresh-ready，主/DTB初始化、内存设置、认证及
初始停止原始寄存器全0，GLINK/QRTR后收到modem IPA服务和65.863s
INDICATION_REGISTER。随后无返回请求而复位，OEM历史在10:14:18UTC
记录`tz_others`，未给出地址；不把查询时序当稳定停止修复，也不称XPU违规。

已确认的缺口是item497统计区没有加入替换IPA上下文的页表：
OnePlus SM8850下游`5ab2a68`的`ipa_smmu_ap_cb_probe`先按真实SMEM
地址对齐并作恒等映射，再把未对齐地址给`per_stats_smem_pa`；
Linux `ipa_mem.c::ipa_smem_init`同样映射该页段。本机原厂
`ipa_bootup`已记录102400字节、AP SID0x4a0、uC SID0x4a2。
框架/其他设备没有现成nezha绑定，沿已有QcomSmem和ArmSmmu4K接口补齐。
本轮开工fetch UniXNU/main `7d85ede`与myron-next `f1a1bfe`，未升级GPU024。

009只读查已有apps/modem分区item497，要求原102400字节，
在原SMEM mutex下取其真实地址和页段；不分配或清零。IPA尚未开启时
将该段加入原六范围的同一32位IOVA/36位物理页表，RW/NC/outer属性保持。
QMI使用同一个已映射的原始item地址，不在MSS认证之后再创建或更换统计区。
保持原PAS门槛、固件、DSM、GLINK、IPA/GSI寄存器和Data/存储权限。

宿主真实绑定已走出实际TTBR页表，验证未对齐item首尾均恒等映射、
相邻未声明页仍缺失，原DMA和IPA寄存器区可达；缺失/错尺寸/损坏SMEM拒绝。
完整ASan/UBSan回归通过；实际iOS27原生、IPA握手和复位效果待下一轮。
来源仍固定GPU024/SMMU/USB/common；最新fetch UniXNU7d85ede、myronf1a1bfe。

## 19. MSS初始停止前移与统计区创建（010，2026-11-13）

009实际本机在60.631s查不到SMEM497，setup=-7；IPA配置、IPA PAS、
MSS PAS和QRTR均未开始。该结果比008退步，证明不能假设安卓分配的
统计项跨临时启动仍存在。009已一次正常返回安卓/root、通过必要保护后验，
63132采集器退出，12:41:12UTC释放；其固定源码与成品保留。

010沿007音频复用的initial proxy→PAS4停止→PAS0x26停止路径，
将这段从原MSS启动流程中先执行，成功停在NbMssCheck。任何实际停止
失败仍终止，不修改SMEM、DSM或IPA；保留008四查询及原始返回。
随后在原SMEM硬件mutex内调用既有QcomSmem查询/分配接口：
已有497要求原102400字节且不写，缺失才创建一次，读回真实地址后
沿009精确页段建立原IPA页表。QMI仍使用同一个实际已映射地址。
IPA固件和配置完成后，原MSS状态机从Check继续DSM/SMEM/认证/fresh-ready。
原阶段号保留，新的worker InitialStop仅追加；不新建trace设施。

统计项分配及恒等映射合同来自同SoC OnePlus SM8850下游5ab2a68
`ipa_smmu_ap_cb_probe`和Linux `ipa_mem.c::ipa_smem_init`；实际分配、
mutex、页表和PAS逻辑分别复用QcomSmem、SM8850 common、ArmSmmu4K及
已集成的NbMss。GPU024、低池、DMA/GSI/IPA寄存器、PAS参数和存储权限保持。
009结果后fetch UniXNU/main7d85ede80092050f4d87f72ac7229bb605ce07fb
与myron-nextf1a1bfe561226dbb48e2904e0a2e74c67d2485ed，未升级固定框架。

实际C worker/MSS/SMEM状态机回归验证两停止前无IPA电源/分配/映射，
缺失创建和已有原样复用、两停止失败均不写且不重发。C++真实board绑定
以两个进程分别测试缺失/已有项，实际TTBR首尾恒等映射、相邻缺页、
原DMA/PAS路径均通过；完整ASan/UBSan/Werror宿主回归通过。
宿主检查不代替原生目标与真机IPA握手、移动数据、电话或短信验收。

## 20. 原厂MSM外设统计初始化（011，2026-11-13）

010实机已确认两MSS初停、497创建/映射、IPA和MSS认证成功；
收到IPA INDICATION_REGISTER后仍自发TZ复位，与008停在同一位置。
原厂复位记录没有违规地址，本轮不据此推定XPU或SMMU根因。

已对照同SoC下游`ipa_stats.c::ipa3_peripheral_stats_init`的MSM分支，
并核本机原版ipam.ko同名函数：统计区首104字节含12条Q6定义的
记录，类型依次0/1/2/7/8/9/10/11/12/13/17/19，长度均4，
计数/枚举初值0。010仅复用QcomSmem分配的零区，缺少这个初始化。

011在两MSS成功停止、原SMEM mutex内、IPA/固件开启之前补齐这些
记录并读回。保持canary、reserved、104字节之后的全部原内容；
不清空2KiB前缀或102400字节整项。沿已有32位对齐SMEM访问和DSB发布，
SMMU enable重试不再初始化；原统计地址、页段、QMI字段、DMA、
IPA/GSI、PAS、DSM、GLINK、GPU024与存储授权保持。

来源为OnePlus SM8850模块仓库5ab2a68的MSM初始化及本机原版函数。
开工fetch UniXNU/main7d85ede与myron-nextf1a1bfe，固定框架未升级；
pandora、Pagani、fairlady、cas和umi没有可直接搬的同版统计绑定。
独立电脑比对执行原厂ARM64函数的MSM分支，只重定位ipa3_ctx两条引用，
零区/已有数据两例的2KiB结果与本候选完全相同。ASan/UBSan实际
SMEM及板级绑定验证两停止门、原标记/尾区保持和重试不重置计数；
29单元Linux17头编译screen通过，原iOS27目标和真机结果另行验收。

## 21. IPA v5.5 uC描述符首字（012，2026-11-14）

011真机已验证12项统计读回、IPA/MSS认证和fresh-ready，但同IPA注册20后
自发复位；OEM记录仍为tz_others，没有实际违规地址。012补齐另一个已核实
的初始化缺口，复位因果关系仍待真机。

同SoC OnePlus SM8850下游5ab2a68的`_ipa_init_sram_v3`在IPA>=v5.5、
descriptor区非空时，于canary之后写首字`0xABCDEF01`。本机原版ipam.ko
的0x6d40..0x6d80实际指令相同：版本比较24、检查区大小、构造常量并写入。
本机布局为descriptor偏移0/大小0x300，uC共享命令区另在0x300/大小0x80；
011实测SHARED_MEM_SIZE=0xa00，即mem_offset0/mem_size0x5000。

012在现有mem_config中保留canary写入，只补该四字节marker并沿既有printf
读回；生成器同步修改。原SRAM表、uC命令区、MODEM区、DMA清零范围、
统计/INIT_DRIVER参数、时钟与SMMU/PAS、GPU024、存储权限均保持。

实际配置函数以宿主MMIO执行，验证当前大小、较大报告、非零restricted
偏移及无效大小；逐字节核对全部SRAM、共享命令区、descriptor尾区和DMA
边界。原厂ARM64标记分支不改指令，以显式ipa3_ctx布局执行v5.5/旧版/空区
三例，只有v5.5非空区写该首字。该证据和ASan/UBSan通过只证明初始化合同。
开工fetch UniXNU/main7d85ede与myron-nextf1a1bfe；继续沿第10节现成来源
检索结论，固定GPU024框架不升级。实际iOS27编译、完整IPA握手和移动数据、
电话/短信结果单独验收。

## 22. IPA CONFIG请求应答（013，2026-11-14）

012真机61.849s读回descriptor首字，65.843s完成uC与全部IPA握手，
TX11/RX1进入Running并持续至570秒；原生CommCenter仍在SIM激活前反复退出，
待取实际崩溃。握手后远端65.843s发送CONFIG0x27，67.938s发送INSTALL_FILTER0x23；
当前桥均未应答，后者需要真正的过滤规则安装，不能用通用成功响应替代。

同SoC OnePlus SM8850下游5ab2a68的`handle_ipa_config_req`调用
`ipa_mhi_handle_ipa_config_req`后回复结果。本机原版ipam.ko实际函数
0x104e4c..0x104ef8只有现有日志调用并返回0，不读取配置内容或改动硬件。
013采用这个已核实的CONFIG应答合同，沿原QMI writer回复同transaction；
只允许REQUEST0x27，不将CONFIG错误视为uC ready或indication注册。
发送失败仍不推进状态，原0x20/0x35初始化和0x23未实现行为保持。

实际nb_qmi函数抽取后以ASan/UBSan执行，验证同transaction的14B回复、
CONFIG前后握手状态不变、发送失败不推进、indication不误应答、0x23无假成功
与真实0x35仍置uC-ready。开工fetch UniXNU/main7d85ede和myron-nextf1a1bfe，
沿第10节公开实现检索；未升级固定GPU024框架。此源码步骤只补CONFIG，
不能独自证明CommCenter修复、过滤规则、移动数据或电话短信可用。

## 23. 原生CommCenter的CPMS能力与本板一致（013，2026-11-14）

012的CommCenter PID95于124.926s退出；只读完整报告的procExitAbsTime
2398441163/19.2MHz与此对应，bootUUID127BB615-FBBA-4EDE-A24F-773F6795D7D0。
faultingThread在cpms.mod，libBasebandManager原UUID
e7e1cd9b-2938-3519-92a0-7a119279a107，bootstrap回调偏移0x798d0的brk1；
原指令与报告instructionByteStream完全对应。断言条件为fCPMSAgent非空，
之前sharedCPMSAgent返回null；此时尚无宿主QMUX、SIM激活或WDS数据请求。

原CPMSModule::create仅在capabilities::abs::getCPMSVariant返回1时创建；
其余值沿原User-space CPMS not supported分支返回空模块。原CPMSAgent依赖
ApplePPM服务/CPMSSupported属性、UserClient和通知，固定GPU024没有这个
Apple专用功率预算实现。实际SM8850温控/电源实现仍由既有驱动负责。

013以已存在的native-text-fixup在签名验证后的RAM页中，将该能力查询入口
返回0；选择原生不支持路径，不改bootstrap断言、不模拟预算代理。
原文件/root/Data不写；固定IPSW库UUID40dce84e-e426-333e-b135-9f1c600e3cb0、
共享缓存.19文件偏移0x4fbd668和12字节原文均需吻合。配置见
integration/cpms-capability.json，保留原PACIBSP入口与原栈指针，再用mov w0,0
和RETAB认证返回调用者；目标内核已有的签名页替换机制负责一致性。电脑解析和原函数
控制流需单独验证，实际CommCenter能否继续初始化仍待下一候选真机。

## 24. 原生启动控制器接入实际板级基带（014，2026-11-14）

013实机已走原CPMS不支持分支，CommCenter PID95没有再退出；MSS/IPA/uC
完整握手保持480秒。原生日志的同boot UUID 7E13EFA7、625/12 timesync显示
140.600s的BootControllerLegacy::prepare找不到donor Mav21的preboot固件目录，
BootModule随后将状态改成Dead；全部QMUX客户端停在kStopped。因此这次适配
原生引导与本板实际PAS引导的边界，不修改已成功的固件认证和加载顺序。

NbBoot.h从现有worker核PAS三映像、MSS fresh-ready、非fatal/watchdog、
IPCRTR/QRTR和WDS/DMS/NAS/UIM服务，以及IPA已打开状态。NbBridgeGlue只发布
该快照，watchdog立即撤销，并在userclient查询时复核原watchdog计数。
既有AppleBaseband selector28的板级属性0x4e42返回该值，不表示SIM注册、
激活或联网。没有就绪、查询失败、故障均返回0。

native-boot-query.json复用原24A446 BootController对象内0x10的
TelephonyBasebandController句柄，以及原GetBasebandProperty接口。具体prepare
入口在原签名验证后的RAM页查询0x4e42；bootModem通过保留PACIBSP的分支再核
同一真实就绪状态。两次结果继续由未改的BootModule异步流程处理，原IPC服务
开启、QMUX请求、错误与状态事件仍按原生代码。原root/Data和缓存文件不改，
只有固定libBasebandManager UUID的原字节匹配才应用，入口/栈/RETAB保持。

来源为已采用的QcomBaseband dd0199c控制userclient、现有nezha PAS/MSS/QRTR/IPA
worker，以及本地24A446原库调用者与实际013日志。开工fetch UniXNU/main
14236be（文档更新）和myron-next f1a1bfe；复查pandora/Pagani/fairlady/cas/umi，
没有可替代的同SoC原生启动适配。GPU024框架87bb1c5c保持。实际指令10状态组合、
原签名页应用器7条和主机测试通过；新的arm64e构建和真机SIM/WDS验收仍待完成。

## 25. 原控制对象到context的包装入口（015，2026-11-14）

014实际七个RAM页修改均应用，MSS/IPA保持320秒、CommCenter没有退出，
但原生prepare仍失败。只读正确boot UUID AEECF252的完整日志显示
123.393944s GetBasebandProperty返回0x10000003，内核未收到该查询；随后
原函数打印初始化哨兵0xaaaaaaaaaaaaaaaa，BootModule进入Dead。

原TelephonyBasebandCreateController的CF对象保存0x10起的context，而
BootController自身0x10保存的是这个外层CF对象。原libTelephonyBasebandDynamic
的TelephonyBasebandGetBasebandProperty（0x2bfaf0470）先add x0,x0,0x10，再跳到
TelephonyBasebandControllerGetBasebandProperty（0x2bfaf69a4）；后者直接从context
读取首word的IOConnect Mach端口。014直接调用底层入口，误把CF头当端口。
015只将native-ready-query的BL改调原包装入口，原真实worker就绪查询、七条
签名页匹配、PACIBSP/RETAB、BootModule与IPC流程保持。

来源是实际014日志及24A446原包装、create/context和callMethod指令。
指令模型执行原add+branch的8字节包装，并验证底层收到context+0x10处的
正确连接端口；10种两入口状态组合和原签名页应用器通过。仅配置和此来源说明
改变，已成功的Native014所有生产单位逐字节保持，可复用原构建产物封装015；
实际查询、SIM/NAS/WDS与流量/电话短信仍待实机。

## 26. 最新同SoC、同基带来源与015实机后续（016，2026-11-14）

本轮先查来源再适配，UniXNU/main14236be、myron-nextf1a1bfe已fetch。
GitHub覆盖SM8850的198个仓库两页索引、带fork的iOS/XNU/UniXNU检索、
同基带MPSS.DE.9.0/canoe与X85、SM8845相关候选、QcomBaseband全部分支/
fork/5个已关闭PR，并对候选实际源码与提交逐项核查；公开索引与不可访问
仓库不能证明所有未公开开发的状态。原结果留在research/github-search-20261114。

| 来源 | 本轮固定提交与结论 | 采用范围 |
| --- | --- | --- |
| UniXNU/QcomBaseband | dd0199c，main及唯一fork一致 | 沿用已派生的QMUX/QRTR、PDP和数据口；无新替换实现 |
| chiba233/Trogdor-ipadOS | e55ae43；最新增量为固件归档第9项读取，卡端点实现承接ffbda28/1df7f75 | 复用已测17协议逻辑；本机无eUICC交换，不套单待机切槽；不采用其整分区布局 |
| ShueiTsuki/data-a-live | d2c7089，已归档，明确转至yechenan | 同SM8850硬件参考，蜂窝未实现 |
| yechenan/data-a-live | 7c779bd，续作6文件增量是构建/文档/内核内容 | 无可替换蜂窝驱动；20个可取得fork主分支逐内容核，2个不可取得保留限制 |
| OctoDevStudios/ios-popsicle | a4ef4ed，MSS与服务启动，RAM文件；没有iOS蜂窝桥 | 既有PAS/SMEM/SMP2P/rmtfs/TFTP硬件事实保持 |
| lx200332/popsicle-ios-port | 31e0626，MSS驱动未实机 | 不作为功能基线 |
| Chromitsune/Portsmith-iOS | 98a41c0，QMUX与QcomBaseband逐字节相同 | 无独立新实现 |
| hxfuxyy/ios-macan、ACE6T-OS | bf6106b，SM8845；README蜂窝/SIM未完成，QMI文件是Wi-Fi路径 | 无可搬蜂窝实现 |
| myron/songyuan/pma120/pudding/onelineage | d9b6be4/b489567/06d8772/1028ed6/2506458，NiandeNian songyuan d52f416 | 未找到基带桥实现 |
| u0er nezha/Pagani/fairlady/cas/umi | 1bb442b/a84204e/bb05659/8e2d37a/3eb7257 | 全部已核，无替换蜂窝实现 |

015的双QMUX已打开，273原生QMI请求全部往返，DPM/WDA和WDS端点绑定
成功，IPA持续750秒。只读原生日志531384条、目标UUID D4604A98唯一；
35296相关记录全部按原625/12timebase校正。两实体卡被原生解析为Ready，
各自ICCID/IMSI读取成功；无WDS Start或PDP收发，未验数据/通话/短信。

本机桥仅读取UIM旧Card Status TLV10，而27请求带Extended选项。实际
libqmiservices.so的UIM对象0x30a50、type表0x29af8/message表0x2a290确认：
GetCardStatus响应TLV13和CardStatus指示TLV12都引用type41，布局为有计数
的GW索引数组、1x索引数组、卡数组，最多5卡、每卡20应用；旧type35保留。
016在原NbSim解析层支持这两种格式，应用/AID/PIN记录仍沿libqmi以及
Trogdor cbl_card_bytes已核布局，只把两授权实体卡实际映射写进激活快照。
不交换卡、不新增卡请求、不修改SIM设置；原生日志报文自身已private，
不把未取得的原233字节称为已经解码。

同015原生日志53条CLIENT_IDS_EXHAUSTED错误与实机64clients对应，NAS/
DMS客户端池和信号查询受阻。016用原QMUX算法把容量改为128；QRTR接收
入口、PDP、VOICE和释放/撤销数组均使用同CBQ_MAX_CLIENTS，板层unrouted
直接委托已有桥，无第二个64端口窗口。有效客户端/CID/transaction/端点
校验保持。96客户端收发、边界拒绝和撤销释放回归通过；扩展双卡响应/
指示、换序、移除、截断与短数组回归通过。两个新场景在原015源码确实
失败，修改后ASan/UBSan与既有完整宿主检查通过。目标arm64e及真实016
SIM映射/注册/数据尚待构建上机，不升主包。

## 27. 本板无eHRPD时保留原生空fallback分支（017，2026-11-14）

016实机扩展UIM映射gen1、两卡身份更新和71客户端已通过；原生首次
触发重试的精确原因来自016 UUID3104D5D5日志：131.395798s的卡2
3GPP attach APN命令WDS C8已成功，131.395911s仅3GPP2 eHRPD fallback
APN命令D2返回71（INVALID_QMI_CMD），131.396684s因Failure to Push
发软复位，143.495375s再转硬复位，随后高速虚拟复位和CommCenter退出。
不是把171秒退出当首个原因；MSS/IPA实际保持，原131ms双卡映射成功保留。

源码范围仅native-fallback-apn.json，固定24A446库UUIDc16c5fc9…、
cache.46偏移26a238原指令e2031caa（mov x2,x28），换e2031faa
（mov x2,xzr）。该位置只在Mav20QMIAttachApnCommandDriver的3GPP2
fallback取配置路径，调用原getApnListForType_sync；原函数先清空
返回vector，再见空key返回，后续沿原空列表成功收尾，完全不发送D2。
本板/现网LTE-NR路径保留C8和其所有真实失败响应；不报告D2假成功、
不改modem/运营商配置、射频或NV。该功能视为本板不支持的eHRPD配置。
既有原签名页验证/文本fixup在RAM应用第8条，root/Data文件不写。

四实际指令组合执行原helper起始并验证空配置vector；既有真实页应用器
对原7条加本条共8条应用/二次拒绝ASanUBSan通过。所有101生产源文件与
Native016编译源f298236逐字节相同，复用唯一Native43537052603，不再
派CI/重编；仅017新配置和来源说明，不搬17/16二进制。随后新封包需
严格原native/38依赖/PAC/HMAC/原镜像只读及唯一正常返回资格；未称重试、
联网/电话短信已修，不升级稳定/主包。最新UniXNU14236be/myronf1a1bfe
fetch和同SoC参考已核，无同基带27完整替代实现。

## 28. 双订阅NAS端口复用（018，2026-11-14）

015和016原生均打开17个NAS客户端。本板QRTR用来源端口区分客户端，
前10个真实NAS端口能完成查询或绑定，第11起的绑定/注册报error5。
016完整console实际捕获55条error5应答；原来的12条仅为有请求配对的
应答，已有日志预算会省略部分请求。NAS/WDS 0x555b由旧桥本地回答，
这些成功不算真实基带客户端成功。017固定成品先验证fallback复位，
018在独立副本处理这项已经实机复现的NAS容量缺口。

来源：QcomBaseband dd0199c的QMUX/CID/QRTR帧与原锁；
libqmi 9d17bea的事务归属、原NAS注册TLV和真实应答转发语义；
wwand f255620明确的“QRTR来源端口就是客户端”规则；本机原厂
libqmiservices NAS v1.421表（请求/响应各238、指示82）给出实际
BindSubscription 0x45、RegisterIndications 0x3字段布局。上游UniXNU
14236be/myron-next f1a1bfe和Trogdor e55ae43动手前再次fetch。
原生QMIClientPool已存在，但本轮失败的模块用createWithQueueLocal
建立各自NAS客户端；它的XPC共享池不能直接替换这些对象和双卡绑定。

`NbNasMux.h`保留原生各CID，在原QRTR owner线程分别用0x4180/0x4181
为primary/secondary建立真实端口。原Bind必须收到基带成功后才改变
该原生客户端的订阅；未绑定查询走真实默认primary Bind，不猜运营商
或改卡设置。每个真实端口一次只发一个请求，原生客户端的请求顺序、
transaction、command、代际与完整应答字节保留，发送拒绝或等待超时
返回真实失败，不把本地排队算成功。隐藏Bind错误回到原查询。

同订阅RegisterIndications按原厂89个可选字段合并“仍有客户端需要”的
报告位；仅在基带成功应答后保存各客户端配置。任一客户端取消某项
不撤掉同订阅其他客户端需要的报告。完整原始指示只发给该端口对应
订阅的现有CID。取消/释放和DEL_SERVER拒旧代回应，发送中的缓冲区
保留到owner完成发送后；NAS Reset成功才清该真实端口注册状态。
已经注册的客户端跨订阅移动目前返回NOT_SUPPORTED，保持原状态；
初始双卡绑定、同订阅重绑定及先绑定后排队查询均已覆盖。

其它服务沿原每客户端端口。新C导出coachz_modem_qmi_send仅返回已有
nb_qrtr_send_data的真实排队结果，原回调ABI不改；两NAS端口随原
DEL_SERVER/关机寿命，不引入线程、trace或复位设施。PAS、IPA、UFS、
双卡UIM身份、WDS/PDP/VOICE/WMS和原017八条签名RAM修改保持。

宿主回归复现旧17端口对十端口服务后7客户端失败，新实现两订阅端口
保留17个CID全部真实回复；相同transaction反序双卡回复、注册合并/
失败不更新、取消与发送寿命、端点错误/失效、容量和等待失败通过
ASan/UBSan、Werror。完整既有宿主检查已过；Linux17头编译仅是预检，
最终arm64e/iOS27原生、严格封包和真机仍待，不升级稳定或主包。

## 29. 原生NAS扩展指示注册（019，2026-11-14）

018真机验证了两真实订阅端口能消除017的92条远端error5，
双QMUX各只打开一次，CommCenter到423.712秒未退出；WDS连接仍未开始。
原018日志同8B622DC1显示两卡网络注册、CellMonitor、Desense和手动
选网注册报MALFORMED。当前桥把原厂89字段表当成完整原生请求格式，
在QRTR发送前拒绝Apple扩展TLV；真实原厂字段表不能用作完整iOS格式。

来源仍是QcomBaseband/libqmi的原生事务与响应语义，以及本机原厂
NAS字段表。动手前再次fetch UniXNU14236be、myron-next f1a1bfe、
QcomBaseband dd0199c、Trogdor e55ae43；pandora/data-a-live/Pagani/
fairlady/cas/umi没有可直接替换的NAS多客户端指示桥。公开协议
常量与现有锁/线程复用；原生私有指令只在本机核验，不进Git或CI。

原生Mav20QMIDesenseCommandDriver::registerFrequencyReporting
实际构造TLV0xb0并写入模式2；原生网络注册还按能力条件构造0xd2、
0xe4、0xe7等单字节字段。原始writer与0x21两字节writer已在本机
执行并核对字节，SP保持；宿主模拟PAC返回不代替正式内核PAC资格。

019仅扩大`NbNasMux.h`的指示注册表到完整TLV-id范围：原厂字段仍
校验布尔值和0x21两字节格式，Apple扩展单字节的值完整保存，交给
真实modem接受或拒绝，不把2改成1，不丢弃扩展，不伪成功。原厂布尔
报告仍按“任一客户需要”合并；扩展非零模式相同时复用，客户撤销不
撤掉其他客户需要的模式；不同非零模式无法推定合并语义时返回
NOT_SUPPORTED并保留原状态。注册缓存只在真实成功应答后更新。

两卡NAS端口、原生CID/transaction、队列代际和取消寿命保持。
注册后跨订阅重绑定限制仍保留。UIM/WDS/PDP/MFSE/VOICE/WMS、
PAS/MSS/IPA、钉住文件读写范围和八条既有签名RAM修改未改变。
原生频率/完整网络注册报文回归在018实现上必失败，019通过；
完整宿主检查与旧17头改变C++编译预检通过。正式27原生、
严格封包与真机网络注册/数据仍待，不合主包。

## 30. 原生网络时间缓存字段（020，2026-11-14）

019实机已消除NAS容量错误并保持双QMUX各opens1、CommCenter零退出；
两卡网络注册仍收到MALFORMED，WDS上下文已INITIALIZED但未Start。
019同boot UUID的526704条原生日志与33210条基带记录完成625/12校正。
错误从同一to-host出口返回，缺失原始请求字节时不能仅据此断言全部来自
modem；019的结构校验仍会拒绝大于单字节的扩展。

本机原生网络注册构造器按运营商配置附带NetworkTimeCachePeriod
TLV0xc0。原getSize返回7（3字节TLV头加4字节值），原writer读取对象
0xc处的uint32并写全部四个小端字节。六种原始writer执行覆盖0、1、
30、300、3600和UINT32_MAX，原指令不变、SP保持；这不是实机PAC验收。
019的一字节扩展限制会在QRTR发送前拒绝此合法字段。

020只补该字段的准确长度和值：显式零仍保留，不能当布尔撤销；
同订阅客户的相同时间值可以共享，不同时间值没有已证合并语义，
返回NOT_SUPPORTED并保持此前真实基带状态。成功后的配置更新、
原错误响应、取消/端点寿命、两个订阅端口和其余单字节扩展保持。
原厂0x21两字节和全部布尔校验保持。未删未知字段、未伪造注册成功。

来源是现有QcomBaseband/libqmi事务层、本机原生编码器和019实机日志。
开工再次fetch UniXNU2eb3699、myron-next f1a1bfe、QcomBaseband dd0199c、
Trogdor e55ae43；pandora/data-a-live/Pagani/fairlady/cas/umi既有核查无
可替换蜂窝桥。GPU024固定框架不升级；本机私有输入和指令不进Git/CI。
真实modem的GetSupportedFields 0x1f协议已取得公开参考，尚未查询，
不把原厂IDL或宿主测试称为本机modem能力实证。UIM/WDS/PDP/MFSE/
VOICE/WMS、PAS/IPA、八签名RAM修改和钉住文件范围均沿019。
