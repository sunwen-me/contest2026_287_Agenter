#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
TOOLCHAIN_BIN="${K1_TOOLCHAIN_BIN:-${WORKSPACE_ROOT}/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin}"

exec python3 "${SCRIPT_DIR}/decode_k1_trap.py" \
  --addr2line "${TOOLCHAIN_BIN}/riscv-none-elf-addr2line" \
  "$@"
