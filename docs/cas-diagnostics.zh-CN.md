# cas RAM 内存与日志诊断

[English](cas-diagnostics.md)

独立 `cas-diag-009` profile 构建只在 RAM 中运行的 EFI 诊断程序，用于收集当前内存依据、拒绝未取得所有权的高地址访问，并检查结构化 RAM 日志的恢复。程序不加载 XNU。默认 `cas.dsc` / `cas-complete-001` 路线保留原行为。

## 构建和打包

使用登记的相邻源码目录和固定环境。候选构建前提交改动；打包器要求源码干净且与成功构建时的身份一致。每个输出目录都必须全新。

```sh
source environment/cas-env.sh
python3 -m unittest discover -s tests -v
python3 scripts/build_firmware.py --uefi ../uefi-cas --diagnostics \
  --out "$HOME/cas-work/builds/cas-diag-009"
python3 scripts/package_boot.py --uefi ../uefi-cas \
  --firmware-build "$HOME/cas-work/builds/cas-diag-009" \
  --environment environment/cas-env.sh --stock-dtb /path/to/private/boot-02.dtb \
  --efi-ram-fat --diagnostics --out "$HOME/cas-work/artifacts/cas-diag-009"
```

原 stock DTB 的固定身份、RAM 临时启动与恢复流程见[构建说明](build.zh-CN.md)和[操作说明](operations.zh-CN.md)。打包器绑定诊断固件、shim 和 FAT，拒绝默认/诊断 profile 混用。FAT 只允许一个 EFI 路径 `\EFI\Tools\CasDiag.efi`，另有普通回 Android 和返回固件动作；没有 donor、内核或根卷。脚本不调用 ADB/Fastboot。

诊断 DSC/FDF 使用原有受限 heap 和不含物理存储驱动的选择，新增源码 CasDiag，设置 `CAS_XNU_DIAGNOSTICS=1`，保留日志尾槽并限制普通串口日志范围。它不把高地址 RAM 导入 GCD 或 UEFI 分配器。

## 菜单与当前依据

音量键选择，短按电源执行，长按电源返回。日志分有限页显示，短按下一页，长按退出。只有选中明确的菜单项才请求 warm reset。

| 项目 | 动作 |
| --- | --- |
| 枚举 | 读取本轮有界 FDT 副本、RAM/保留区、当前 UEFI 描述符、GOP，以及有界原始 SMEM RAM 表；结构化记录写入固件自有 RAM 槽。 |
| 测试高地址 256 MiB | 重新枚举并满足所有门限后，才请求精确 UEFI 分配；拒绝时不访问高地址。 |
| 读日志 | 显示上一记录的校验状态、构建、序号、flags、boot stamp，并分页显示本轮/历史文本。 |
| Warm reset | 提交 reset-request 标记后调用 `ResetSystem(EfiResetWarm)`；后续检查标记是否还在，不预设保存成功。 |
| 返回 | 提交退出标记，返回 UniXNU。 |

枚举不修改 FDT、SMEM、存储、变量或分配器拓扑；快照使用普通有界 pool，诊断文本只写自有 RAM 日志。诊断专用 memory-map 库只将既有 SMEM 保留区 `[0x80900000, 0x80b00000)` 映射为 NC reserved，SEC 在进入 DXE 前设置 RO/XP；默认生成表保持原内容，分配器仍为 128 MiB。任何 SMEM 读取前，程序要求**本次** FDT `qcom,smem` 的 `memory-region` phandle 恰好指向该 `no-map` 保留区、当前 UEFI reserved/UC 描述符覆盖该区，且 Memory Attribute Protocol 实际确认 RO/XP、无 RP。不获取硬件锁、不写 SMEM。未解析的 FDT 保留区阻止探测；边界完整的动态保留区排除整个允许分配区间。

`SmemCore.c` 只读分配元数据和 item 402。支持 SMEM heap 11、12（12 仅限 **uncached global partition**），RAM 表 1/2/3 版分别采用 24 字节 header 后的 64/72/72 字节条目。辅助区、仅 cached 项、未知版本、重复/重叠、magic/count/padding 错误及溢出全部拒绝。所有读取限制在 2 MiB 内，每次最多 4096 字节、每轮最多 1200 次读取与 64 KiB 轨迹、最多 32 行 RAM 表。两轮观察的有序 offset、length、原始字节必须完全相同；这能发现观察到的数据变化，不声称原子快照，也不替代 Qualcomm 远程锁协议。不读、不记录其他 item 的 payload。

