#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Create a non-destructive U-Boot handoff package for first-board bring-up.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
ELF="${K1_ELF:-${WORKSPACE_ROOT}/cmake_out/muse_pi_pro_nsh/nuttx}"
CONFIG="${K1_CONFIG:-${WORKSPACE_ROOT}/cmake_out/muse_pi_pro_nsh/.config}"
OUTPUT="${K1_PACKAGE_DIR:-${WORKSPACE_ROOT}/out/k1-bringup}"

usage()
{
  cat <<'EOF'
Usage: package_k1_bringup.sh [options]

Options:
  --elf PATH       K1 ELF to package
  --config PATH    Generated NuttX .config
  --output DIR     Package output directory
  -h, --help       Show this help
EOF
}

fail()
{
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

while (($# > 0)); do
  case "$1" in
    --elf)
      ELF="$2"
      shift 2
      ;;
    --config)
      CONFIG="$2"
      shift 2
      ;;
    --output)
      OUTPUT="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

[[ -f "${ELF}" ]] || fail "ELF not found: ${ELF}"
[[ -f "${CONFIG}" ]] || fail "config not found: ${CONFIG}"

mkdir -p "${OUTPUT}"
mkdir -p "${OUTPUT}/licenses"
install -m 0644 "${ELF}" "${OUTPUT}/nuttx"
install -m 0644 \
  "${CONTEST_ROOT}/docs/K1_UBOOT_BRINGUP.md" \
  "${OUTPUT}/K1_UBOOT_BRINGUP.md"
install -m 0644 \
  "${CONTEST_ROOT}/docs/K1_TEST_RECORD_TEMPLATE.md" \
  "${OUTPUT}/TEST_RECORD.md"
install -m 0644 \
  "${CONTEST_ROOT}/docs/K1_HOST_TOOLING.md" \
  "${OUTPUT}/K1_HOST_TOOLING.md"
install -m 0644 \
  "${CONTEST_ROOT}/docs/K1_SOURCE_AND_LICENSES.md" \
  "${OUTPUT}/SOURCE_AND_LICENSES.md"
install -m 0755 \
  "${CONTEST_ROOT}/tools/capture_k1_serial.py" \
  "${OUTPUT}/capture_k1_serial.py"
install -m 0755 \
  "${CONTEST_ROOT}/tools/decode_k1_trap.py" \
  "${OUTPUT}/decode_k1_trap.py"
install -m 0644 \
  "${CONTEST_ROOT}/LICENSE" \
  "${OUTPUT}/licenses/CONTEST-LICENSE"
install -m 0644 \
  "${WORKSPACE_ROOT}/nuttx/LICENSE" \
  "${OUTPUT}/licenses/NUTTX-LICENSE"

if [[ -f "${WORKSPACE_ROOT}/nuttx/NOTICE" ]]; then
  install -m 0644 \
    "${WORKSPACE_ROOT}/nuttx/NOTICE" \
    "${OUTPUT}/licenses/NUTTX-NOTICE"
fi

"${SCRIPT_DIR}/check_k1_elf.sh" \
  --elf "${OUTPUT}/nuttx" \
  --config "${CONFIG}" \
  --uart-source "${CONTEST_ROOT}/chip/k1/k1_console.c" \
  >"${OUTPUT}/elf-report.txt"

(
  cd "${OUTPUT}"
  sha256sum nuttx >nuttx.sha256
)

ELF_SIZE="$(stat -c '%s' "${OUTPUT}/nuttx")"
ELF_SIZE_HEX="$(printf '0x%x' "${ELF_SIZE}")"
ELF_SHA256="$(awk '{print $1}' "${OUTPUT}/nuttx.sha256")"

sed \
  -e "s/@ELF_SIZE@/${ELF_SIZE}/g" \
  -e "s/@ELF_SIZE_HEX@/${ELF_SIZE_HEX}/g" \
  -e "s/@ELF_SHA256@/${ELF_SHA256}/g" \
  "${SCRIPT_DIR}/uboot-commands.txt.in" \
  >"${OUTPUT}/uboot-commands.txt"

cat >"${OUTPUT}/PACKAGE_MANIFEST.txt" <<EOF
Target: MUSE Pi Pro (SpacemiT K1)
Payload: nuttx
ELF staging address: 0x12000000
ELF entry/PT_LOAD base: 0x11000000
ELF size: ${ELF_SIZE} bytes (${ELF_SIZE_HEX})
ELF SHA256: ${ELF_SHA256}

Files:
  nuttx                     RV64 S-mode NuttX ELF
  nuttx.sha256              Host integrity checksum
  elf-report.txt            Offline ELF/config/UART validation
  uboot-commands.txt        Copy-ready U-Boot command sequence
  K1_UBOOT_BRINGUP.md       Bring-up and recovery guide
  TEST_RECORD.md            First-board evidence template
  K1_HOST_TOOLING.md        Serial capture, trap decode and CI commands
  SOURCE_AND_LICENSES.md    Source provenance and license inventory
  capture_k1_serial.py      Standalone raw serial capture tool
  decode_k1_trap.py         Standalone trap parser/symbolizer
  licenses/                 Contest and NuttX license/notice files
EOF

printf 'K1 U-Boot bring-up package generated\n'
printf '  Directory: %s\n' "${OUTPUT}"
printf '  SHA256:   %s\n' "${ELF_SHA256}"
