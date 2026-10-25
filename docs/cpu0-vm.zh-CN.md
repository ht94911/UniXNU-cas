# CPU0 VM 观测候选004

003 已在本机执行到实际 XNU `start_first_cpu`，并在回 Android 后恢复其身份绑定日志。
004 新内核由已授权的 macos-15/Xcode16.4 工作流40217312445构建，SHA-256
`b5e428922606300c5f81b49884146f2a74939995d29bc148d6319c5a1d73a83e`。
它增加 MMU、arm_vm_init 和早期 PE 的观察点，以及 CPU0 终止异常记录；尚未做004真机试验。

启动只需 Prepare high RAM ownership → Boot XNU CPU0。独立 Validate 仍可选，004 不再以
上一启动的 Validate 记录作为门禁。每次 Boot 内的实时 FDT/SMEM/UEFI、affinity0、精确256MiB
分配、两组完整内存写读/缓存/清零、kernel/ADT/args 检查均保留，完成后才允许 EBS。

日志原256字节保持ABI，后接128字节异常扩展；记录 ESR/FAR/ELR/SPSR/TTBR/SP、向量和前一阶段。
boot、候选tag、双读稳定性、XOR/反码都正确才接受异常记录。查看器只读；新loader请求清除旧扩展。
EL1 SP0 异常保存被中断的 SP_EL0。正式页表切换到重新 IO 映射日志之间有已声明的记录空隙；
只有阶段56不能证明故障位置。尚无panic字符串捕获。

53 表示 arm_vm_init 返回且日志 IO 映射可写；54/55只是早期 PE(TRUE)/诊断停止。
它们不等于 IOKit ArmDTPlatform::configure，不能据此单独完成 X09 平台验收。
诊断停点在时钟/IOKit/machine_startup/存储之前，不启动 UFS、Data、SMP 或用户态。

未变的003固件、shim、DTB和菜单按实际输入哈希复用，并生成复用收据，不伪称重新编译。
新内核/EFI/ADT/FAT和boot镜像另存新编号。GitHub仅用于本次已授权Mac源码构建，不发布手机镜像。
