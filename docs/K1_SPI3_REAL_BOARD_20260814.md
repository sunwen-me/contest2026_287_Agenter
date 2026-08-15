# K1 SPI3 Real-Board Verification

## Result

PASS on the SpacemiT K1 MUSE Pi Pro real board, 2026-08-14.

The test used the RAM-only package:

```text
/home/sw/Dev/k1-workspace/out/k1-spi3-loopback-stack4096
```

The final ELF SHA-256 and board-side payload hash were checked before boot:

```text
ELF:          bc9588e6ea998218c3ff048e047d3c38913bb110a8d025916a8aac1af918e8d4
flat payload: 7ea19b848c543cf6ebfcd817ab5608cc6d66a4496d35b8324562f5238df23e8a
wrapper:      4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81
```

## Hardware Boundary

Exactly one short jumper was fitted on the 40-pin header:

```text
Pin 19 (MOSI, GPIO77) <-> Pin 21 (MISO, GPIO78)
```

These are adjacent pins on the odd-numbered column. Counting from the bottom
of that column, Pin 21 is the 10th pin and Pin 19 is the 11th pin. Pin 23
(SCLK), Pin 24 (CS0/FRM), and the board QSPI flash were not connected or
accessed.

## Boot Method

U-Boot watchdogs were stopped for the RAM-only session:

```text
wdt dev PMIC_WDT
wdt stop
wdt dev watchdog@D4080000
wdt stop
```

The wrapper and flat payload were loaded from the existing bootfs partition
`mmc 2:5` at `0x12000000` and `0x11000000`, then started with
`go 0x12000000`. No `saveenv`, `mmc write`, FDL flashing, or persistent boot
configuration change was performed.

## Serial Evidence

The commands were run through the USB-TTL console at 115200 8N1:

```text
nsh> spi bus
 BUS   EXISTS?
Bus 3: YES
nsh>

nsh> spi exch a55a3cc3
Sending:  A5 5A 3C C3
Received: A5 5A 3C C3
nsh>

nsh> spi exch a55a3cc3
Sending:  A5 5A 3C C3
Received: A5 5A 3C C3
nsh>

nsh> uptime
00:02:56 up  0:02, load average: 0.00, 0.00, 0.00
nsh>
```

Both exchanges returned to `nsh>` with every received byte matching the
transmitted byte. The subsequent shell command also completed normally.

## Corrected Test Profile

The initial SPI image inherited `CONFIG_NSH_MAXARGUMENTS=7` and a 2048-byte
SPI tool stack. A long manual command was therefore truncated before it could
reach the driver, then its error path faulted. That failure is not a SPI bus
transfer result.

The accepted profile sets `CONFIG_DEFAULT_TASK_STACKSIZE=4096`,
`CONFIG_SPITOOL_STACKSIZE=4096`, and `CONFIG_NSH_MAXARGUMENTS=16`. It fixes
bus 3, 800 kHz, mode 0, 8-bit words, and four words in defconfig, so the
real-board command is simply `spi exch a55a3cc3`.
