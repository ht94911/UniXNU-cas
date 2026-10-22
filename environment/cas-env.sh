#!/usr/bin/env bash
# Source this file from WSL before building. Does not install or operate a device.
export CAS_WORK_ROOT="${CAS_WORK_ROOT:-${HOME}/cas-work}"
if [[ ! -f "${CAS_WORK_ROOT}/.venv/bin/activate" ]]; then
    printf 'Missing project venv: %s/.venv\n' "${CAS_WORK_ROOT}" >&2
    return 1
fi
source "${CAS_WORK_ROOT}/.venv/bin/activate"
export CLANGPDB_BIN=/usr/lib/llvm-18/bin/
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
export PATH="${CLANGPDB_BIN}:${PATH}"
export PYTHONHASHSEED=0
export LC_ALL=C.UTF-8
export TZ=UTC
export CAS_JOBS="${CAS_JOBS:-8}"
# SOURCE_DATE_EPOCH is set by the source/candidate manifest, never wall-clock time.
