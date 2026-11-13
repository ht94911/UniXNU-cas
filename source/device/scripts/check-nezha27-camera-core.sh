#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

if (( $# != 1 )); then
    printf 'Usage: %s OUTPUT_DIRECTORY\n' "$0" >&2
    exit 2
fi

mkdir -p "$1"
out=$(cd "$1" && pwd)
device=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$device"

host_cc=${CAMERA_HOST_CC:-gcc}
host_cxx=${CAMERA_HOST_CXX:-g++}
camera=package/camera
smmu=package/gpu/adreno
host_flags=(-std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
            -fno-omit-frame-pointer -I"$camera" -I"$smmu")

check() {
    local test=$1
    shift
    "$host_cc" "${host_flags[@]}" "$@" "tests/camera-$test.c" -o "$out/test-camera-$test"
    "$out/test-camera-$test"
}

check cci "$camera/NezhaCameraCci.c" "$camera/NezhaCameraOvx10500u.c"
check cci-burst "$camera/NezhaCameraCci.c" "$camera/NezhaCameraOvx10500u.c"
check rdi1080 "$camera/NezhaCameraRdi1080.c"
check camss1080 "$camera/NezhaCameraRdi1080.c" "$camera/NezhaCameraCamss1080.c"
check csiphy24 "$camera/NezhaCameraCsiphy24.c"
check dma "$camera/NezhaCameraRdi1080.c" "$camera/NezhaCameraDma.c" \
    "$smmu/ArmSmmu4K.c" "$smmu/ArmSmmu4KContext.c" "$smmu/ArmSmmu4KProgram.c" \
    "$smmu/ArmSmmu4KStreams.c"
check camcc "$camera/NezhaCameraCamcc.c"
check crm "$camera/NezhaCameraCrm.c"
check cpas "$camera/NezhaCameraCpas.c"
check profile "$camera/NezhaCameraProfile.c"
check main-capture "$camera/NezhaCameraMainCapture.c" "$camera/NezhaCameraRdi1080.c"
check main-pins "$camera/NezhaCameraMainPins.c"
check rpmh "$camera/NezhaCameraRpmh.c" "$camera/upstream/QcomRpmh.c"
check ife-bandwidth "$camera/NezhaCameraIfeBandwidth.c" "$camera/upstream/QcomRpmh.c"
check ife-rsc "$camera/NezhaCameraIfeRsc.c"
check irq-queue "$camera/NezhaCameraIrqQueue.c"
check frame-probe "$camera/NezhaCameraFrameProbe.c" "$camera/NezhaCameraMainCapture.c" \
    "$camera/NezhaCameraCamss1080.c" "$camera/NezhaCameraRdi1080.c" \
    "$camera/NezhaCameraOvx10500u.c" "$camera/NezhaCameraCci.c" "$camera/NezhaCameraDma.c" \
    "$smmu/ArmSmmu4K.c" "$smmu/ArmSmmu4KContext.c" "$smmu/ArmSmmu4KProgram.c" "$smmu/ArmSmmu4KStreams.c"

mkdir -p "$out/native-fixture/IOKit"
printf 'typedef int IOReturn;\n#define kIOReturnSuccess 0\nextern "C" void IOSleep(unsigned);\n' \
    > "$out/native-fixture/IOKit/IOLib.h"
for unit in IrqQueue Camss1080 Rdi1080; do
    "$host_cc" "${host_flags[@]}" -c "$camera/NezhaCamera$unit.c" -o "$out/irq-$unit.o"
done
"$host_cxx" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"$camera" -I"$out/native-fixture" \
    "$camera/NezhaCameraNativeIrq.cpp" tests/camera-native-irq.cpp \
    "$out/irq-IrqQueue.o" "$out/irq-Camss1080.o" "$out/irq-Rdi1080.o" -o "$out/test-camera-native-irq"
"$out/test-camera-native-irq"

python3 "$device/scripts/build-nezha27-camera-core.py" --out "$out"
