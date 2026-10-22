# 来源与许可证

[English](sources.md) · [项目首页](../README.zh-CN.md)

| 组件 | 来源与许可 |
| --- | --- |
| cas 原创工具和板级支持 | [UniXNU-cas LICENSE](../LICENSE)，BSD-2-Clause-Patent |
| cas 固件基础 | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)，基线 `42fe563791e51bf5500c2e470ed294ca0c94b97c`，保留原许可证文件 |
| 通用 UEFI 基础 | Microsoft Project Mu / EDK II，适用各目录许可证 |
| UniXNU 应用与注册驱动 | [UniXNU](https://github.com/UniXNU/UniXNU)，固定提交见 [source-pins.json](../source-pins.json)，BSD-2-Clause-Patent |
| Brotli、LZMA、libfdt、Oniguruma、MIPI Sys-T 和 OpenSSL 源码依赖 | 固定子模块保留原始许可，发布包在 `licenses/` 中附组件声明 |
| Qualcomm／Kona 及社区参考 | 现有 Mu-Silicium 目标、[uefi-sheng](https://github.com/UniXNU/uefi-sheng)、[uefi-pipa](https://github.com/xVanTuring/uefi-pipa)；参考项目不作为 cas 真机验收 |

固件仓库保留上游历史和许可证，cas 新增文件保留各自源码头。仓库包含更多上游代码，部分目录有自己的 GPL 声明，不能把 cas 许可证套用于所有上游文件。精简 cas 构建通过受校验的允许清单选择源码模块。

公开发布保留原始源码 bundle 和配套许可。`uefi-cas-overlay.bundle` 需要已记录的 Mu-Silicium 基础对象；`UniXNU-cas.bundle` 包含设备仓库历史。直接克隆公开仓库更方便，bundle 另提供一条冻结源码恢复路径。

原始 DTB、DTBO、完整启动镜像、原始设备日志和恢复备份是本地输入。公开记录提供输入哈希和脱敏观察，不分发这些文件，也不包含 Apple IPSW、kernelcache 或系统镜像。
