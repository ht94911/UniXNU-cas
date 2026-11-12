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
check rdi1080 "$camera/NezhaCameraRdi1080.c"
check camss1080 "$camera/NezhaCameraRdi1080.c" "$camera/NezhaCameraCamss1080.c"
check dma "$camera/NezhaCameraRdi1080.c" "$camera/NezhaCameraDma.c" \
    "$smmu/ArmSmmu4K.c" "$smmu/ArmSmmu4KContext.c" "$smmu/ArmSmmu4KProgram.c" \
    "$smmu/ArmSmmu4KStreams.c"
check camcc "$camera/NezhaCameraCamcc.c"
check main-capture "$camera/NezhaCameraMainCapture.c" "$camera/NezhaCameraRdi1080.c"
check main-pins "$camera/NezhaCameraMainPins.c"

python3 "$device/scripts/build-nezha27-camera-core.py" --out "$out"
