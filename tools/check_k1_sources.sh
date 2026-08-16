#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Static checks that do not require an openvela checkout or cross toolchain.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

pass()
{
  printf 'PASS: %s\n' "$1"
}

fail()
{
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

while IFS= read -r script; do
  bash -n "${script}"
done < <(find "${CONTEST_ROOT}/tools" -maxdepth 1 -type f -name '*.sh' |
         sort)
pass "shell syntax"

python3 -c \
  'import pathlib, sys, xml.etree.ElementTree as ET; ET.parse(sys.argv[1]); [compile(p.read_text(), str(p), "exec", flags=0, dont_inherit=True) for p in sorted(pathlib.Path(sys.argv[2]).glob("*.py"))]' \
  "${CONTEST_ROOT}/contest2026_287_Agenter.xml" \
  "${CONTEST_ROOT}/tools"
pass "manifest XML and Python syntax"

if grep -Eq \
  'k1_uart_putreg[[:space:]]*\([[:space:]]*K1_UART_IER_OFFSET' \
  "${CONTEST_ROOT}/chip/k1/k1_console.c"; then
  fail "K1 console writes UART IER"
fi

if grep -Eq \
  'putreg(8|16|32)?[[:space:]]*\([^;]*K1_UART_IER_OFFSET' \
  "${CONTEST_ROOT}/chip/k1/k1_console.c"; then
  fail "K1 console writes UART IER through a raw putreg call"
fi
pass "polling UART source has no IER write"

grep -Fq 'la   gp, __global_pointer$' \
  "${CONTEST_ROOT}/chip/k1/k1_head.S" ||
  fail "entry does not initialize gp"
grep -Fq 'PROVIDE(__global_pointer$' \
  "${CONTEST_ROOT}/board/k1/muse_pi_pro/scripts/ld.script" ||
  fail "linker script does not provide __global_pointer$"
pass "RISC-V gp initialization contract"

awk '
  $1 == "config" { inside = ($2 == "K1_PLIC") }
  inside && $1 == "default" && $2 == "n" { found = 1 }
  END { exit found ? 0 : 1 }
' "${CONTEST_ROOT}/chip/k1/Kconfig" ||
  fail "CONFIG_K1_PLIC is not default n"

if grep -qx 'CONFIG_K1_PLIC=y' \
  "${CONTEST_ROOT}/board/k1/muse_pi_pro/configs/nsh/defconfig"; then
  fail "initial NSH defconfig unexpectedly enables K1 PLIC"
fi
pass "PLIC remains opt-in"

awk '
  $1 == "config" { inside = ($2 == "K1_WATCHDOG") }
  inside && $1 == "default" && $2 == "n" { found = 1 }
  END { exit found ? 0 : 1 }
' "${CONTEST_ROOT}/chip/k1/Kconfig" ||
  fail "CONFIG_K1_WATCHDOG is not default n"

if grep -qx 'CONFIG_K1_WATCHDOG=y' \
  "${CONTEST_ROOT}/board/k1/muse_pi_pro/configs/nsh/defconfig"; then
  fail "initial NSH defconfig unexpectedly enables K1 watchdog"
fi
pass "K1 watchdog remains opt-in"

awk '
  $1 == "config" { inside = ($2 == "K1_WATCHDOG") }
  inside && $0 ~ /depends on WATCHDOG && !WATCHDOG_AUTOMONITOR/ { found = 1 }
  END { exit found ? 0 : 1 }
' "${CONTEST_ROOT}/chip/k1/Kconfig" ||
  fail "CONFIG_K1_WATCHDOG must exclude WATCHDOG_AUTOMONITOR"
pass "K1 watchdog uses manual control only"

wifi_sdio_source="${CONTEST_ROOT}/chip/k1/k1_sdio.c"
if [[ "$(grep -c 'SDIO_CCCR_IOEN' "${wifi_sdio_source}")" != "1" ]] ||
   ! grep -Eq \
     'k1_sdio_wifi_cmd52\(dev, false, SDIO_CCCR_IOEN, 0,' \
     "${wifi_sdio_source}"; then
  fail "Wi-Fi enumeration must only read SDIO_CCCR_IOEN"
fi
pass "Wi-Fi enumeration leaves SDIO IOEN untouched"

bt_h5_source="${CONTEST_ROOT}/chip/k1/k1_bt_uart.c"
wireless_defconfig="${CONTEST_ROOT}/board/k1/muse_pi_pro/configs/wireless/defconfig"
if grep -Eq 'btuart_register|K1_UART_IER_RDA|irq_attach|up_enable_irq' \
  "${bt_h5_source}"; then
  fail "Bluetooth H5 diagnostic must not register H4 or enable UART2 IRQ"
fi

grep -Fq 'K1_BT_HCI_READ_LOCAL_VER' "${bt_h5_source}" ||
  fail "Bluetooth H5 diagnostic must use the standard local-version opcode"

if grep -Eq 'CONFIG_K1_PLIC=y|CONFIG_UART_BTH4=y' \
  "${wireless_defconfig}"; then
  fail "wireless H5 diagnostic must not enable the H4 or PLIC path"
fi
pass "Bluetooth remains a polled H5 diagnostic"

if git -C "${CONTEST_ROOT}" rev-parse --is-inside-work-tree \
     >/dev/null 2>&1; then
  # Patch files contain mandatory one-character context prefixes for blank
  # lines. They are valid patch syntax but look like trailing whitespace to
  # git diff --check, so validate source files separately.
  git -C "${CONTEST_ROOT}" diff --check -- . \
    ':(exclude)tools/patches/*.patch'
  pass "git whitespace check"
fi

if command -v shellcheck >/dev/null 2>&1; then
  while IFS= read -r script; do
    shellcheck "${script}"
  done < <(find "${CONTEST_ROOT}/tools" -maxdepth 1 -type f -name '*.sh' |
           sort)
  pass "shellcheck"
else
  printf 'SKIP: shellcheck is not installed\n'
fi

printf '\nK1 source validation succeeded\n'
