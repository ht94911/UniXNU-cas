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
当前工作流尚未实现这一步。

## 最小源码内核候选

独立的 `xnu-source.yml` 和 `ci/xnu/build_source.py` 实现首次源码构建通道。
首轮实际 macOS 构建在 bootstrap 阶段发现继承的可选 GPU addon 补丁引用了
未定义的 `BINDM_FORCE_ROOT`。输入锁 `ci/xnu/inputs.json` 固定框架、4 个选中
模块、6 个源码输入、55 个基线内核补丁、两组补充头文件提交和两项候选适配。
最小配置先验证全部 55 份原始补丁哈希，再明确排除整个 GPU addon 覆盖补丁，
有效补丁为 54 份。`patch-selection.json` 保存基线、排除原因与实际顺序。
内核最终链接仍待实际构建验证。
输入来自固定的公开源码及受限的 2023 crypto 镜像；当前范围为内部正确性和
集成验证，原件及衍生产物不作为公开发布包。

每次创建新的 runner 临时 stage，源引用与实际修改的源码分开。先验证补丁原始
顺序与哈希，再应用 ArmCpu Darwin 汇编修正和可选显示引用保护，恢复并逐文件
校验补充头文件。bootstrap 只用于生成编译记录；之后实际构建 crypto、pthread、
ArmCpu 与 ArmDTPlatform，再执行最终内核链接。它不调用预链接和 IPSW 流程。

成功输出保留未剥离的 ARM64 内核、从该内核直接生成的符号列表、配置、输入
清单、补丁前后哈希和完整日志，并加密取回。符号记录绑定内核 SHA；内核自带
真实符号表。这不保证已有完整源码行号调试信息，运行与 crypto 自检仍待后续验证。
首次链接成功之后，再从第二个干净输出比较构建结果。

`--prepare-only` 可以在 WSL 验证准备步骤；它不会编译或生成内核。`--platform qemu`
另选 ArmVirtPlatform，仅用于单独命名的虚拟平台候选；cas 配置使用 ArmDTPlatform。

## 本地检查

```sh
python3 -B -m unittest discover -s tests -p test_xnu_ci.py -v
python3 -B ci/xnu/preflight.py --profile ci/xnu/toolchain.json \
  --out /absolute/path/to/new/probe
```

第二条命令在 Linux／WSL 会如实失败并保存实际缺项。本地测试实际执行
OpenSSL 加解密，验证错误归档拒绝行为；不能计为 macOS 或 XNU 编译通过。
