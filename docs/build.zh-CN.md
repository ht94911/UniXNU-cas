# 构建与可复现说明

[English](build.md) · [项目首页](../README.zh-CN.md) · [启动与恢复](operations.zh-CN.md)

本手册按准确源码提交重建 **`cas-complete-001`**。已验收的独立重建对比了 34 项产物，全部逐字节一致。该结果适用于记录中的主机与输入，新环境编译成功本身不等于已经验证真机兼容性。

## 环境要求

原记录使用 WSL Ubuntu、Python **3.12.3**、LLVM/LLD **18.1.3**、dtc **1.7.0**、zlib **1.3**，ARM64 主机回归使用 QEMU **8.2.2**。在 Linux 文件系统构建，三个源码目录保持相邻。已有源码 checkout 时，避免再维护第二个可写副本。

以下命令用于**新环境**。已有项目应保留当前改动，使用已登记的源码目录。示例输出编号如已存在，必须改用新编号；工具会拒绝覆盖历史输出。

安装所需系统包。软件源中的版本可能变化，请对照 [source-pins.json](../source-pins.json) 核实实际版本。

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends git build-essential \
  clang-18 llvm-18 lld-18 clang llvm lld nasm uuid-dev acpica-tools \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu python3-venv python3-pip \
  device-tree-compiler mtools dosfstools ninja-build cmake pkg-config \
  zlib1g-dev libssl-dev qemu-system-arm
```

## 获取准确源码

先克隆默认分支，把公开的环境支持文件保存到源码目录之外，再切换到原始真机验收提交。这样可以保持源码干净，满足来源校验。冻结标签保留当时源码状态，默认分支提供完整的公开文档。

```sh
set -euo pipefail
export CAS_WORK_ROOT="$HOME/cas-work"
mkdir -p "$CAS_WORK_ROOT"
git clone https://github.com/ht94911/UniXNU-cas.git "$CAS_WORK_ROOT/UniXNU-cas"
git clone https://github.com/ht94911/uefi-cas.git "$CAS_WORK_ROOT/uefi-cas"
git clone https://github.com/UniXNU/UniXNU.git "$CAS_WORK_ROOT/UniXNU"

# Keep publication support files outside the exact source checkouts.
export CAS_SUPPORT="$CAS_WORK_ROOT/support/cas-complete-001"
mkdir -p "$CAS_SUPPORT"
cp "$CAS_WORK_ROOT/UniXNU-cas/environment/cas-env.sh" "$CAS_SUPPORT/"
cp "$CAS_WORK_ROOT/UniXNU-cas/environment/python-packages.txt" "$CAS_SUPPORT/"

git -C "$CAS_WORK_ROOT/UniXNU-cas" switch --detach 346a404f6cee464e3e95f6ded464caf804a1f89e
git -C "$CAS_WORK_ROOT/uefi-cas" switch --detach 893f5025cea91a121f2566730779cb34955325fb
git -C "$CAS_WORK_ROOT/UniXNU" switch --detach 67e8ef0aa2a0a1fcb940889fb39ed1752b4b1593
```

只初始化本目标用到的依赖，gitlink 与[版本清单](../source-pins.json)固定了准确提交。不要使用 `submodule update --remote`、递归下载全部厂商二进制依赖，或直接运行面向其他机型的通用准备脚本。

```sh
git -C "$CAS_WORK_ROOT/uefi-cas" submodule update --init -- \
  Common/Mu Common/Mu_OEM_Sample Mu_Basecore \
  Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl

git -C "$CAS_WORK_ROOT/uefi-cas/Mu_Basecore" submodule update --init -- \
  BaseTools/Source/C/BrotliCompress/brotli \
  MdeModulePkg/Library/BrotliCustomDecompressLib/brotli \
  MdeModulePkg/Universal/RegularExpressionDxe/oniguruma \
  MdePkg/Library/BaseFdtLib/libfdt \
  MdePkg/Library/MipiSysTLib/mipisyst
