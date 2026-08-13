#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Validate the host-build K1 ELF before it is copied to boot media.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"

ELF="${K1_ELF:-${WORKSPACE_ROOT}/cmake_out/muse_pi_pro_nsh/nuttx}"
CONFIG="${K1_CONFIG:-${WORKSPACE_ROOT}/cmake_out/muse_pi_pro_nsh/.config}"
UART_SOURCE="${K1_UART_SOURCE:-${CONTEST_ROOT}/chip/k1/k1_console.c}"
EXPECTED_ENTRY="${K1_EXPECTED_ENTRY:-0x11000000}"
TOOLCHAIN_BIN="${K1_TOOLCHAIN_BIN:-${WORKSPACE_ROOT}/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin}"
EXPERIMENTAL_IRQ=0

usage()
{
  cat <<'EOF'
Usage: check_k1_elf.sh [options]

Options:
  --elf PATH          ELF to validate
  --config PATH       Generated NuttX .config
  --uart-source PATH  K1 polling console source
  --entry ADDRESS     Expected ELF entry (default: 0x11000000)
  --toolchain-bin DIR Directory containing riscv-none-elf tools
  --experimental-irq  Allow and require the K1 PLIC/GPIO IRQ path
  -h, --help          Show this help
EOF
}

pass()
{
  printf 'PASS: %s\n' "$1"
}

fail()
{
  printf 'FAIL: %s\n' "$1" >&2
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
    --uart-source)
      UART_SOURCE="$2"
      shift 2
      ;;
    --entry)
      EXPECTED_ENTRY="$2"
      shift 2
      ;;
    --toolchain-bin)
      TOOLCHAIN_BIN="$2"
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

READELF="${TOOLCHAIN_BIN}/riscv-none-elf-readelf"
NM="${TOOLCHAIN_BIN}/riscv-none-elf-nm"
SIZE="${TOOLCHAIN_BIN}/riscv-none-elf-size"

[[ -f "${ELF}" ]] || fail "ELF not found: ${ELF}"
[[ -f "${CONFIG}" ]] || fail "config not found: ${CONFIG}"
[[ -f "${UART_SOURCE}" ]] || fail "UART source not found: ${UART_SOURCE}"
[[ -x "${READELF}" ]] || fail "readelf not found: ${READELF}"
[[ -x "${NM}" ]] || fail "nm not found: ${NM}"
[[ -x "${SIZE}" ]] || fail "size not found: ${SIZE}"

HEADER="$("${READELF}" -h "${ELF}")"
grep -Eq 'Class:[[:space:]]+ELF64' <<<"${HEADER}" ||
  fail "ELF class is not ELF64"
grep -Eq 'Data:[[:space:]]+2.s complement, little endian' <<<"${HEADER}" ||
  fail "ELF is not little-endian"
grep -Eq 'Machine:[[:space:]]+RISC-V' <<<"${HEADER}" ||
  fail "ELF machine is not RISC-V"
pass "ELF64 little-endian RISC-V"

ENTRY="$(awk -F: '/Entry point address/ {gsub(/[[:space:]]/, "", $2); print tolower($2)}' <<<"${HEADER}")"
[[ "${ENTRY}" == "${EXPECTED_ENTRY,,}" ]] ||
  fail "entry is ${ENTRY}, expected ${EXPECTED_ENTRY}"
pass "entry ${ENTRY}"

mapfile -t LOADS < <(
  "${READELF}" -W -l "${ELF}" |
    awk '$1 == "LOAD" {
      flags = "";
      for (i = 7; i < NF; i++)
        {
          flags = flags $i;
        }
      print $3, $6, flags;
    }'
)

