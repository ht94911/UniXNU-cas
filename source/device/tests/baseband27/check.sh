#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Host models of the nezha iOS 27 baseband sources. No device, no XNU tree; not part of an image build.
# Usage, from the device tree: sh tests/baseband27/check.sh [QcomBaseband checkout for the derived bridge model]
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
pkg=$root/package/baseband27
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc=${CC:-gcc}
flags="-std=gnu11 -Wall -Wextra -Wno-type-limits -Wno-format -fsanitize=address,undefined"
mkdir "$out/bridge"
cp "$pkg"/bridge/upstream/*.h "$out/bridge/"
cp "$pkg"/bridge/*.h "$out/bridge/"      # the derived headers replace their upstream copies
run() { name=$1; shift; $cc $flags "$@" -o "$out/$name"; "$out/$name"; }
run ipa55_values -I "$pkg/ipa" "$root/tests/baseband27/ipa55_values.c" > "$out/ipa55_values.txt"
echo "ipa55 values: ok ($(wc -l < "$out/ipa55_values.txt") lines)"
run ipa55_model -I "$pkg/ipa" "$root/tests/baseband27/ipa55_model_test.c"
python3 "$root/tests/baseband27/ipa_sram_test.py"
run bridge27_values -I "$out/bridge" "$root/tests/baseband27/bridge27_values.c"
run qmux_version -Werror -I "$out/bridge" "$root/tests/baseband27/qmux_version_test.c"
run qmux_endpoint -Werror -I "$out/bridge" "$root/tests/baseband27/qmux_endpoint_test.c"
run qmux_capacity -Werror -I "$out/bridge" "$root/tests/baseband27/qmux_capacity_test.c"
run nas_mux -Werror -I "$out/bridge" "$root/tests/baseband27/nas_mux_test.c"
run sim_identity -Werror "$root/tests/baseband27/sim_identity_test.c"
run device_identity -Werror "$root/tests/baseband27/device_identity_test.c"
run pdp_flow -Werror -I "$out/bridge" "$root/tests/baseband27/pdp_flow_test.c"
run wds_completion -Werror -I "$out/bridge" "$root/tests/baseband27/wds_completion_test.c"
run low_memory -Werror "$root/tests/baseband27/low_memory_test.c"
run voice_state -Werror "$root/tests/baseband27/voice_state_test.c"
run transport_model "$root/tests/baseband27/transport_model_test.c"
if [ $# -gt 0 ]; then
    python3 "$root/tools/baseband27/derive-bridge27-test.py" "$1/tests/coachz_bb_model_test.c" > "$out/bb_model27.c"
    cmp -s "$out/bb_model27.c" "$root/tests/baseband27/coachz_bb_model_test27.c" || { echo "coachz_bb_model_test27.c is stale"; exit 1; }
fi
run bb_model27 -w -I "$out/bridge" "$root/tests/baseband27/coachz_bb_model_test27.c"
run board_core "$root/tests/baseband27/board_core_test.c" "$pkg/platform/NbPas.c" "$pkg/upstream/QcomMdt.c" "$pkg/transport/NbQrtr.c" "$pkg/services/NbRmtfs.c" "$pkg/platform/NbMss.c" "$pkg/upstream/QcomGlink.c" "$pkg/upstream/QcomSmem.c" "$pkg/upstream/QcomSmp2p.c"
run mss_reuse -Werror "$root/tests/baseband27/mss_reuse_test.c" "$pkg/platform/NbMss.c" "$pkg/platform/NbPas.c" "$pkg/upstream/QcomMdt.c" "$pkg/upstream/QcomGlink.c" "$pkg/upstream/QcomSmem.c" "$pkg/upstream/QcomSmp2p.c"
run ipa_smem -Werror "$root/tests/baseband27/ipa_smem_test.c" "$pkg/upstream/QcomSmem.c"
run startup_order -Werror "$root/tests/baseband27/startup_order_test.c" "$pkg/platform/NbBaseband.c" "$pkg/platform/NbMss.c" "$pkg/platform/NbPas.c" "$pkg/upstream/QcomMdt.c" "$pkg/upstream/QcomGlink.c" "$pkg/upstream/QcomSmem.c" "$pkg/upstream/QcomSmp2p.c" "$pkg/transport/NbQrtr.c" "$pkg/services/NbRmtfs.c" "$pkg/services/NbTftp.c" "$pkg/services/NbMemshare.c" "$pkg/services/NbServloc.c"
run boot_ready -Werror "$root/tests/baseband27/boot_ready_test.c" "$pkg/transport/NbQrtr.c"
python3 "$root/tests/baseband27/image_test.py"
ufs=${BB_UFS_SOURCE:-$root/../../gpu27-astra/ufs-persist}
${CXX:-g++} -std=c++17 -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -no-pie -I "$ufs" "$root/tests/baseband27/ufs_reader_test.cpp" -o "$out/ufs_reader"
"$out/ufs_reader"
python3 "$root/tests/baseband27/resources_test.py"
run late_start -no-pie -I "$root/tests/baseband27" -I "$root/../usb-shared/include" "$root/tests/baseband27/start_test.c"
python3 "$root/tests/baseband27/native_test.py"
python3 "$root/tests/baseband27/audio_tas_lease_test.py"
python3 "$root/tests/baseband27/integration_test.py"
python3 "$root/tests/baseband27/generator_test.py"
python3 "$root/tests/baseband27/cellular_launch_test.py"
python3 "$root/tests/baseband27/cellular_task_permissions_test.py"
python3 "$root/tests/baseband27/cellular_network_runtime_test.py"
python3 "$root/tests/baseband27/wds_diagnostics_test.py"
echo "baseband27 host models: all passed"
