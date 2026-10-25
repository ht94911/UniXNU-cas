# XNU 主机集成（X-06）

独立 cas-x06-host-fixture 使用真实 X-02 未剥离 XNU 和 X-03 CPU0 ADT，构造
boot_args、装载后释放内存并返回。ExitBootServices 与 DarwinEnter 在编译时关闭。
32MiB FAT 保留 CasDiag、回 Android 和回固件入口，经现有 UniXNU driver 机制注入
固定 DarwinBoot v2；已验收16MiB配置和013固件不变。

本配置仅限主机：ADT fixture 没有新鲜随机熵；选择范围始于0x100004000，比 X-05
实测起点偏移16KiB，尾部未被旧试验覆盖。不能直接升级为手机候选。完整 ABI、
日志字段、阶段编号和新候选前置条件见 config/handoff-contract.json。

scripts/cas_handoff.py 从干净固定 d099354d0eab34df5088ca1ba89c9c88bc3260bb
生成构建 overlay，核对真实源码 SHA 并保存差异与许可，不修改共享上游 checkout。
scripts/check_handoff_host.py --driver-build BUILD --evidence-root EVIDENCE
--out NEW_OUTPUT 用主机 UEFI 服务替身运行完整加载器 C，验证段复制、BSS、ADT、
boot_args、释放、失败路径和日志 CRC。仅替换寄存器读取与主机服务；ARM64 EFI
另行编译，主机 C 通过不代表执行过目标 EFI 或手机成功。

256字节日志位于原固件保留的 NC 区，0..231字节 CRC 位于232；240/248的汇编
阶段/反码在 CRC 之外，需结合本次 boot stamp 单独核对。1..5是加载器阶段，绿色条
不能证明 XNU 已进入。可选 --entry-trace 只在真实 __start/start_first_cpu 添加
0x40/0x41，使用经核对的临时寄存器，且均在 XNU 开 MMU 前；保留10063/4KiB基线，
严格绑定输入与 patch SHA。尚不声称 MMU 后日志、CPU0 运行或 iOS 成功。

既有 macos-15/Xcode16.4 加密源码构建 workflow 接受显式 entry_trace 参数，无需
本机 Mac。每个新构建仍需保存 run/commit/artifact digest、解密内核及匹配符号。
参考 umi@3eb72578597d2ca16471b172d45eddc096a34a04 的寄存器/阶段分离设计，
不复制其 ramoops 地址和页表。新增代码遵循 BSD-2-Clause-Patent，Apple patch
保留 APSL 上下文。源码、构建、模拟服务和真实硬件证据分别记录。
