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
FLAT="${K1_FLAT:-}"
WRAPPER="${K1_WRAPPER:-}"
EXPERIMENTAL_IRQ=0

usage()
{
  cat <<'EOF'
Usage: package_k1_bringup.sh [options]

Options:
  --elf PATH       K1 ELF to package
  --config PATH    Generated NuttX .config
  --output DIR     Package output directory
  --flat PATH      Existing flat NuttX binary (optional)
  --wrapper PATH   Existing U-Boot wrapper binary (optional)
  --experimental-irq  Package the explicit K1 PLIC/GPIO IRQ build
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
    --flat)
      FLAT="$2"
      shift 2
      ;;
    --wrapper)
      WRAPPER="$2"
      shift 2
      ;;
    --experimental-irq)
      EXPERIMENTAL_IRQ=1
      shift
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

if [[ -n "${FLAT}" ]]; then
  [[ -f "${FLAT}" ]] || fail "flat binary not found: ${FLAT}"
  install -m 0755 "${FLAT}" "${OUTPUT}/contest-nuttx-flat.bin"
else
  OBJCOPY="${WORKSPACE_ROOT}/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin/riscv-none-elf-objcopy"
  [[ -x "${OBJCOPY}" ]] || fail "objcopy not found: ${OBJCOPY}"
  "${OBJCOPY}" -O binary "${ELF}" "${OUTPUT}/contest-nuttx-flat.bin"
fi

if [[ -n "${WRAPPER}" ]]; then
  [[ -f "${WRAPPER}" ]] || fail "wrapper binary not found: ${WRAPPER}"
  install -m 0755 "${WRAPPER}" "${OUTPUT}/k1-go-wrapper.bin"
else
  "${SCRIPT_DIR}/build_k1_wrapper.sh" --output-dir "${OUTPUT}"
fi
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
  "${CONTEST_ROOT}/tools/console_k1_serial.py" \
  "${OUTPUT}/console_k1_serial.py"
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

CHECK_ARGS=(
  --elf "${OUTPUT}/nuttx"
  --config "${CONFIG}"
  --uart-source "${CONTEST_ROOT}/chip/k1/k1_console.c"
)

if ((EXPERIMENTAL_IRQ == 1)); then
  CHECK_ARGS+=(--experimental-irq)
fi

"${SCRIPT_DIR}/check_k1_elf.sh" "${CHECK_ARGS[@]}" \
  >"${OUTPUT}/elf-report.txt"

(
  cd "${OUTPUT}"
  sha256sum nuttx >nuttx.sha256
)

ELF_SIZE="$(stat -c '%s' "${OUTPUT}/nuttx")"
ELF_SIZE_HEX="$(printf '0x%x' "${ELF_SIZE}")"
ELF_SHA256="$(awk '{print $1}' "${OUTPUT}/nuttx.sha256")"
FLAT_SIZE="$(stat -c '%s' "${OUTPUT}/contest-nuttx-flat.bin")"
FLAT_SIZE_HEX="$(printf '0x%x' "${FLAT_SIZE}")"
FLAT_SHA256="$(sha256sum "${OUTPUT}/contest-nuttx-flat.bin" | awk '{print $1}')"
WRAPPER_SIZE="$(stat -c '%s' "${OUTPUT}/k1-go-wrapper.bin")"
WRAPPER_SHA256="$(sha256sum "${OUTPUT}/k1-go-wrapper.bin" | awk '{print $1}')"

sed \
  -e "s/@ELF_SIZE@/${ELF_SIZE}/g" \
  -e "s/@ELF_SIZE_HEX@/${ELF_SIZE_HEX}/g" \
  -e "s/@ELF_SHA256@/${ELF_SHA256}/g" \
  -e "s/@FLAT_SIZE@/${FLAT_SIZE}/g" \
  -e "s/@FLAT_SIZE_HEX@/${FLAT_SIZE_HEX}/g" \
  -e "s/@FLAT_SHA256@/${FLAT_SHA256}/g" \
  -e "s/@WRAPPER_SIZE@/${WRAPPER_SIZE}/g" \
  -e "s/@WRAPPER_SHA256@/${WRAPPER_SHA256}/g" \
  "${SCRIPT_DIR}/uboot-commands.txt.in" \
  >"${OUTPUT}/uboot-commands.txt"

cat >"${OUTPUT}/PACKAGE_MANIFEST.txt" <<EOF
Target: MUSE Pi Pro (SpacemiT K1)
Payload: nuttx
Validation profile: $([[ "${EXPERIMENTAL_IRQ}" == 1 ]] &&
  printf 'experimental K1 GPIO IRQ' || printf 'initial bring-up')
ELF staging address: 0x12000000
ELF entry/PT_LOAD base: 0x11000000
ELF size: ${ELF_SIZE} bytes (${ELF_SIZE_HEX})
ELF SHA256: ${ELF_SHA256}
Flat payload: contest-nuttx-flat.bin
Flat size: ${FLAT_SIZE} bytes (${FLAT_SIZE_HEX})
Flat SHA256: ${FLAT_SHA256}
Wrapper: k1-go-wrapper.bin (${WRAPPER_SIZE} bytes)
Wrapper SHA256: ${WRAPPER_SHA256}

Files:
  nuttx                     RV64 S-mode NuttX ELF
  contest-nuttx-flat.bin    Real-board flat payload for U-Boot go
  k1-go-wrapper.bin         RAM-only U-Boot handoff wrapper
  nuttx.sha256              Host integrity checksum
  elf-report.txt            Offline ELF/config/UART validation
  uboot-commands.txt        Copy-ready U-Boot command sequence
  K1_UBOOT_BRINGUP.md       Bring-up and recovery guide
  TEST_RECORD.md            First-board evidence template
  K1_HOST_TOOLING.md        Serial capture, trap decode and CI commands
  SOURCE_AND_LICENSES.md    Source provenance and license inventory
  capture_k1_serial.py      Standalone raw serial capture tool
  console_k1_serial.py      Interactive raw serial console
  decode_k1_trap.py         Standalone trap parser/symbolizer
  licenses/                 Contest and NuttX license/notice files
EOF

printf 'K1 U-Boot bring-up package generated\n'
printf '  Directory: %s\n' "${OUTPUT}"
printf '  SHA256:   %s\n' "${ELF_SHA256}"