((${#LOADS[@]} >= 2)) || fail "expected at least two LOAD segments"

declare -a STARTS
declare -a ENDS
ENTRY_VALUE=$((ENTRY))
ENTRY_IN_EXEC=0
LOWEST_START=-1

for index in "${!LOADS[@]}"; do
  read -r vaddr memsz flags <<<"${LOADS[${index}]}"
  start=$((vaddr))
  end=$((start + memsz))

  ((memsz > 0)) || fail "LOAD ${index} has zero memory size"
  [[ ! ("${flags}" == *W* && "${flags}" == *E*) ]] ||
    fail "LOAD ${index} is writable and executable (${flags})"

  STARTS[${index}]="${start}"
  ENDS[${index}]="${end}"

  if ((LOWEST_START < 0 || start < LOWEST_START)); then
    LOWEST_START="${start}"
  fi

  if [[ "${flags}" == *E* ]] &&
     ((ENTRY_VALUE >= start && ENTRY_VALUE < end)); then
    ENTRY_IN_EXEC=1
  fi

  printf 'PASS: LOAD[%d] vaddr=0x%x memsz=0x%x flags=%s\n' \
    "${index}" "${start}" "$((memsz))" "${flags}"
done

((LOWEST_START == ENTRY_VALUE)) ||
  fail "lowest LOAD does not begin at the entry address"
((ENTRY_IN_EXEC == 1)) || fail "entry is not inside an executable LOAD"

for ((left = 0; left < ${#STARTS[@]}; left++)); do
  for ((right = left + 1; right < ${#STARTS[@]}; right++)); do
    if ((STARTS[left] < ENDS[right] && STARTS[right] < ENDS[left])); then
      fail "LOAD ${left} overlaps LOAD ${right}"
    fi
  done
done
pass "LOAD segments are non-overlapping and contain no RWX segment"

SYMBOLS="$("${NM}" -g --defined-only "${ELF}")"
for symbol in __start __global_pointer\$ k1_start nsh_main; do
  awk -v wanted="${symbol}" '$NF == wanted { found = 1 }
    END { exit found ? 0 : 1 }' <<<"${SYMBOLS}" ||
    fail "required symbol missing: ${symbol}"
done
pass "required boot and NSH symbols"

if grep -qx "CONFIG_ARCH_RV_EXT_SSTC=y" "${CONFIG}"; then
  pass "S-mode timer uses the SSTC stimecmp CSR"
else
  awk '$NF == "riscv_sbi_set_timer" { found = 1 }
    END { exit found ? 0 : 1 }' <<<"${SYMBOLS}" ||
    fail "SBI TIME timer symbol is missing while SSTC is disabled"
  pass "S-mode timer uses the SBI TIME extension"
fi

for option in ARCH_USE_S_MODE ARCH_RV_ISA_ZICSR_ZIFENCEI \
              K1_PRESERVE_BOOT_UART K1_EARLY_BOOT_LOG ALARM_ARCH \
              DEV_CONSOLE; do
  grep -qx "CONFIG_${option}=y" "${CONFIG}" ||
    fail "CONFIG_${option} is not enabled"
done

if ((EXPERIMENTAL_IRQ == 1)); then
  grep -qx "CONFIG_K1_PLIC=y" "${CONFIG}" ||
    fail "experimental IRQ validation requires CONFIG_K1_PLIC=y"
  grep -qx "CONFIG_K1_GPIO_IRQ=y" "${CONFIG}" ||
    fail "experimental IRQ validation requires CONFIG_K1_GPIO_IRQ=y"
  grep -qx "CONFIG_SMP=y" "${CONFIG}" &&
    fail "K1 GPIO IRQ validation requires SMP to remain disabled"
  pass "K1 experimental PLIC/GPIO IRQ Kconfig invariants"
else
  for option in SMP K1_PLIC; do
    if grep -qx "CONFIG_${option}=y" "${CONFIG}"; then
      fail "CONFIG_${option} must remain disabled for initial bring-up"
    fi
  done
  pass "K1 initial bring-up Kconfig invariants"
fi

if grep -Eq \
  'k1_uart_putreg[[:space:]]*\([[:space:]]*K1_UART_IER_OFFSET' \
  "${UART_SOURCE}"; then
  fail "K1 console writes UART IER"
fi

if grep -Eq \
  'putreg(8|16|32)?[[:space:]]*\([^;]*K1_UART_IER_OFFSET' \
  "${UART_SOURCE}"; then
  fail "K1 console writes UART IER through a raw putreg call"
fi

grep -Eq \
  'k1_uart_putreg[[:space:]]*\([[:space:]]*K1_UART_THR_OFFSET' \
  "${UART_SOURCE}" || fail "K1 console THR write path is missing"
pass "polling console writes THR and has no IER write path"

SHA256="$(sha256sum "${ELF}" | awk '{print $1}')"
printf '\nK1 ELF validation succeeded\n'
printf '  ELF:    %s\n' "${ELF}"
printf '  SHA256: %s\n' "${SHA256}"
"${SIZE}" "${ELF}"
