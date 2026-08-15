# K1 I2C2 Real-Board Verification

## Result

PASS on the SpacemiT K1 MUSE Pi Pro real board, 2026-08-14.

The test used the RAM-only package:

```text
/home/sw/Dev/k1-workspace/out/k1-i2c-bringup-stack4096
```

The package contains the 4096-byte-stack I2C profile. The board-side payload
and wrapper hashes were checked before boot:

```text
flat payload: 5c97dcb0178aed78d44aaee1b9c4bf588f3a4404d4f07daafe34592829e241a5
wrapper:      4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81
```

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
nsh> i2c bus
 BUS   EXISTS?
Bus 2: YES
nsh>

nsh> i2c get -b 2 -a 0x50 -r 0 -w 8 -f 100000
READ Bus: 2 Addr: 50 Subaddr: 00 Value: 54
nsh>

nsh> i2c get -b 2 -a 0x50 -r 0 -w 8 -f 100000
READ Bus: 2 Addr: 50 Subaddr: 00 Value: 54
nsh>

nsh> uptime
00:40:06 up  0:40, load average: 0.00, 0.00, 0.00
nsh>
```

The first byte read is `0x54`. Both transfers returned to `nsh>` and the
subsequent shell command ran normally.

## Previous Failure And Fix

The earlier image used the inherited 2048-byte default stack for the `i2c`
builtin. Its transfer also read `0x54`, but the task corrupted its stack while
returning from the command and faulted. The I2C profile now sets
`CONFIG_DEFAULT_TASK_STACKSIZE=4096`; the generated registry entry is:

```text
{ "i2c", SCHED_PRIORITY_DEFAULT, 4096, i2c_main },
```

The corrected image completed the real-board acceptance above without the
exception.
