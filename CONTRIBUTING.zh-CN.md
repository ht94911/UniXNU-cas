# 参与贡献

[English](CONTRIBUTING.md) · [项目首页](README.zh-CN.md)

修改前先阅读[架构说明](docs/architecture.zh-CN.md)、[构建手册](docs/build.zh-CN.md)和[验收边界](docs/validation.zh-CN.md)。

## 选择代码位置

设备配置、内核配置、板级脚本、设备驱动、补丁和公开文档放在 `UniXNU-cas`；固件启动、cas 固件驱动、BootShim 和 CasSmoke 放在 [uefi-cas](https://github.com/ht94911/uefi-cas)；通用修改可以向 [UniXNU 上游](https://github.com/UniXNU/UniXNU)提交。

## 提交修改

1. 使用范围清晰的分支，保留已有工作，不修改已验收的 `cas-complete-001` 标签。
2. 说明触发条件、修改后的行为和受影响候选，并同步英文及简体中文文档。
3. 运行构建手册中适用的离线检查，如实记录跳过或不可用的检查。
4. 新输出使用新编号。源码检查、编译成功、QEMU 与真机观察属于不同证据等级。
5. 候选发生变化时重新完成相关真机验证，旧镜像的成功不会自动适用于新字节。

提交标题可使用 `docs:`、`fix(cas):`、`feat(cas):` 或 `build(cas):`。PR 说明应描述最终修改及实际验证。

## 报告问题

提供代号／型号、相关系统固件版本、源码提交、镜像 SHA-256、具体菜单操作、实际结果和恢复情况，并说明证据来自现场观察、主机命令、模拟器还是串口日志。失败试验同样有价值，应保留独立编号。

公开日志前移除序列号、IMEI、MAC 地址、账户信息、IP 地址及令牌。不要上传原厂固件、从手机提取的 DTB、GPT 原始数据、恢复备份、专有系统镜像或完整设备绑定启动镜像。可通过[问题表单](https://github.com/ht94911/UniXNU-cas/issues/new/choose)提交。

## 许可证

原创贡献遵循 [BSD-2-Clause-Patent](LICENSE)。复用代码保留已有版权与许可证，准确标注参考来源。
