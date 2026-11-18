#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# User's source-stage boundary: do not run until a fixed test package is chosen.
set -eu
task_gnss_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
task_gnss_temp=$(mktemp -d)
trap 'rm -rf "$task_gnss_temp"' EXIT HUP INT TERM
case $(uname -s) in
    Darwin) task_gnss_link_flags= ;;
    *) task_gnss_link_flags='-fno-pie -no-pie' ;;
esac
for task_gnss_case in gnss_protocol_test gnss_lifecycle_test gnss_qmux_test; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
        -fno-omit-frame-pointer $task_gnss_link_flags \
        "$task_gnss_root/tests/gnss27/$task_gnss_case.c" \
        "$task_gnss_root/package/baseband27/gnss/NbGnssProtocol.c" \
        "$task_gnss_root/package/baseband27/gnss/NbGnss.c" \
        -o "$task_gnss_temp/$task_gnss_case"
    "$task_gnss_temp/$task_gnss_case"
done
PYTHONDONTWRITEBYTECODE=1 python3 "$task_gnss_root/tests/gnss27/evidence_test.py"
PYTHONDONTWRITEBYTECODE=1 python3 "$task_gnss_root/tests/gnss27/launcher_job_test.py"
PYTHONDONTWRITEBYTECODE=1 python3 "$task_gnss_root/tests/gnss27/exception_context_test.py"
