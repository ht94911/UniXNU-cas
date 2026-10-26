# 当前格式2构建流程

上游文档与代码基线：UniXNU@ae687009348296e97e49ce6e925795044f363ce6。
设备接线和内存布局在本仓库，SM8250公共硬件在独立common仓库；通用逻辑在框架与组件。
保留iOS17、单核连续启动、SMP后置。旧EFI候选见原build.md，不能当作新候选已验收。

```sh
./unixnu defconfig cas_continuous_defconfig -D ../UniXNU-cas
./unixnu fetch -D ../UniXNU-cas
./unixnu status --json -D ../UniXNU-cas
./unixnu tree -D ../UniXNU-cas
./unixnu build kernel -D ../UniXNU-cas -j4
```

The CI source manifest pins the reviewed framework fixes and canonical component
revisions; `feeds.toml`/`unixnu.lock` pin SoC and source references. Development
overrides belong in ignored `local.toml` or explicit `--local-module soc:sm8250=PATH`.
Output is `out/cas/ipados-17.7.11-21H461/cas_continuous/`. Kernel build includes prelink
against the hash-checked matching kernelcache. The standard series order is arch,
SoC, OS, then device. Do not replay the historical late-patch wrapper mutations.

`build-cas-armcpu-overlay.py` stages declared diagnostics only in generated output,
then the canonical ArmCpu recipe builds the real driver; the source module remains
untouched. Multi-bank ADT is explicitly enabled, preserves holes/reservations, and
does not grant ownership. Actual UEFI allocation/held-probe checks remain required.

The GitHub job uses macos-26/Xcode26.6, records the actual SDK/compiler and builds
the continuous kernel plus own signed iOS17 user fixture. Only encrypted owner
results are uploaded; private keys, IPSW/DTB/firmware and raw device data stay out
of Git. A successful host build is not a device, root or interface result.

Firmware and boot image assembly must retain the cas BootShim, correct EDK2 ABI,
loader ownership guard and bounded input audit. The generic packer cannot replace
the checked cas packer by simply changing a command. Never run `userdata` for this
RAM-only target. See [status](porting-status.md) and `porting.toml` for actual limits.

## 最小RAM根卷

上游mockfs使用自有签名静态Mach-O作为内存设备，在`/sbin/launchd`提供该程序。
通过`build-native-user-fixture.py`构建后，将程序作为`--data-ramdisk`输入，
对应bundle作为`--trust-cache`输入，候选参数增加`rd=md0`。设备loader只接受
显式契约绑定的内核、ADT、根卷和trust-cache哈希。该程序用于最小系统调用验证，
不代表Apple launchd或iOS界面。
