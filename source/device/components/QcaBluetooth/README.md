# QcaBluetooth

Qualcomm WCN7850 Bluetooth driver for XNU. It runs the Linux `btqca.c` startup
sequence over a GENI UART and connects the controller to Apple's `bluetoothd`.

## Status

Device: Xiaomi Pad 6S Pro (sheng), iPadOS 27.0.1 (24A446).

The table below is the upstream author's sheng record. The consuming nezha
Bluetooth27 adaptation passed PERI/TME startup,3.2Mbps negotiation, independent
board0 NVM queries and controller-ready gates in Bluetooth007/Native009. The
original24A446 bluetoothd connected to UART.SK; counters grew over366 seconds
without transport errors. Normal XNU shutdown, Android/root protection and
collector exit passed. The human opened Bluetooth Settings, where Devices kept
spinning with no entries. Nezha discovery, pairing and audio remain unverified;
see the device's Bluetooth27 handoff.

The earlier003/004 FC48 trials and005/006 current115200 diagnostics remain
historical evidence. The actual N0 HAL uses PERI/TME before ordinary BT startup.
The source retains the genuine BID layout, exact hash-locked zero-board NVM,
successful HCI Reset and strict address readback requirements. Full profile,
sleep/wake and Wi-Fi coexistence acceptance still require device evidence.

## Runtime discovery observations

The existing H4 TX boundary parser records metadata only after UART consumption
completes a command; its preview has no side effects. The existing RX event path
observes CC/CS, Inquiry, Remote Name and LE reports while service/state2 are
active. `qbt_stats.hci` stores counts and24 bounded records. Records contain only
sequence, monotonic time, opcode, event kind/code, status and whitelisted flags
or announcement counts. Addresses, names, keys and other payload are excluded.
Missing status is `ff` and does not increment errors. Announcement totals
include repeated reports and are not unique-device counts.

The consuming device's cached provider10/v2 uses the existing2KiB USB limit:
snapshot retains its original state32 schema and adds20 compact counters plus
the last8 records; read-only QUD_HISTORY pages at most16 records from the same
cache. Core ASan/UBSan cases cover short writes, split/merged frames, RX pressure,
preview isolation, privacy and exact passthrough. The new metadata source still
requires Native010 target qualification and actual device observations.

运行期发现诊断复用原H4边界解析和事件入口，只记录实际发完的命令、回执和发现
公告摘要；preview不产生记录，透传、流控、线程和等待行为保持原样。24条缓存
经既有provider10只读分页，2KiB上限不变。公告含重复，不能当唯一设备数量；
新摘要的宿主检查与Native010目标/真机资格分别记录。

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
