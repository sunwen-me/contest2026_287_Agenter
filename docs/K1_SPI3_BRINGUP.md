# K1 MUSE Pi Pro SPI3 Loopback

## Scope

The initial K1 SPI lower-half targets the MUSE Pi Pro 40-pin header's SSP3
controller. It is not the on-board QSPI controller and never accesses the
boot flash used by FSBL, U-Boot, or the operating system.

| Item | Value |
| --- | --- |
| Controller | SSP3 / SPI3 |
| Controller base | `0xd401c000` |
| APBC clock/reset | `0xd401507c` |
| Header SCLK | Pin 23, GPIO75 |
| Header CS0 / FRM | Pin 24, GPIO76 |
| Header MOSI | Pin 19, GPIO77 |
| Header MISO | Pin 21, GPIO78 |
| Pin mux | mode 2, 3.3 V external IO domain |
| Initial test rate | 800 kHz |

The signal mapping is cross-checked against Linux mainline `k1.dtsi`,
`k1-pinctrl.dtsi`, and `drivers/spi/spi-spacemit-k1.c`, plus the vendor
U-Boot SSP3 driver and K1 MUSE Pi Pro DTS. The GPIO2 IO-power domain is
explicitly switched to 3.3 V before its pins are muxed. This is a required
property of GPIO75 through GPIO78, not an application-selectable voltage.
All four pins use K1 3.3 V drive code DS4 (19 mA), matching both the Linux
`drive-strength = <19>` pinctrl state and the vendor U-Boot SSP3 state.

## Implementation

`chip/k1/k1_spi.c` implements a polling NuttX `spi_dev_s` lower-half for bus
3. It supports master mode, one hardware chip select (the `FRM` pin), modes
0 through 3, and 8-bit words. Transfers service one TX and one RX FIFO entry
at a time, so a long exchange cannot overrun the 32-entry RX FIFO. TX-ready,
RX-ready, error, and idle waits have bounded timeouts; failure disables the
controller and the next select resets it.

The only clock generation mechanism on this SSP is the APBC parent mux. The
driver can choose one of 0.8, 1.6, 3.2, 6.4, 12.8, 25.6, or 51.2 MHz, selecting
the fastest rate that does not exceed a request when possible. The dedicated
bring-up configuration fixes the `spi` tool default to the conservative
800 kHz rate. DMA, PLIC, and all SSP interrupt-enable bits remain unused.

`/dev/spi3` is registered only with both `CONFIG_K1_SPI3` and
`CONFIG_SPI_DRIVER`. The latter is enabled only by the `spi3_loopback`
configuration; the normal `nsh` configuration does not register an SPI
character device or alter SPI3 pins.

## Build

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/spi3_loopback \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_spi3_loopback \
  --jobs 8
```

## Hardware Check

With board power disconnected, connect exactly one short 3.3 V-compatible
jumper wire:

```text
40Pin Pin 19 (MOSI, GPIO77) <-> 40Pin Pin 21 (MISO, GPIO78)
```

Do not connect either end to Pin 23 or Pin 24. SCLK (Pin 23) and CS0/FRM
(Pin 24) are driven by SSP3. Do not connect, probe, read, write, or erase the
separate on-board QSPI flash.

Boot the RAM-only SPI image using the established safe U-Boot flow. At the
NSH prompt, run these commands only:

```text
spi bus
spi exch a55a3cc3
```

`spi bus` must report bus 3 present. With the MOSI/MISO wire in place, the
second command must report the same four bytes under `Received` as under
`Sending`: `A5 5A 3C C3`. The command clocks data only between the two header
pins and does not address any storage device. The loopback configuration
fixes bus 3, 800 kHz, mode 0, 8-bit words, and four words as the `spi` tool
defaults, so the test command remains within NSH's argument limit.

Power off before removing the loopback wire. As with every RAM-only K1 test,
NSH `reboot` returns the board to the normal U-Boot/Linux boot chain. Use RST
only if the serial console is no longer responsive.

## Verification State

The dedicated configuration built cleanly and passed real-board loopback on
2026-08-14. Its ELF is
`cmake_out/muse_pi_pro_spi3_loopback/nuttx` with SHA-256
`bc9588e6ea998218c3ff048e047d3c38913bb110a8d025916a8aac1af918e8d4`.
The build configuration enables `CONFIG_K1_SPI`, `CONFIG_K1_SPI3`,
`CONFIG_SPI_DRIVER`, `CONFIG_SPI_EXCHANGE`, and `CONFIG_SYSTEM_SPITOOL`; the
ELF contains `k1_spibus_initialize` and `k1_spi_exchange_words`. It also sets
`CONFIG_DEFAULT_TASK_STACKSIZE=4096`, `CONFIG_SPITOOL_STACKSIZE=4096`, and
`CONFIG_NSH_MAXARGUMENTS=16` for the SPI test utility.

The real board reported `Bus 3: YES`, then completed two independent
`A5 5A 3C C3` loopback exchanges with matching received bytes and a subsequent
successful `uptime` command. The immutable test record is
[`K1_SPI3_REAL_BOARD_20260814.md`](K1_SPI3_REAL_BOARD_20260814.md).
