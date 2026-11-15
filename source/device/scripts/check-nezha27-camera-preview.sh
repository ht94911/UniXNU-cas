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
python3 - <<'PY'
import hashlib, json
from pathlib import Path
p = Path('package/camera/userland/upstream/hakios')
origin = json.loads((p / 'ORIGIN.json').read_text())
assert origin['repository'] == 'https://github.com/yfl520/hackios-raphael'
assert origin['revision'] == 'f128f1d8e2ef1f588002787c5efb94ec134ddeac'
for name, digest in origin['sha256'].items():
    assert hashlib.sha256((p / name).read_bytes()).hexdigest() == digest, name
print('HakiOS RAW processor: four source/license hashes passed; local still allocation result retained')
PY
camera=package/camera/userland
flags=(-std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
       -fno-omit-frame-pointer -I"$camera")
# Preserve the pinned source; its pixel helper retains an unused ISP argument.
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" -Wno-unused-parameter \
    -c "$camera/upstream/hakios/camera_isp.c" -o "$out/hakios-isp.o"
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" "$camera/NezhaCameraPreview.c" \
    tests/camera-preview.c "$out/hakios-isp.o" -lm -o "$out/test-camera-preview"
"$out/test-camera-preview"
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" -Wno-unused-parameter \
    -Dmalloc=still_test_malloc -Dfree=still_test_free \
    -c "$camera/upstream/hakios/camera_isp.c" -o "$out/still-allocation-isp.o"
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" "$camera/NezhaCameraPreview.c" \
    tests/camera-still-allocation.c "$out/still-allocation-isp.o" -lm -o "$out/test-camera-still-allocation"
for slot in {1..7}; do "$out/test-camera-still-allocation" "$slot"; done
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" -Itests/camera-platform \
    "$camera/NezhaCameraPreview.c" "$camera/NezhaCameraPixelBuffer.c" \
    tests/camera-pixel-buffer.c "$out/hakios-isp.o" -lm -o "$out/test-camera-pixel-buffer"
"$out/test-camera-pixel-buffer"
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" \
    -Ddlopen=nezha_test_dlopen -Ddlsym=nezha_test_dlsym \
    -c "$camera/NezhaFigCapture24A446.c" -o "$out/fig-api.o"
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" \
    tests/camera-fig-api.c "$out/fig-api.o" -ldl -pthread -o "$out/test-camera-fig-api"
"$out/test-camera-fig-api"
"$out/test-camera-fig-api" missing-class-id
# Use the existing bundled Blocks runtime for actual copied-block lifetime.
blocks=/opt/unixnu-linux/CommandLineTools/usr/libdispatch
for name in "$camera/NezhaCameraGraphOutput.c" tests/camera-graph-output.c; do
    obj="$out/$(basename "$name" .c).o"
    "${CAMERA_HOST_CLANG:-clang}" "${flags[@]}" -fblocks -Itests/camera-platform \
        -I"$blocks/include" -c "$name" -o "$obj"
done
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" "$out/NezhaCameraGraphOutput.o" \
    "$out/camera-graph-output.o" -L"$blocks/lib" -Wl,-rpath,"$blocks/lib" \
    -lBlocksRuntime -o "$out/test-camera-graph-output"
"$out/test-camera-graph-output"
for name in "$camera/NezhaCameraFigProvider.c" "$camera/NezhaCameraFactory.c" tests/camera-fig-provider.c; do
    "${CAMERA_HOST_CLANG:-clang}" "${flags[@]}" -fblocks -Itests/camera-platform \
        -I"$blocks/include" -c "$name" -o "$out/$(basename "$name" .c).o"
done
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" "$out/NezhaCameraFigProvider.o" \
    "$out/NezhaCameraGraphOutput.o" "$out/camera-fig-provider.o" \
    "$camera/NezhaCameraPreview.c" "$camera/NezhaCameraPixelBuffer.c" "$camera/NezhaCameraFormats.c" \
    "$out/NezhaCameraFactory.o" "$out/hakios-isp.o" \
    -Itests/camera-platform -L"$blocks/lib" -Wl,-rpath,"$blocks/lib" \
    -lBlocksRuntime -ldispatch -lm -o "$out/test-camera-fig-provider"
"$out/test-camera-fig-provider"
"$out/test-camera-fig-provider" ae
"${CAMERA_HOST_CLANG:-clang}" "${flags[@]}" -fblocks -Itests/camera-platform \
    -I"$blocks/include" -Daccess=backend_test_access -Dopen=backend_test_open \
    -Dclose=backend_test_close -Dread=backend_test_read -Dioctl=backend_test_ioctl \
    -c "$camera/NezhaCameraDeviceBackend.c" -o "$out/NezhaCameraDeviceBackend.o"
"${CAMERA_HOST_CLANG:-clang}" "${flags[@]}" -fblocks -Itests/camera-platform \
    -I"$blocks/include" -c tests/camera-device-backend.c -o "$out/camera-device-backend.o"
"${CAMERA_HOST_CC:-gcc}" "${flags[@]}" "$out/NezhaCameraFigProvider.o" \
    "$out/NezhaCameraGraphOutput.o" "$out/NezhaCameraFactory.o" \
    "$out/NezhaCameraDeviceBackend.o" "$out/camera-device-backend.o" \
    "$camera/NezhaCameraPreview.c" "$camera/NezhaCameraPixelBuffer.c" "$camera/NezhaCameraFormats.c" \
    "$out/hakios-isp.o" -Itests/camera-platform -L"$blocks/lib" -Wl,-rpath,"$blocks/lib" \
    -lBlocksRuntime -ldispatch -lm -o "$out/test-camera-device-backend"
"$out/test-camera-device-backend"
"${CAMERA_HOST_CC:-gcc}" -std=gnu11 -Wall -Wextra -Werror -O2 -I"$camera" \
    "$camera/NezhaCameraPreview.c" "$camera/NezhaCameraRawPreview.c" \
    -Wno-unused-parameter "$camera/upstream/hakios/camera_isp.c" -lm \
    -o "$out/nezha-camera-raw-preview"
