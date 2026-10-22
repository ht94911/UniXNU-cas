# Sources and licensing

[简体中文](sources.zh-CN.md) · [Project home](../README.md)

| Component | Origin and license |
| --- | --- |
| Original cas tools and board support | [UniXNU-cas LICENSE](../LICENSE), BSD-2-Clause-Patent |
| cas firmware base | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium), baseline `42fe563791e51bf5500c2e470ed294ca0c94b97c`; upstream license files retained |
| Shared UEFI foundations | Microsoft Project Mu / EDK II; per-directory licenses apply |
| UniXNU application and registration driver | [UniXNU](https://github.com/UniXNU/UniXNU), pinned commit in [source-pins.json](../source-pins.json), BSD-2-Clause-Patent |
| Brotli, LZMA, libfdt, Oniguruma, MIPI Sys-T and OpenSSL source dependencies | Their original notices remain in the pinned submodules; the release includes component notices under `licenses/` |
| Qualcomm/Kona and community reference work | Existing Mu-Silicium targets, [uefi-sheng](https://github.com/UniXNU/uefi-sheng), [uefi-pipa](https://github.com/xVanTuring/uefi-pipa); references are not cas hardware acceptance |

The firmware repository retains its upstream history and licenses. The cas additions carry their own source headers. The repository includes broader upstream code, including directories with their own GPL notices; do not apply the cas license to all upstream files. The small cas build selects source modules through its checked allowlist.

The public release preserves the original source bundles and all accompanying license notices. The `uefi-cas-overlay.bundle` requires the stated Mu-Silicium base object; `UniXNU-cas.bundle` contains the device history. Cloning the public repositories is the convenient source path; bundles provide an additional frozen recovery path.

The original stock DTB, DTBO, complete boot image, raw device logs and recovery backups are local inputs. Public records include input hashes and sanitized observations without redistributing those files. No Apple IPSW, kernelcache or system image is included.
