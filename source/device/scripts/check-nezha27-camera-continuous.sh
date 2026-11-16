#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
if (( $# != 1 )); then
    printf 'Usage: %s OUTPUT_DIRECTORY\n' "$0" >&2
    exit 2
fi
device=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
cd "$device"
shared=${NEZHA_CAMERA_SHARED_SOC:?Set the selected SM8850 owner source path}
mkdir -p "$out/fixture/IOKit" "$out/fixture/machine" "$out/fixture/sys" "$out/target-fixture"
printf 'extern "C" int printf(const char *,...);\n' > "$out/fixture/sys/systm.h"
printf '#include <stddef.h>\nextern "C" void *memcpy(void *,const void *,size_t);\n' > "$out/target-fixture/string.h"
printf 'typedef unsigned long vm_offset_t;\ntypedef unsigned long vm_size_t;\ntypedef int IOReturn;\n#define kIOReturnSuccess 0\nextern "C" void IOSleep(unsigned);\nextern "C" void IODelay(unsigned);\n' > "$out/fixture/IOKit/IOLib.h"
printf 'typedef unsigned long vm_offset_t;\ntypedef unsigned long vm_size_t;\n' > "$out/fixture/machine/machine_routines.h"
python3 - "$out" <<'PYHOST'
from pathlib import Path
import sys
text=Path('package/camera/NezhaCameraNativeCapture.cpp').read_text()
for old,new in [
    ('__asm__ volatile("dmb oshld" ::: "memory");','__atomic_thread_fence(__ATOMIC_ACQUIRE);'),
    ('__asm__ volatile("dsb oshst" ::: "memory");','__atomic_thread_fence(__ATOMIC_RELEASE);')]:
    assert text.count(old)==1
    text=text.replace(old,new)
# All state, control callbacks and cleanup are the actual production source;
# only architecture barriers use the host counterpart, no physical device.
(Path(sys.argv[1])/'NativeCapture-host.cpp').write_text(text)
PYHOST
flags=(-std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined
       -fno-omit-frame-pointer -Ipackage/camera -Ipackage/gpu/adreno
       -I"$shared/include" -I"$out/fixture")
g++ "${flags[@]}" "$out/NativeCapture-host.cpp" tests/camera-native-capture.cpp -o "$out/test-capture"
"$out/test-capture"
"$out/test-capture" controls
"$out/test-capture" controls-mismatch
"$out/test-capture" AF-readonly
"$out/test-capture" AF-eeprom-fail
"$out/test-capture" AF-position-fail
g++ "${flags[@]}" package/camera/NezhaCameraNativeSmmu.cpp tests/camera-native-smmu.cpp -o "$out/test-smmu"
"$out/test-smmu"
for unit in NativeCapture NativeSmmu; do
    "${CAMERA_TARGET_CC:-clang}" -target arm64e-apple-ios27.0 -ffreestanding \
        -fno-stack-protector -mgeneral-regs-only -std=c++17 -Wall -Wextra -Werror \
        -Ipackage/camera -Ipackage/gpu/adreno -I"$shared/include" -I"$out/fixture" -I"$out/target-fixture" \
        -c "package/camera/NezhaCamera$unit.cpp" -o "$out/$unit.arm64e.o"
done
gcc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Ipackage/camera -Ipackage/gpu/adreno \
    -Itests/camera-platform/kernel-fixture package/camera/NezhaCameraNativeDevice.c \
    package/camera/NezhaCameraOvx10500u.c package/camera/NezhaCameraCci.c \
    tests/camera-native-device.c -pthread -o "$out/test-device"
"$out/test-device"
"$out/test-device" AF-readonly
gcc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Ipackage/camera \
    package/camera/NezhaCameraOvx10500u.c tests/camera-ovx-readback.c -o "$out/test-ovx-readback"
"$out/test-ovx-readback"
