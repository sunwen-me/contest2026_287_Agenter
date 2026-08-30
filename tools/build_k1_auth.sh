#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Build and package the RTL8852BS authentication diagnostic image for the
# MUSE Pi Pro K1.  The board configuration, the CMake output directory and the
# U-Boot package directory are the ones every authentication board run uses,
# so an incremental build keeps its ccache and its object tree.
#
# Usage: build_k1_auth.sh [--clean] [--jobs N]

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"

CONFIG_PATH="${CONTEST_ROOT}/board/k1/muse_pi_pro/configs/wireless_auth_diag"
BUILD_DIR="${WORKSPACE_ROOT}/cmake_out/k1-auth"
PACKAGE_DIR="${WORKSPACE_ROOT}/out/k1-auth"
JOBS="${K1_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '8')}"
CLEAN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --clean) CLEAN=1 ;;
    --jobs) shift; JOBS="$1" ;;
    -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
  shift
done

ARGS=(--config "${CONFIG_PATH}" --build-dir "${BUILD_DIR}"
      --package --package-dir "${PACKAGE_DIR}" --jobs "${JOBS}")
if [ "${CLEAN}" -eq 1 ]; then
  ARGS+=(--clean)
fi

exec "${SCRIPT_DIR}/build_k1.sh" "${ARGS[@]}"
