> 当前活动构建入口已迁移到[格式2流程](build-format2.zh-CN.md)和标准有序series；以下旧source-only命令仅保留历史可复现说明。

# 单核连续启动到 iOS 17 用户态

按2026-10-26人类指令，保留XNU10063／原生4KiB／iPadOS17.7.11，先用CPU0推进用户态和界面，SMP后置。005已完成VM和早期平台检查，不再重跑。

`ci/xnu/build_source.py --continuous-boot`保留已通过的入口与内存路径，在`machine_startup`前恢复XNU正常异常向量，继续运行调度器、IOKit、根卷选择及init装载。成功点只写保留RAM日志，不主动停机或重启；无效内存/平台输入仍由原门禁阻止。

ArmCpu观察器复用UniXNU-umi3eb72578597d2ca16471b172d45eddc096a34a04的真实PPI27 handler、`rtclock_intr`返回与`ICC_EOIR1_EL1`位置，不改变GIC和timer行为。记录第1、2、8次及之后每64次IRQ。独立packet绑定boot、候选tag、sequence、payload XOR和magic反码，位于现有4KiB mailbox映射内。首个panic另存caller、reason、异常寄存器快照及最多111字节原始格式串。

`--user-fixture`同时构建XNU mockfs、运行时trust-cache支持和框架自有签名ARM64/iOS17静态程序，预期调用getpid与HW_PAGESIZE。这是最小用户态工具，不是Apple launchd或iOS界面。进手机前仍须新的有界RAM payload／ADT／loader契约，冻结CPU0载荷会拒绝这些新增输入。

IRQ位于512..832，启动位于832..960，panic位于1024..1280；原256B头、早期异常与平台记录偏移不变。需要对应的扩展读取镜像，旧512B reader不能验收新增packet。

已验证：7个XNU文件精确补丁应用/反向还原、已适配ArmCpu补丁、实际packet核心ASan/UBSan和ARM64/iOS17写入器/观察器编译。完整XNU链接、自有程序签名、扩展载荷和真机尚未执行。本地代码不构成GitHub推送或Actions派发授权。