```

创建项目 venv 并加载与原交付字节一致的环境文件。requirements 清单保留当时完整环境，也包含部分非固件必需的软件包，用于保存原始版本记录。

```sh
python3 -m venv "$CAS_WORK_ROOT/.venv"
source "$CAS_SUPPORT/cas-env.sh"
python3 -m pip install -r "$CAS_SUPPORT/python-packages.txt"
python3 --version
clang --version
dtc --version
qemu-system-aarch64 --version
python3 -c 'import zlib; print(zlib.ZLIB_RUNTIME_VERSION)'
```

环境文件只激活已有 venv，并把 LLVM 18 放到 `PATH` 前方，不会安装软件或操作手机。构建与打包脚本固定 `SOURCE_DATE_EPOCH=1792643741`，不要替换为当前时间。

## 离线检查

源码测试与目标预检不需要手机，也不需要私有原厂 DTB。已验收候选有 126 项源码测试通过。ARM64 入口回归需要 QEMU，缺少模拟器会报告失败，模拟器结果也不属于真机证据。

```sh
python3 -m unittest discover -s "$CAS_WORK_ROOT/UniXNU-cas/tests" -v
python3 "$CAS_WORK_ROOT/UniXNU-cas/scripts/preflight.py" \
  --uefi "$CAS_WORK_ROOT/uefi-cas" \
  --out "$CAS_WORK_ROOT/validation/cas-static-001.json"
```

## 构建固件

使用 cas 专用入口，它会调用固定 BaseTools、执行强制预检，并把日志和源码身份保存到新输出目录。

```sh
python3 "$CAS_WORK_ROOT/uefi-cas/Platforms/Xiaomi/casPkg/DeviceBuild.py" \
  --build --out "$CAS_WORK_ROOT/builds/cas-rebuild-001"
```

完整固件位于 `builds/cas-rebuild-001/Build/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`，大小恰为 8 MiB，EFI 模块在相应的 `AARCH64/` 目录。这部分可以在没有私有 DTB 的情况下构建。通用 UniXNU `build/bootimg` 不是本里程碑支持的构建入口。

## 打包完整候选

完整打包需要从测试手机原 boot 输入中选取的基础 DTB，大小 **536,890 字节**，SHA-256：

```text
0eae9bf6a42b38ce8490f100ff24a681f50a67da53b8dd366ef7e6e119a59963
```

请从自己有权使用的本地备份或匹配固件来源准备该输入，公开项目不分发它。文件名相同不足以证明输入匹配，打包器会核对实际字节，再调整 `totalsize` 并追加已记录的 8 KiB 扩展区。实测运行时还依赖 DTBO SHA-256 `5dbd3622d80151384164bf614e060456c1d2bf06d8bb26896a4676270231fca9`；系统升级或另一台手机可能改变这些条件。

先完成完整固件构建，三个源码工作区必须干净，且自构建后没有改变。将下方私有 DTB 路径替换为实际位置。

```sh
CAS_STOCK_DTB=/absolute/path/to/private/boot-02.dtb
sha256sum "$CAS_STOCK_DTB"

python3 "$CAS_WORK_ROOT/UniXNU-cas/scripts/package_boot.py" \
  --uefi "$CAS_WORK_ROOT/uefi-cas" \
  --firmware-build "$CAS_WORK_ROOT/builds/cas-rebuild-001" \
  --environment "$CAS_SUPPORT/cas-env.sh" \
  --stock-dtb "$CAS_STOCK_DTB" --efi-ram-fat \
  --out "$CAS_WORK_ROOT/artifacts/cas-rebuild-001"
sha256sum "$CAS_WORK_ROOT/artifacts/cas-rebuild-001/boot.img"
```

预期输出大小 **17,616,896 字节**，SHA-256：

```text
a085d19b6a4a1ea429f2f10cb191050f316cf044d4fd864b53d398e4f6a4f67c
```

16 MiB RAM FAT 包含菜单配置和源码构建的 CasSmoke。打包目录保存命令日志、源码／子模块／工具清单、输入哈希、镜像检查、FD、EFI、map 及拆包结果。这些本地产物可能包含私有路径和 DTB 数据，分享前需要审阅。

## 核对可复现结果

[重建对比记录](evidence/rebuild-comparison.json)列出全部 34 项产物及哈希。实际验收在独立的完整构建和打包输出目录完成，没有复制旧镜像充作重建。最终 EFI 已移除 CodeView/PDB 输出路径记录，gzip 时间为零，FAT 布局和时间戳固定。

哈希不同时，先保留两份输出，依次比较源码、子模块、工具、FD、EFI、FAT、shim 和私有 DTB，不要把新文件附会到旧候选的验收结果。输入变化时使用新候选编号，并完成相应验证。

## 发布文件与源码 bundle

[公开发布页](https://github.com/ht94911/UniXNU-cas/releases/tag/v0.1.0)提供冻结的可分享 ZIP 和校验文件。原始源码 bundle 提供另一条恢复路径：先导入 Mu-Silicium 基础提交，再导入固件 overlay bundle；设备 bundle 包含全部设备历史，使用时保留所附许可证。

公开 ZIP 不包含完整启动镜像、原始 DTB／DTBO、设备身份或恢复备份。详见[来源与许可证](sources.zh-CN.md)及[验收记录](validation.zh-CN.md)。
