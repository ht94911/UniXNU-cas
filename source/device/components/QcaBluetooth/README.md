# QcaBluetooth

Qualcomm WCN7850 Bluetooth driver for XNU. It runs the Linux `btqca.c` startup
sequence over a GENI UART and connects the controller to Apple's `bluetoothd`.

## Status

Device: Xiaomi Pad 6S Pro (sheng), iPadOS 27.0.1 (24A446).

The table below is the upstream author's sheng record. The consuming nezha
Bluetooth27 adaptation has separately reached a real UART controller-version
reply on 2026-11-09, followed by a baud-change timeout in002 and003. Nezha pairing and audio
are still unverified; see the device's Bluetooth27 handoff. Its board selects
the actual SM8850 DFS clock bank when enabled and uses a bounded Ganges reply
observation. The003/004 hardware had DFS disabled. The004 trial verified
RTS-only and ordinary RCG bit20 programming but still received no data after
FC48. The actual N0 HAL bypasses FC48 for the observed Ganges tuple after a
PERI/TME bootstrap. This source rejects that incorrect high-rate path and
prepares a distinct current115200 diagnostic, with60s initialization and65s
caller budgets. Full PERI/TME and high-rate hardware support remain pending.

The005 current115200 trial completed the entire ELF transfer and received its
final ACK, then stopped at board-selected NVM with board_valid=false. It returned
normally and passed the phone protection audit. The next shim correction follows
the pinned Linux BID layout: skip edl metadata, take the two observed board-ID
bytes, and retain the real HCI status and complete response checks. Metadata is
not another status. Existing controller models cover metadata0/2 and short replies;
target compilation and a new hardware BID/NVM/HCI check remain required.

| State | Features |
| --- | --- |
| Working | Pairing, reconnection after reboot, power toggle, A2DP playback, AirPods noise control and battery, use alongside Wi-Fi |
| Not tested | Calls and microphone (HFP/SCO), sleep and wake |
| Unavailable | Features that need Apple vendor commands |

## Layout

| Path | Contents |
| --- | --- |
| `include/qbt.h`, `src/qbt_core.c` | Linux initialization, H4 framing and IBS on one executor |
| `include/qbt_geni.h`, `src/qbt_geni.c` | GENI SE DMA: four 2048-byte RX buffers, one 4096-byte TX buffer |
| `include/qbt_peri.h`, `src/qbt_peri.c` | N0 Ganges PERI/TME commands, mixed H4/IBS receive and activation proof |
| `include/qbt_firmware.h`, `src/qbt_firmware.c` | Full ELF/NVM validation and caller-owned PERI NVM baud preparation |
| `include/qbt_transport.h`, `src/qbt_transport.c` | UART.SK kernel pipe, backpressure, channel lifetime and worker power requests |
| `src/qbt_linux.c`, `shim/` | Boundary to the locked Linux sources |
| `userland/BlueTool.c` | `com.apple.BlueTool` XPC bootstrap service |
| `userland/Type0.c` | Capabilities from HCI reports for the native stack |
| `tools/` | Native daemon patch, signing, source lock and checks |

The board supplies WCN power, clocks, pins, IRQs, mapped DMA addresses and
firmware. Pairing, key storage, BLE and audio profiles run in `bluetoothd`.
[Native routing](native-ios27.md) describes the iPadOS 27 adapter.

## Kernel interface

| Sysctl | Access | Meaning |
| --- | --- | --- |
| `kern.qbt_boot` | Read | Controller state |
| `kern.qbt_power_on` | Read | Reading starts the controller and returns the result |
| `kern.qbt_firmware` | Read | Loaded firmware name |

`kern.qbt_power_on` is a read because BlueTool's sandbox denies sysctl writes.

## Checks

```sh
python3 tools/test-runtime.py
python3 tools/test-geni.py
python3 tools/test-peri.py
python3 tools/test-firmware.py
python3 tools/build-bluetool.py
python3 tools/build-type0.py
python3 tools/check-kernel.py /path/to/tree --record <kernel source compile record> \
    --extra-source <board source> --board-export <symbol>
```

`tools/prepare-linux.py` verifies the source hash and stages byte-identical Linux
code in `out/linux`. `QBT_LINUX_TREE` selects an existing Linux checkout.

## Sources and licenses

Linux initialization and GENI/H4/IBS code use GPL-2.0-only. `src/qbt_transport.c`
uses APSL-2.0, and `userland/Type0.c` uses BSD-3-Clause. Public interfaces and
BlueTool use BSD-2-Clause-Patent. Each file identifies its license;
[NOTICE](NOTICE.md) and `tools/source-lock.json` record attribution.

Vendor firmware and Apple binaries are external inputs and stay out of this
repository.

The nezha PERI helper follows the actual Ganges2.0 HAL command/reply layouts.
The board retains the existing UART executor and shared power owner, supplies
an RX producer mark at TX admission, and performs a genuine H4 version query
after the activation completion/notice. Pending/unread DMA input is included
in the mark; a reply queued before the write callback returns remains valid.
PERI notifications continue to be demultiplexed during normal HCI operation.
The source tests use synthetic controllers; live DMA/FIFO semantics and the
complete PERI/TME3.2Mbps chain still require the fixed nezha candidate.

For PERI NVM, `qbt_firmware_prepare_peri_nvm()` modifies only tag17 payload
byte1 in an already SHA-verified, caller-owned temporary copy. The selected
3.2Mbps value is0x11. Complete type2/type4 validation precedes mutation;
the original file/digest, other tags and non-type2 sets are retained.