线格式核对来源为 [Linux v4.19 SMEM](https://github.com/torvalds/linux/blob/v4.19/drivers/soc/qcom/smem.c)、[固定 LK RAM 表定义](https://github.com/msm8916-mainline/lk2nd/blob/5e6f1adcb1c070e28a70dc1f3d807ce9538f61c5/platform/msm_shared/smem.h)、[Qualcomm v3 RAM 表定义](https://www.mail-archive.com/u-boot%40lists.denx.de/msg570801.html)。有界解码器独立编写，没有引入这些驱动代码。

每行数字字段均输出；仅明确 read/write、APPS/shared domain 的 SDRAM/system-memory 行参与高地址门限，其他行及不可用尾部作为排除区。未知或默认 attr/domain 只能成为观察，不能成为所有权。

FDT 只能来自已验证的 cas 固定地址和大小，RAM/保留区必须使用支持的双 cell 宽度，每个区间均检查。输出包含 FDT CRC 和完整 UEFI 描述符的类型、物理/虚拟地址、长度、属性。当前 GOP 用于排除 framebuffer；历史 RAM bank 本身不能证明所有权。

## 探测契约

固定测试候选为 `[0x100000000, 0x110000000)`，256 MiB，位于较大的历史 512 MiB 调查区。它必须 2 MiB 对齐，并完整位于一条当前 FDT bank、一条当前 RAM-partition entry 和一段当前 UEFI conventional WB 区间内。当前 FDT 保留区、不合格 SMEM 行/尾部和 framebuffer 全部排除；低 4 GiB 的整体排除还覆盖固件、栈、输入走廊、控制与日志区。算术、数量、重叠、缺失依据均失败即拒绝。

当前固件只发布原有 **128 MiB 低地址 allocator**，因此本 profile 预期会**拒绝高地址测试**。拒绝是诊断结果，不是高地址通过。程序不会用 `AddMemorySpace`、历史 bank 导入或直接读写绕过门限。X-05 应先收集真实输出，再决定后续 allocator 候选如何调整。

门限通过后才执行共享 C 事务：

1. `AllocatePages(AllocateAddress, EfiLoaderData)` 必须成功且返回精确请求地址。
2. 对整段分配写入随完整物理地址变化的 64 位 pattern。
3. clean/invalidate cache、fence 后逐 word 读回；再以反码重复。
4. 清零整段、再次 clean/invalidate 和 fence，然后释放 pages。
5. 只有读回、清理和释放全部成功才通过。

异常返回地址只释放，不读写。普通 write/cache/read 错误返回统一进入清理和释放，主机 callback 测试覆盖这些路径；它们不证明 CPU exception、reset 或 bus fault 后仍能清理，仍需真机观察。

## 保留日志格式

诊断专用槽位于既有固件自有 log 区的尾 64 KiB：`[0x9f970000, 0x9f980000)`，不新增物理 RAM 所有权。诊断 shim 只清低 `0x30000` 字节，并将新的 `CNTVCT_EL0` boot stamp 放入 boot-info `Reserved[0]`；普通串口日志避开尾槽。默认 shim 仍清空原日志全区。

记录为小端，header **96 字节**：

| 偏移 | 字段 |
| ---: | --- |
| 0 | 八字节 `CASLOG01` magic，最后提交 |
| 8、12 | `u32` version=1、header length=96 |
| 16、20 | `u32` 总长度、ASCII payload 长度 |
| 24、32 | `u64` sequence、boot stamp |
| 40、44 | `u32` flags、CRC32 |
| 48 | NUL 结尾、补零的 32 字节 build ID |
| 80 | 16 字节保留零 |

CRC32 覆盖 header 和 payload，计算时 CRC 字段视为零。写入先使 magic 无效，再写有界 body/checksum、fence，最后提交 magic。读取器拒绝残缺、非法长度/版本/flags、错误 padding/CRC、非 ASCII 文本；其他 build ID 单独报告。stamp 相同表示同次启动复用，不同只表示**前次启动候选记录**，不能自动判定持久化通过。ABL、安全固件或 reset 都可能清除或覆盖该区。

EFI viewer 是机上的恢复读取器。已保存到本地的 slot 还可离线解码：

```sh
python3 scripts/read-cas-diag-log.py /path/to/saved-slot.bin \
  --build cas-diag-009 --current-boot 0x1234
```

该命令只读文件，不会取得物理内存 dump。诊断格式尚未接入 XNU panic/logging；该接口属于 X-06。

## 验证边界

可移植 `DiagCore.c` 就是 EFI 使用的范围门限、日志 codec 和分配/清理事务。24 项主机测试实际编译并调用该 C 代码，与独立 Python reader 交叉核对记录、破坏/截断记录并注入 callback 失败。SMEM 测试实际编译并调用 `SmemCore.c`，使用合成内存注入元数据/内容变化和读取失败，覆盖坏表、未知版本、越界、cached/辅助区拒绝和读取预算。独立 C harness 编译真实诊断映射 wrapper，核对只改变 SMEM HOB 选项、重复调用稳定、类型错误拒绝，且 conventional heap 仍为 128 MiB。各次实际测试数量和构建结果以候选报告为准。

完整固件/镜像构建报告另行标识实际产物。编译和主机测试不能证明当前 cas 内存所有权、高地址 cache 行为、实体按键、warm reset 保留或 XNU/iOS 启动；这些必须来自新镜像哈希绑定的 X-05 真机观察，不能复用旧 UEFI 候选的成功结果。

## 003 早期屏显日志

002 的一次真机临时启动仅显示阶段1～7，未到菜单；长按电源后已回Android。
阶段7是SEC解压后标记，阶段8实际位于CasBdsInitialize，具体停滞原因未知。
003仅为诊断profile选择SerialDiagLib：原始RAM日志保持完整大小写，另外将最近
16行、每行56字符绘到继承framebuffer，屏显字母转大写。文本位于原控制页
log+0xc00，编译时检查与显示、按键、raw log不重叠；不分配新物理区。
绘制复用有边界Blt和继承refresh，保留底部阶段/故障条，在阶段8后停止。
003的SMEM映射和128MiB allocator沿用002；新增屏显本身尚须真机验证。

## 004 异常关键行保留

003照片确认屏显工作，CPU异常处理器末尾断言可见，但ESR/ELR/FAR被堆栈滚出。
004仅为屏显增加行首 `Stack dump:` 的跨写入识别，在该标记处锁存现有画面；
原始RAM日志继续完整接收堆栈和断言。状态改为version2/936字节，仍位于原空隙。
不改变异常处理、SMEM权限或allocator；真实异常原因仍待004屏幕证据。

## 诊断005的采集改动

动态 reserved-memory 节点按当前 `size`、可选的二次幂 `alignment` 和完整
`alloc-ranges` 校验，并排除每个允许分配的整个区间，不猜测实际放置地址。
边界缺失或非法仍使 FDT 不完整。SMEM provider、UEFI 覆盖及真实 RO/XP 协议
结果分别输出；所有检查同意后才允许有界只读 SMEM。allocator 与映射/权限要求不变。

日志查看器查询实际控制台尺寸，显式拆分每一物理行，页头显示字节位置。
短按 Power 下一页，音量上回上一页，长按 Power 返回诊断菜单。
这些只改变呈现，原始 RAM 日志字节保持不变。

## 诊断006的权限准备

005真机最后页为mask13/15：provider与UEFI reserved/UC通过，但实际权限只有XP。
固定DXE NX策略会替换原访问位，不能假定SEC设置的RO一直保留；尚无逐阶段真机trace。
006在完整当前FDT/UEFI和provider验证后，先查询精确SMEM 2MiB区间权限。
仅当查询成功且无RP/未知位时，才通过Memory Attribute Protocol补加RO|XP；
不清除任何权限、不新增映射、不导入GCD、不改SMEM内容或allocator拓扑。
调用前保存意图日志，成功后重新查询；已满足权限的路径不调用setter但也重新查询。
读回必须恰为RO|XP，之后才允许原有有界SMEM读取；错误、缺协议或不同权限全部拒绝。

主机测试提取并编译EFI实际使用的权限函数，注入协议/查询/设置/读回故障，
验证调用顺序、精确地址与长度、幂等和RP拒绝。这不是硬件页表或手机运行证据。
下一次手机只枚举，先确认006编号和权限before/verified、SMEM结果及最终mask。
128MiB allocator未变；即使枚举完整，也不意味着256MiB owned-memory通过。

## 诊断007的格式拒绝定位

006真机确认SMEM权限从0x4000补加到0x24000，三个预读条件均通过，随后
返回CdSmemFormat（2）、heap12、item尚未定位，最终mask13/15；006经菜单回Android。
007保留006的全部接受条件和读取顺序，输出稳定检查编号/名称、pass、读取次数、
轨迹字节及最后读取的offset/length。只复制已经读取的至多48字节元数据；
不增加物理读取，不复制RAM item payload/名称。callback失败时样本清零。
主机将真实007 C与冻结006 C进行有效/坏格式输入的返回码、Info和callback序列对比。

重点检查编号：11 primary-header（initialized/reserved）、20 directory-header
（magic/version/reserved）、22 directory-entry（重复global/flags/reserved）、
25 partition-header（magic/hosts/size/reserved）、28 item-header（canary/reserved）。
元数据按小端32位word显示，+xx是相对last-read offset的字节偏移。
编号和原始字段用于定位失败；未知布局仍然拒绝，不能据遥测跳过门禁。

## 诊断008的v12目录兼容

007真机check22的48字节原始目录项为offset0x1000、size0x146000、flags3、
hosts0xfffe/0xfffe、cacheline32，reserved七word均0。v4.19 Linux定义目录flags
为unused且global定位不以flags为条件；008只记录它，不把它当RAM行的访问权限。
007的Part>=8400沿用了v11的512-entry allocation table；008的v12布局保留首4KiB、
末4KiB目录，要求分区8字节对齐、完整落在中间范围。v11仍保护8400字节旧表。

唯一global、零reserved、正确partition header/hosts/size、uncached范围、item头、
读预算、双次一致观察与RAM表本身的属性/所有权检查继续执行。
输出SMEM global offset/size/flags/cacheline，失败继续显示007的具体拒绝点和metadata。
合成测试只复用照片已知的目录项；未观察的partition/item内容明确为synthetic。
参考：[Linux v4.19 SMEM](https://github.com/torvalds/linux/blob/v4.19/drivers/soc/qcom/smem.c)。

## 诊断009的首PC行保留

008真机照片出现阶段7后的CPU异常，ELR0x9bb09500/ESR0x02000000；随后长按Power
已回Android。照片缺少模块加载基址，静态模块映射不能唯一符号化这个物理地址。
009把屏显锁存提前至第一条行首PC 0x...完整换行后，保留其前面的装载/异常上下文。
没有PC行时，原Stack dump锁存仍作为fallback。原始串口RAM日志继续记录全部寄存器
和堆栈；只改变镜像屏显，不改异常处理器、SMEM、权限或内存布局。
屏显状态为version3/944字节，仍位于log+0xc00到+0x1000的既有空隙。

主机测试覆盖前缀/完整行的跨write拆分、换行前不锁存、CRLF/折行、嵌入PC字样拒绝、
坏状态恢复，以及屏显锁存后原RAM日志继续完整接收。009用于补齐未知异常的定位证据；
不能把该呈现改动称为CPU异常已修复。若菜单到达，继续原来的仅枚举；若再异常，拍全
保留的首PC行和其前装载信息后恢复Android。

## 诊断010的部分RAM数字行

009真机已到heap12/RAM表3，item offset0x7ab0/2328字节，在check44重叠拒绝，
尚未看到冲突行。010记录已解码前缀长度、两个从0开始的冲突索引；失败时只输出
各已解码行的base/size/available及attribute/category/domain/type，明确标NO OWNERSHIP。
不打印名称、opaque item payload或未解析行；前缀最多32行，包括被拒绝的当前行。
每次观察先重置前缀/索引sentinel，第二次观察在metadata失败时不会把第一次行当作当前行。

不改变任何接受条件、物理读取顺序、权限门禁、内存映射或allocator。真实C测试对比009
返回码/Info/callback，并覆盖前缀/数量边界、冲突数字、work复用及双次观察失败。
看到重叠不等于证明可以去掉门禁；先依据实际行类型和格式来源解释，再独立证明owned-memory。

## 诊断011的首PC与数字异常摘要

010在CasDiag前失败：实测ArmCpuDxe load0x9bb05000/entry0x9bb061cc和PC0x9bb07500
对应实际RVA0x2500，属于UpdateRegionMappingRecursive。静态指令为ldr x0,[sp,#0x20]；
未取得本次运行时opcode/ESR/FAR/SP。009与010的ArmCpuDxe字节相同，不能据此认定
新增partial行代码出错或MMU helper已确认为致因。

011保留原16行加载/首PC屏显。锁存后只解析固定串口handler的SP/ELR/SPSR/FPSR与
ESR/FAR精确源行；每值最多16个hex、仅记录一次。原屏上方另用四行显示已知字段，
缺失字段显示问号并标PARTIAL；绝不解引用打印的地址。原RAM日志完整保留，IRQ/重入
保护及stage8停止屏显不变。字段齐全前若出现新的异常banner，标NESTED/UNBOUND并拒绝
后续值，避免将嵌套异常的ESR与首PC错配。version4状态1024字节，仍在log+0xc00..+0x1000空隙。
这是补证，不是CPU/MMU修复；SMEM门禁、权限和allocator不变。
