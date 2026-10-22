# Frozen build environment / 固定构建环境

These files are copied byte for byte from the accepted `cas-complete-001` delivery.
They contain no installation or device operations. Follow [the build guide](../docs/build.md)
or [中文构建说明](../docs/build.zh-CN.md). The Python file records the complete original
environment; some packages were incidental to that environment rather than required by firmware.

这两个文件与已验收交付保持逐字节一致。`cas-env.sh` 仅激活已存在的 venv 并设置工具路径，
不会安装软件或操作手机。Python 清单保留当时的完整环境，其中部分包并非固件构建必需。
