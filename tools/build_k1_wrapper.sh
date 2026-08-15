#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
TOOLCHAIN_BIN="${K1_TOOLCHAIN_BIN:-${WORKSPACE_ROOT}/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin}"
OUTPUT_DIR="${K1_WRAPPER_DIR:-${WORKSPACE_ROOT}/out/k1-bringup}"

usage()
{
  cat <<'EOF'
Usage: build_k1_wrapper.sh [--output-dir DIR]

Build the RAM-only wrapper used by the real MUSE Pi Pro U-Boot `go` path.
EOF
}

while (($# > 0)); do
  case "$1" in
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'ERROR: unknown option: %s\n' "$1" >&2
      exit 1
      ;;
  esac
done

AS="${TOOLCHAIN_BIN}/riscv-none-elf-as"
LD="${TOOLCHAIN_BIN}/riscv-none-elf-ld"
OBJCOPY="${TOOLCHAIN_BIN}/riscv-none-elf-objcopy"
SOURCE="${SCRIPT_DIR}/k1_go_wrapper.S"
OBJECT="${OUTPUT_DIR}/k1-go-wrapper.o"
ELF="${OUTPUT_DIR}/k1-go-wrapper.elf"
BINARY="${OUTPUT_DIR}/k1-go-wrapper.bin"

[[ -x "${AS}" ]] || { printf 'ERROR: assembler not found: %s\n' "${AS}" >&2; exit 1; }
[[ -x "${LD}" ]] || { printf 'ERROR: linker not found: %s\n' "${LD}" >&2; exit 1; }
[[ -x "${OBJCOPY}" ]] || { printf 'ERROR: objcopy not found: %s\n' "${OBJCOPY}" >&2; exit 1; }
[[ -f "${SOURCE}" ]] || { printf 'ERROR: wrapper source not found: %s\n' "${SOURCE}" >&2; exit 1; }

mkdir -p "${OUTPUT_DIR}"
"${AS}" -march=rv64imac -mabi=lp64 -o "${OBJECT}" "${SOURCE}"
"${LD}" -m elf64lriscv -Ttext=0x12000000 --entry=_start \
  -o "${ELF}" "${OBJECT}"
"${OBJCOPY}" -O binary "${ELF}" "${BINARY}"

printf 'K1 U-Boot wrapper generated\n'
printf '  ELF:    %s (%s bytes)\n' "${ELF}" "$(stat -c '%s' "${ELF}")"
printf '  Binary: %s (%s bytes)\n' "${BINARY}" "$(stat -c '%s' "${BINARY}")"
