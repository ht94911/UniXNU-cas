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

## 本地检查

```sh
python3 -B -m unittest discover -s tests -p test_xnu_ci.py -v
python3 -B ci/xnu/preflight.py --profile ci/xnu/toolchain.json \
  --out /absolute/path/to/new/probe
```

第二条命令在 Linux／WSL 会如实失败并保存实际缺项。本地测试实际执行
OpenSSL 加解密，验证错误归档拒绝行为；不能计为 macOS 或 XNU 编译通过。
