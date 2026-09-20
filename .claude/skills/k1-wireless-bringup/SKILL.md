---
name: k1-wireless-bringup
description: Reproduce and extend the K1 MUSE Pi Pro RTL8852BS2 wireless bring-up with RAM-only board tests, evidence gates, and source/license checks.
---

# K1 Wireless Bring-up

Use this skill when working on the SpacemiT K1 MUSE Pi Pro RTL8852BS2 Wi-Fi or Bluetooth
port in this repository.

## Safety Boundary

- Keep board tests RAM-only: use U-Boot `loadx + go` or the existing smoke wrappers.
- Do not run `saveenv`, eMMC/SPI writes, eFuse writes, FDL, or fastboot writes.
- Do not place Wi-Fi passwords, generated WPA images, or vendor firmware with unclear
  redistribution terms in Git.
- Do not claim Wi-Fi networking from a scan-only or diagnostic profile. Require a log
  line and a corresponding `--require-*` gate for every hardware claim.

## Source of Truth

Read these before changing the wireless path:

- `docs/K1_WIRELESS_BRINGUP.md`
- `docs/K1_REAL_BOARD_HANDOFF.md`
- `docs/K1_SOURCE_AND_LICENSES.md`
- `chip/k1/k1_rtl8852bs_gpl.c`
- `chip/k1/k1_sdio.c`
- `board/k1/muse_pi_pro/src/k1_wireless.c`

The Linux comparison source is SpacemiT Linux 6.6 revision
`31c449aeaad8c7759bc983ca0e26946e5b6746dc`. Prefer single-file source retrieval and
remove temporary copies after inspection; do not clone a complete Linux tree into `/tmp`.

## Verification Order

1. Run `tools/check_k1_sources.sh` and `git diff --check`.
2. Build the smallest relevant profile with `tools/build_k1.sh`.
3. Run the matching `tools/run_k1_wireless_smoke.py` or wrapper.
4. Check the full serial log for explicit `PASS` and absence of `FAIL`.
5. Update `docs/K1_WIRELESS_BRINGUP.md` with the exact image, log, commit, and SHA-256.

For Wi-Fi, treat these as separate gates: SDIO enumeration, firmware/WCPU, MAC/BB/RF
initialization, real 802.11 RX, management exchange, WPA keys, protected data, DHCP, and
IP echo. Passing one gate does not imply the next gate passed.

## Board Facts

- SDH1 Wi-Fi: `0xd4280800`, GPIO15--20, 4-bit 1.8 V SDIO.
- Bluetooth UART2: GPIO21 TX, GPIO22 RX, GPIO23 CTS, GPIO24 RTS.
- RF power: GPIO67; WLAN REG_ON: GPIO116; WLAN wake: GPIO66 input; BT reset: GPIO63.
- Use the serial console at 115200 8N1. The USB-TTL adapter supplies signal and ground
  only; do not feed its VCC into the board.

## Reporting Rule

When a test is unavailable or fails, record the blocker and the exact log path. Never
replace a missing hardware result with an empty `wlan0`, synthetic scan result, or a
host-only success.
