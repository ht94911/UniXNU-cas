# CPU0 RAM 候选与试机流程

本地候选固定 XNU10063.141.1/4KiB、CPU0、新鲜私有熵、DarwinBoot、显式高RAM发布和
只读trace查看器，不含UFS、Data、触摸、GPU、ramdisk、trustcache或addon。构建通过
不代表手机执行通过。未经当次明确人类要求，不推送GitHub、不派发Actions。

默认菜单只读trace，没有超时自动启动。验证与XNU入口的命令行各自固定，其他参数拒绝。
候选tag绑定profile、执行模式、kernel SHA和私有ADT SHA；随机值不进源码或日志。

第一轮A只做验证与恢复：刷新USB/cas/解锁/恢复条件后RAM临时启动，进入诊断选择
Prepare high RAM ownership (512MiB)，返回UniXNU，选Validate CPU0 memory and load。
加载器重新枚举本次FDT/SMEM/GOP/UEFI，精确分配0x100004000起256MiB LoaderCode，
核对held描述符WB/非runtime/非RO/非RP及实时权限，完成全范围两遍互补地址模式、
缓存维护、读回、清理，再装载kernel/ADT/args，释放后在EBS前返回。旧诊断Probe的
旧起点结果不能放行这个偏移窗口。

返回后读trace：CRC/ABI/raw反码有效、Candidate MATCH、C/raw stage5、status0，
held PA100004000、bytes10000000，record=current boot。先收齐全部照片/日志并确认，
再要求回Android；实际回Android后记录新boot身份。全程不刷分区、不写内部存储。

第二轮B用同一SHA新RAM启动，先只读上一轮trace：Candidate MATCH，record与current
boot不同，CRC/raw pair/held pass完整。丢失、坏CRC、不同候选或同boot不放行，原记录
不会被拒绝流程清除。确认后重新Prepare高RAM，再选Boot XNU CPU0。本次仍重新枚举
并完整测试同一次held分配；旧记录只证明恢复可用，不授本次RAM所有权。

判据：C记录EL/SCTLR/TCR/SP/entry/args/owned范围；EBS重试只调用GetMemoryMap/EBS。
raw20=EBS后loader汇编，21=关MMU前，22=关MMU后/分支前。只有实际内核的40(__start)
或41(start_first_cpu)能证明XNU自有代码进入。绿色条仅属于loader；这些标记仍不证明
VM、平台初始化、用户态或iOS。

若内核停住，先保存观察，再沿已验收的长按Power恢复：持续到手机实际重启后松开。
若进Fastboot，先重新核对指定cas身份再reboot；确认Android启动后可另立恢复RAM启动
读取trace。仅有intent无result时先查实际状态，不自动重跑；新槽记录丢失时XNU结果未知，
不得补PASS。已验收cas-complete-001救援镜像保持独立身份。

复现：本地build_firmware.py --diagnostics构建FD，package_boot.py --diagnostics
--efi-ram-fat生成新base；cas_cpu0.py --adt-audit PRIVATE_AUDIT --mode execute构建
新CPU0 overlay；check_cpu0_host.py运行完整真实loader C和实际held probe核心，平台、
EBS与handoff明确为主机替身；package_cpu0.py创建私有32MiB FAT和独立解析的boot。
输出路径不可复用，保存私有权限、全部输入/源码/目标IR/map/SHA与恢复资料。构建工具
不操作手机，原始固件、带熵ADT和镜像不进入公开包。
