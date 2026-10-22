# Boot, controls and recovery

[简体中文](operations.zh-CN.md) · [Project home](../README.md)

This runbook describes the tested **Xiaomi 10 Ultra (cas / M2007J1SC)** combination: Android 15, OS3.0.4.0.VMKCNXM, kernel 4.19.157-moon, and the pinned stock DTB/DTBO. It does not establish compatibility with another phone or a firmware update.

## Before a temporary boot

Use the locally reconstructed `cas-complete-001` image, **17,616,896 bytes**, SHA-256:

```text
a085d19b6a4a1ea429f2f10cb191050f316cf044d4fd864b53d398e4f6a4f67c
```

Have a USB connection, an operator who can see the phone and use its buttons, and known recovery inputs. Save a new trial identifier, start time, image hash, current device/mode and exit plan. If a previous boot attempt has no result, inspect the phone before dispatching another command.

Commands below show ADB/Fastboot syntax. Replace `USB_SERIAL` and `ABSOLUTE_BOOT_IMG` with the device and file just verified. Use the current serial explicitly for every operation. On a WSL build host, USB ADB/Fastboot may run from Windows while build tools run in Linux.

```text
adb devices -l
adb -s USB_SERIAL shell getprop ro.product.device
adb -s USB_SERIAL shell getprop ro.product.model
adb -s USB_SERIAL shell getprop sys.boot_completed
```

Confirm `cas`, `M2007J1SC` and completed Android startup. Then enter bootloader mode and re-enumerate:

```text
adb -s USB_SERIAL reboot bootloader
fastboot devices
fastboot -s USB_SERIAL getvar product
fastboot -s USB_SERIAL getvar unlocked
fastboot -s USB_SERIAL getvar max-download-size
```

Continue only with the confirmed cas, a bootloader currently reporting `unlocked: yes`, and a download limit larger than the image. Android root access or green/orange text is not a substitute for the current Fastboot state. Unlocking, partition flashing and repartitioning are outside this runbook.

After checking the local image's size and SHA-256, dispatch exactly once:

```text
fastboot -s USB_SERIAL boot ABSOLUTE_BOOT_IMG
```

Save the host response and observe the phone. `Sending/Booting OKAY` only confirms the command response; it does not prove that the menu ran. The tested procedure uses a RAM temporary boot and RAM FAT.

## Menu and physical controls

The menu title is **UniXNU - Xiaomi 10 Ultra**. There is no automatic execution countdown.

| Control | Behavior |
| --- | --- |
| Short volume up/down press and release | Move one entry; holding repeats after a delay |
| Short power press and release | Confirm on release |
| Hold power for about one second, then release | Escape; the main menu ignores it and release does not also confirm |
| Keep holding power until a hardware restart | Recovery, distinct from the one-second menu action |

| Entry | Action |
| --- | --- |
| Run CasSmoke EFI test | Load `EFI/Tools/CasSmoke.efi` from read-only RAM FAT |
| Check missing EFI handling | Try absent `EFI/Tools/Missing.efi` and return an error |
| Reboot to Android | Restart into the existing Android installation |
| Return to firmware information | Exit UniXNU to the firmware menu, with information and reboot options |

CasSmoke displays `CAS EFI SMOKE - build cas-smoke-001` and `StartImage reached the test application.` Short-press power to return, then check that UniXNU is navigable and its bottom status line reports `Success`. The source also prints `CAS_SMOKE_RETURN_SUCCESS` before returning; a transient line should only be recorded as observed if the operator actually saw it.

The missing-file test redraws the menu directly. Look at the **bottom status line**, near the control hints, for `'Check missing EFI handling' returned: Not Found`. This version has no separate error-dismissal screen.

CasSmoke's source implements a longer-power Escape exit with `EFI_ABORTED`; that is not a separate accepted hardware case in the three-run milestone.

## Exit and recover

For a normal exit, select **Reboot to Android** in UniXNU or the firmware menu. Observe Android, then verify the device identity and `sys.boot_completed=1` again using USB ADB. Record the actual exit route.

For a black screen or unresponsive menu, retain the last visible state and host response. If the device re-enumerates and is confirmed to be cas in Fastboot, `fastboot -s USB_SERIAL reboot` can restore Android. Otherwise use the tested continuous power hold until the phone restarts (approximately 10–15 seconds as a guide; observe the actual restart). Reconfirm the resulting mode. Do not automatically repeat `boot` when the outcome is unknown.

If the original Android installation no longer starts, stop additional trials and inspect the device state using the owner's recovery information. Persistent recovery writes are outside this temporary-boot procedure. Partition backups do not substitute for a backup of personal data.

## Evidence to retain

Save distinct observations for menu, keys, EFI marker, return to the menu and Android return, with times and image SHA-256. A five-minute stability window must be continuously observed; an interruption is not observed runtime. Record human observations separately from host/emulator logs. Read [the milestone acceptance](validation.md) before making wider claims.
