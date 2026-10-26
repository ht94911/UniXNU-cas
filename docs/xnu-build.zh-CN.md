> 当前活动构建入口已迁移到[格式2流程](build-format2.zh-CN.md)和标准有序series；以下旧source-only命令仅保留历史可复现说明。

# XNU 构建执行端准备

[English](xnu-build.md)

首个目标继续固定为 **iPad7,1／j120ap／iPadOS 17.7.11／21H461**，
源码采用 **XNU 10063.141.1／Darwin 23.6.0／ARM64／4 KiB**，与固定的
sheng、umi 参考路线一致。umi 记载的 donor 二进制内核另为
**10063.142.1.703.8**；后续预链接必须核对精确文件哈希和接口。
**Xcode 16.4／iPhoneOS SDK 18.5 是编译环境版本，目标系统仍是 17.7.11。**

来源：[sheng](https://unixnu.com/devices/sheng)、
[umi 构建说明](https://github.com/reAAAq/UniXNU-umi/blob/2afd98a065d60d1c067e29b4be4611962a7db8d7/docs/BUILD.md)、
[框架源码版本锁](https://github.com/UniXNU/UniXNU/blob/67e8ef0aa2a0a1fcb940889fb39ed1752b4b1593/feeds.toml)。

## 当前工作流

`xnu-preflight.yml` 使用 ARM64 的 `macos-15` 标准 runner，显式选择
**Xcode 16.4／16F6、iPhoneOS SDK 18.5、macOS SDK 15.5**。
它保存实际 runner 镜像、系统、Python、编译器及工具哈希、SDK build ID
和所选头文件身份，编译一个 ARM64／iOS 17 目标文件，再核对 Mach-O
架构和最低部署版本。runner 镜像和 Homebrew 包会变化，每轮以实际记录为准。
这是执行端资格检查，**不构建或启动 XNU**。

首轮实际运行 [39974022970](https://github.com/ht94911/UniXNU-cas/actions/runs/39974022970)
已成功编译并取回测试目标文件，解密后的文件哈希与运行身份一致。本地和实际
macOS 各十项测试通过。后续预检另记录 ditto、plutil、Perl 等构建工具，并通过
`xcrun --find` 区分系统 make/unifdef 调度入口与所选 Xcode 的实际执行文件。

工作流可手工触发，也会响应 `codex/xnu-executor-001` 分支的指定文件推送。
不需要手机、IPSW、crypto 源码或私有固件；只申请仓库读取权限，不保留 Git
凭据，产物保存七天。历史固件标签、镜像和相应真机验收仍保持原身份。

## 取回产物

公开仓库的 GitHub Actions 产物可被其他读者下载。成功预检会使用提交的
**公钥证书**，加密报告、测试源码和目标文件；私钥只保存在所有者本机。
私钥和解密后的内容不得进入 Git。

从精确登记的运行下载产物后，先核对 GitHub artifact digest 和密文哈希，再
解密到一个新目录：

```sh
python3 -B ci/xnu/artifacts.py unseal \
  --bundle /absolute/path/to/downloaded-artifact \
  --key /absolute/path/to/private/recipient-key.pem \
  --out /absolute/path/to/new/preflight-output
```

工具先核验整个归档的文件集合和逐文件明文哈希，再写出文件；拒绝绝对路径、
目录穿越、链接、重复成员、超大归档和已经存在的输出目录。CMS 加密只提供
保密性，不能认证构建者，仍须把下载绑定到 GitHub 运行、仓库和提交。
以后用这条通道取回内核及匹配符号前，先备份本机私钥。

传输工具不授予 Apple 输入的使用或再分发权限。后续源码内核构建须满足所选
组件的输入契约，并显式列出内核、匹配符号、配置、补丁和清单。
预检工作流只验证传输；下述独立源码工作流负责保留实际内核与符号。

## 最小源码内核候选

独立的 `xnu-source.yml` 和 `ci/xnu/build_source.py` 实现首次源码构建通道。
首轮实际 macOS 构建在 bootstrap 阶段发现继承的可选 GPU addon 补丁引用了
未定义的 `BINDM_FORCE_ROOT`。输入锁 `ci/xnu/inputs.json` 固定框架、4 个选中
模块、6 个源码输入、55 个基线内核补丁、两组补充头文件提交和两项候选适配。
最小配置先验证全部 55 份原始补丁哈希，再明确排除整个 GPU addon 覆盖补丁及
依赖它的 staged-app 补丁及未启用的 progress 补丁，有效补丁为 52 份。`patch-selection.json`
保存基线、排除原因与实际顺序。WSL 准备步骤实际将 52 份补丁应用到 XNU，
并在 `patched-source.json` 记录 57 份改变的源码身份。
下述两次实际清洁构建现已验证源码内核链接。
输入来自固定的公开源码及受限的 2023 crypto 镜像；当前范围为内部正确性和
集成验证，原件及衍生产物不作为公开发布包。

每次创建新的 runner 临时 stage，源引用与实际修改的源码分开。先验证补丁原始
顺序与哈希，再应用 ArmCpu Darwin 汇编修正和可选显示引用保护，恢复并逐文件
校验补充头文件。bootstrap 只用于生成编译记录；之后实际构建 crypto、pthread、
ArmCpu 与 ArmDTPlatform，再执行最终内核链接。它不调用预链接和 IPSW 流程。

编译器审计会按实际 `arm_init` 编译记录执行预处理，核对 `ARM_PGSHIFT` 解析为 12，
且非 Apple 分支将两项运行时页移位变量赋为 12。arm64 的 `PAGE_SHIFT_CONST` 是
运行变量。`page-size.json` 保存实际命令、赋值与哈希；运行时页大小仍待验证。

成功输出保留未剥离的 ARM64 内核、从该内核直接生成的符号列表、配置、输入
清单、补丁前后哈希和完整日志，并加密取回。符号记录绑定内核 SHA；内核自带
真实符号表。这不保证已有完整源码行号调试信息，运行与 crypto 自检仍待后续验证。
两次实际清洁构建现已完成比较。

`--prepare-only` 可以在 WSL 验证准备步骤；它不会编译或生成内核。`--platform qemu`
另选 ArmVirtPlatform，仅用于单独命名的虚拟平台候选；cas 配置使用 ArmDTPlatform。

## 实际源码构建结果

[39986603109 / attempt 1](https://github.com/ht94911/UniXNU-cas/actions/runs/39986603109/attempts/1)
和 [attempt 2](https://github.com/ht94911/UniXNU-cas/actions/runs/39986603109/attempts/2)
在两个独立干净的 hosted VM 上构建提交 `0ce99faf43ef0c8facee11073425da8962c1569f`。
两次均生成 21,392,120 字节的普通 ARM64 内核，实际 `arm_init` 编译审计选中 4 KiB，
两项候选适配通过目标编译。未剥离内核各含 279,022 项符号表记录，另存匹配的
71,178 行非调试符号列表；两组加密产物均已取回并在本地核对。

代码、非调试符号、配置、输入、改变源码清单和实际工具身份一致。整文件 SHA
不同：1,928 个差异字节全部来自构建日期字符串、Mach-O UUID 与 N_OSO 对象时间值。
此结论为清洁构建比较通过，不声称整文件逐字节一致。两组代码段的 POST HMAC
独立复算一致；运行时 crypto 自检、源码行号调试、QEMU XNU、DarwinBoot 接线和
cas 真机启动仍待验证。本阶段未生成新的 Android boot.img。

## 本地检查

```sh
python3 -B -m unittest discover -s tests -p test_xnu_ci.py -v
python3 -B ci/xnu/preflight.py --profile ci/xnu/toolchain.json \
  --out /absolute/path/to/new/probe
```

第二条命令在 Linux／WSL 会如实失败并保存实际缺项。本地测试实际执行
OpenSSL 加解密，验证错误归档拒绝行为；不能计为 macOS 或 XNU 编译通过。
