#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Run the RTL8852BS WPA2-PSK four-way handshake diagnostic image on the MUSE Pi
# Pro K1 over U-Boot loadx + go, with the forty acceptance requirements the
# handshake board runs use.  Nothing here is persistent: the payload is loaded
# into RAM and started, there is no saveenv and no eMMC, SPI or eFuse write.
#
# The image comes from tools/build_k1_wpa.sh, so it carries the passphrase in
# its read-only data.  Do not publish out/k1-wpa.  No credential is passed on
# this command line and none is printed by the firmware.
#
# The reset mode defaults to --nsh-reboot, which works when the board is parked
# at nsh>.  Set K1_RESET_MODE=--manual-reset when it is not, and press RST after
# the listener's prompt appears.  The two are an argparse mutually exclusive
# group, which is why this is an environment variable rather than a pass-through
# argument.
#
# Usage: run_k1_wpa.sh [extra run_k1_wireless_smoke.py options]

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
PACKAGE_DIR="${K1_PACKAGE_DIR:-${WORKSPACE_ROOT}/out/k1-wpa}"
RESET_MODE="${K1_RESET_MODE:---nsh-reboot}"

exec python3 "${SCRIPT_DIR}/run_k1_wireless_smoke.py" \
  --device auto "${RESET_MODE}" --gzip-payload --boot-timeout 300 \
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
  --require-runtime-auth --require-runtime-join \
  --require-runtime-assoc --require-runtime-assoc-response \
  --require-runtime-wpa --require-runtime-wpa-msg1 \
  --require-runtime-wpa-mic --require-runtime-wpa-keys \
  --require-runtime-resident --require-runtime-data-secure-tx \
  --require-runtime-arp-probe \
  --require-runtime-ccmp-selftest \
  --require-runtime-loopback-readback \
  "$@"
