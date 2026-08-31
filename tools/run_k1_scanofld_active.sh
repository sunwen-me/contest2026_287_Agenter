#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Run the RTL8852BS active scan-offload diagnostic image on the MUSE Pi Pro K1
# over U-Boot loadx + go, with the twenty-six acceptance requirements every
# active-scan board run has used.  Nothing here is persistent: the payload is
# loaded into RAM and started, there is no saveenv and no eMMC, SPI or eFuse
# write.
#
# The board has no automatic reset path in this setup, so the listener prints
# a prompt and waits: press RST on the board after that prompt appears.
#
# Usage: run_k1_scanofld_active.sh [extra run_k1_wireless_smoke.py options]

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
PACKAGE_DIR="${K1_PACKAGE_DIR:-${WORKSPACE_ROOT}/out/k1-scanofld-active}"

exec python3 "${SCRIPT_DIR}/run_k1_wireless_smoke.py" \
  --device auto --manual-reset --gzip-payload --boot-timeout 300 \
  --no-voice-prompt \
  --payload "${PACKAGE_DIR}/contest-nuttx-flat.bin" \
  --wrapper "${PACKAGE_DIR}/k1-go-wrapper.bin" \
  --require-dle-scc --require-hci-flow-control --require-firmware-layout \
  --require-firmware-mss-legacy-signature --require-firmware-full-download \
  --require-firmware-runtime --require-runtime-transport \
  --require-runtime-h2c-loopback --require-runtime-mac-core \
  --require-runtime-bb-rf --require-runtime-phy-cr --require-runtime-bb-reset \
  --require-rf-context --require-runtime-rf-cr --require-runtime-control-plane \
  --require-runtime-address-cam --require-runtime-role-cam-done-ack \
  --require-runtime-scanofld-channel-done-ack \
  --require-runtime-scanofld-passive \
  --require-runtime-scanofld-rx --require-scan-rf-readback \
  --require-scan-phy-counters --require-runtime-data-tx-descriptor \
  --require-runtime-tx-security \
  --require-bringup-success --require-wlan0-scan \
  --require-runtime-scanofld-active \
  "$@"
