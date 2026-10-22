# Build and reproducibility

[简体中文](build.zh-CN.md) · [Project home](../README.md) · [Boot and recovery](operations.md)

This guide rebuilds **`cas-complete-001`** from its exact source commits. The accepted independent rebuild compared 34 artifacts byte for byte. That result applies to the recorded host and inputs; a successful new build alone does not establish hardware compatibility.

## Requirements

The recorded environment was WSL Ubuntu with Python **3.12.3**, LLVM/LLD **18.1.3**, dtc **1.7.0**, zlib **1.3** and QEMU **8.2.2** for the ARM64 host regressions. Build on the Linux filesystem, with three sibling source directories. Avoid a second writable copy of an existing source checkout.

The following commands are for a **new environment**. Existing projects should preserve their current changes and use their registered checkouts. Output identifiers in examples must be changed if they already exist; the tools refuse to overwrite previous outputs.

Install the required system packages. Repository package versions can change, so check actual versions against [source-pins.json](../source-pins.json).

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends git build-essential \
  clang-18 llvm-18 lld-18 clang llvm lld nasm uuid-dev acpica-tools \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu python3-venv python3-pip \
  device-tree-compiler mtools dosfstools ninja-build cmake pkg-config \
  zlib1g-dev libssl-dev qemu-system-arm
```

## Obtain the exact sources

Clone the default branches, save the published environment files outside the source directories, then select the historical tested commits. Keeping support files outside those checkouts allows the source-provenance checks to remain clean. The frozen tags contain the original source state; the default branches contain the completed public documentation.

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

Only initialize the dependencies used by this target. The gitlinks and [source pins](../source-pins.json) define their exact revisions. Do not use `submodule update --remote`, a full recursive vendor-binary fetch, or generic setup scripts for other devices.

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

Create the project virtual environment and load the unchanged environment support file. The requirements file records the full original environment, including incidental packages, to preserve the original version inventory.

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

The environment file activates the existing venv and places LLVM 18 tools first on `PATH`. It performs no installation or device operation. The build and packaging scripts set `SOURCE_DATE_EPOCH=1792643741`; do not substitute wall-clock time.

## Offline checks

The source tests and target preflight need no phone and no private stock DTB. The accepted candidate had 126 passing source tests. ARM64 entry regressions require QEMU; absence of the emulator is a failure, not hardware evidence.

```sh
python3 -m unittest discover -s "$CAS_WORK_ROOT/UniXNU-cas/tests" -v
python3 "$CAS_WORK_ROOT/UniXNU-cas/scripts/preflight.py" \
  --uefi "$CAS_WORK_ROOT/uefi-cas" \
  --out "$CAS_WORK_ROOT/validation/cas-static-001.json"
```

## Build the firmware

Use the cas-specific entry point. It invokes the pinned BaseTools, runs mandatory checks and saves logs plus source identities in the new output directory.

```sh
python3 "$CAS_WORK_ROOT/uefi-cas/Platforms/Xiaomi/casPkg/DeviceBuild.py" \
  --build --out "$CAS_WORK_ROOT/builds/cas-rebuild-001"
```

The full firmware volume is at `builds/cas-rebuild-001/Build/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` and is exactly 8 MiB. EFI modules are under the corresponding `AARCH64/` directory. These outputs can be built without the private DTB. The generic UniXNU `build/bootimg` route is not the supported entry point for this milestone.

## Package the complete candidate

Packaging needs the original base DTB selected from the tested phone's stock boot input. It is **536,890 bytes** with SHA-256:

```text
0eae9bf6a42b38ce8490f100ff24a681f50a67da53b8dd366ef7e6e119a59963
```

Provide that input from your own authorized local backup or matching firmware source. This public project does not distribute it. A DTB with the same filename is not enough: the packer checks the actual bytes. It adjusts `totalsize` and adds the recorded 8 KiB expansion area. The tested runtime also used DTBO SHA-256 `5dbd3622d80151384164bf614e060456c1d2bf06d8bb26896a4676270231fca9`. A system update or another device may change these assumptions.

The complete build must succeed first, and all three source checkouts must be clean and unchanged since that build. Replace the private DTB path below with its real location.

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

Expected output: **17,616,896 bytes**, SHA-256:

```text
a085d19b6a4a1ea429f2f10cb191050f316cf044d4fd864b53d398e4f6a4f67c
```

The 16 MiB RAM FAT contains the menu configuration and the source-built CasSmoke application. Packaging saves the command logs, source/submodule/tool manifest, input hashes, image checks, FD, EFI modules, maps and unpacked parts. Those local outputs can include private paths and DTB data; review them before sharing.

## Check reproducibility

The [recorded comparison](evidence/rebuild-comparison.json) lists all 34 artifacts and hashes. The observed comparison used an independent complete build and packaging output directory, not a copy of the previous image. Final EFI CodeView/PDB path records are removed, gzip time is zero, and FAT layout/timestamps are fixed.

If hashes differ, retain both outputs and compare source commits, submodules, tool versions, FD, EFI, FAT, shim and private DTB before changing anything. Do not relabel the result with the tested candidate's evidence. When the inputs change, create a new candidate and run applicable verification.

## Releases and source bundles

The [public release](https://github.com/ht94911/UniXNU-cas/releases/tag/v0.1.0) includes the frozen shareable ZIP and checksum file. Its original source bundles offer an alternative restoration path: import the Mu-Silicium baseline before the firmware overlay bundle; the device bundle contains its complete history. Preserve the included licenses.

The public ZIP deliberately contains no full boot image, original DTB/DTBO, device identity or recovery backup. See [sources and licensing](sources.md) and [validation](validation.md).
