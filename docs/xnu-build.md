# XNU executor preparation

[简体中文](xnu-build.zh-CN.md)

The first target remains **iPad7,1 / j120ap / iPadOS 17.7.11 / 21H461**,
with source **XNU 10063.141.1 / Darwin 23.6.0 / ARM64 / 4 KiB**. This follows
the pinned sheng and umi references. umi reports a distinct donor binary kernel,
10063.142.1.703.8. Preserve that distinction and verify its exact hash and
interfaces before prelinking. Xcode 16.4 and the iPhoneOS 18.5 SDK describe
the compiler environment; they do not change the target system to iOS 18.

References: [sheng](https://unixnu.com/devices/sheng),
[umi build notes](https://github.com/reAAAq/UniXNU-umi/blob/2afd98a065d60d1c067e29b4be4611962a7db8d7/docs/BUILD.md),
[framework source pins](https://github.com/UniXNU/UniXNU/blob/67e8ef0aa2a0a1fcb940889fb39ed1752b4b1593/feeds.toml).

## Current workflow

`xnu-preflight.yml` runs on the ARM64 `macos-15` standard runner and selects
Xcode **16.4 / 16F6**, iPhoneOS SDK **18.5**, macOS SDK **15.5** explicitly.
It records actual image, OS, Python, compiler/tool hashes, SDK build IDs and
selected headers, compiles one ARM64 iOS 17 object, and inspects its Mach-O
header and deployment target. Runner images and Homebrew packages can move;
the resulting attestation identifies the tools actually used on that run.
This is executor qualification. It does **not** build or boot XNU.

The first real run, [39974022970](https://github.com/ht94911/UniXNU-cas/actions/runs/39974022970),
compiled and returned the test object successfully. Decrypted file hashes and run identity
matched. All ten tests passed locally and on the actual macOS runner. Further preflights
also record ditto, plutil, Perl and other build utilities, and use `xcrun --find` to distinguish
the system make/unifdef dispatch shims from the selected Xcode executables.

The workflow accepts manual dispatch and pushes to `codex/xnu-executor-001`.
It uses no phone, IPSW, crypto source or private firmware. It requests only
repository read permission, does not persist Git credentials, and retains its
artifacts for seven days. The historical firmware tags and images retain their
existing identity and hardware validation.

## Artifact retrieval

GitHub public-repository artifacts can be downloaded by other readers.
The successful probe encrypts its report, test source and object with the
committed **public** recipient certificate. The private key stays on the
owner's local machine. No private key or decrypted bundle belongs in Git.

After downloading the artifact from the exact recorded run, verify its GitHub
artifact digest and envelope hash, then decrypt to a new local directory:

```sh
python3 -B ci/xnu/artifacts.py unseal \
  --bundle /absolute/path/to/downloaded-artifact \
  --key /absolute/path/to/private/recipient-key.pem \
  --out /absolute/path/to/new/preflight-output
```

The tool verifies each plaintext hash and the archive member set before
extracting. Absolute paths, traversal, links, duplicates, oversized archives
and existing output directories are rejected. CMS encryption provides privacy;
it does not authenticate a producer. Bind the artifact to its GitHub run,
repository and commit separately. Back up the local key before depending on
this path for later kernel and matching symbol bundles.

This transport does not grant permission to use or redistribute Apple inputs.
A later source-kernel build must satisfy the selected component input contracts
and list its kernel, matching symbols, configuration, patches and manifests
explicitly. This workflow does not implement that later build.

## Minimal source-kernel candidate

The separate `xnu-source.yml` and `ci/xnu/build_source.py` implement the first
source-build route. The first actual macOS build stopped in bootstrap with an
undefined `BINDM_FORCE_ROOT` from the inherited optional GPU addon patch. The input lock
pins the framework, four selected modules, six source inputs, 55 baseline kernel
patches, two supplemental-header commits and two candidate adaptations. The minimal
profile verifies all 55 original hashes and explicitly excludes the whole GPU addon
overlay patch, its dependent staged-app patch and the unused progress hooks, leaving
52 effective patches. `patch-selection.json` records the
baseline, disabled features and effective order. WSL preparation actually applies all 52
patches to XNU and records the 57 changed source files in `patched-source.json`.
A successful kernel link remains unverified.
The selected 2023 crypto mirror is a restricted internal correctness/integration
verification input; its source and derived outputs are excluded from public releases.

Each run creates a new temporary stage. Source references remain separate from
patched files. The builder verifies original patch order and hashes, applies the
ArmCpu Darwin assembly and optional-display fixes, and restores and hashes every
supplemental header. Bootstrap creates compile records only. Real crypto, pthread,
ArmCpu and ArmDTPlatform archives precede the final kernel link. No prelink or
IPSW path is called.

The compiler audit replays the real startup compile record in preprocessing mode
and verifies `ARM_PGSHIFT` and `PAGE_SHIFT_CONST` resolve to 12. Its `page-size.json`
records the actual command and macro hashes; runtime page size remains untested.

Successful exports retain the unstripped ARM64 kernel, symbols generated directly
from that kernel, configuration, input manifests, adaptation hashes and complete logs,
encrypted for retrieval. Symbol metadata binds the kernel SHA and the real symbol
table stays in the kernel. Full source-line debugging is not guaranteed; runtime and
crypto selftests remain unverified. A second clean build follows the first successful link.

`--prepare-only` can check preparation on WSL without compiling a kernel.
`--platform qemu` selects ArmVirtPlatform for a separately identified virtual candidate;
the cas profile uses ArmDTPlatform.

## Local checks

```sh
python3 -B -m unittest discover -s tests -p test_xnu_ci.py -v
python3 -B ci/xnu/preflight.py --profile ci/xnu/toolchain.json \
  --out /absolute/path/to/new/probe
```

The second command deliberately fails on Linux/WSL and preserves the observed
missing inputs. Local tests exercise actual OpenSSL roundtrips and malformed
archive rejection. They do not count as a macOS or XNU build result.
