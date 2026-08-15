# K1 eMMC Bring-Up

## Current status

The K1 SDH2 eMMC lower-half is implemented as a read-only, 1-bit, polling,
PIO SDIO device and is registered by the MUSE Pi Pro board late-init path as
`/dev/mmcsd0`. Host build, ELF validation, card initialization, and direct
read-only block access have now been validated on the real MUSE Pi Pro through
U-Boot. Filesystem mount and write support remain intentionally unverified.

The first real-board attempt reached NuttX and then corrupted the
`AppBringUp` task stack before NSH became available. The eMMC profile used
the inherited `CONFIG_BOARD_INITTHREAD_STACKSIZE=2048`; the corrected profile
sets it to 8192 bytes. This is consistent with the earlier I2C/SPI bring-up
stack failures and is required for the deeper `mmcsd_slotinitialize()` path.

## Real-board result (2026-08-14)

The power-diagnostic package below was copied to the existing bootfs with ADB,
verified by SHA256 on the board, and loaded with the read-only U-Boot
`ext4load` plus wrapper `go` path. No `saveenv`, raw eMMC write, or erase was
performed.

```text
Package: /home/sw/Dev/k1-workspace/out/k1-emmc-powerdiag
Flat:    ad5a46a7d349c776d3f8c6ed3ca61585fb3a8200d45c59ff71f6325607234521
Wrapper: 4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81
```

Observed result:

```text
K1 eMMC: mmcsd_slotinitialize ret=0x0000000000000000
/dev/mmcsd0
/dev/mmcsd0boot0
/dev/mmcsd0boot1
/dev/mmcsd0rpmb
```

The diagnostic snapshot showed SDH2 clock state `CLOCK=0x0000000000001007`
after CMD1 and `POWER=0x000000000000000b`; the earlier image stopped at CMD1
timeout and did not create any `mmcsd` nodes. This confirms controller/card
initialization and device registration. A historical minimal `dd` read then
returned NuttX error 6; that failure was the data-command timeout fixed below.

## Real-board block-read result (2026-08-14)

The corrected package was copied to the existing bootfs with ADB, verified by
SHA256 on the board, and loaded through the read-only U-Boot `ext4load` plus
wrapper `go` path. No raw eMMC write, erase, filesystem mount, or `saveenv`
was performed.

```text
Package: /home/sw/Dev/k1-workspace/out/k1-emmc-multiblock-20260814
ELF:     93f5c4c85e58976c312e4f01e74de3e8355cc7d96ce70993c5bed744245e5747
Flat:    b3b0ee7efc01b6f90dcac7a05b35a866adecba412211c814bbf8232a3bef3446
Wrapper: 4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81
```

The SDHCI data command now programs `Timeout Control (0x2e)` to `0x0e` and
enables Block Count for every data transfer, matching the vendor U-Boot
reference. The read-only eMMC profile now sets
`CONFIG_MMCSD_MULTIBLOCK_LIMIT=2`, so a single 1024-byte block-device read
exercises the bounded `CMD23` + `CMD18` path. The real board reported
`data cmd=0x2452` (CMD18 with `MMCSD_MULTIBLOCK`) and `data mode=0x32`, then
completed the transfers:

```text
dd if=/dev/mmcsd0 of=/dev/null bs=1024 count=1; echo dd1024=$?
dd1024=0
dd if=/dev/mmcsd0 of=/dev/null bs=1024 count=2; echo dd2x1024=$?
dd2x1024=0
```

`bs=512 count=2` is still a valid two-sector read, but the NSH `dd`
implementation issues two 512-byte reads in that form and therefore produces
two CMD17 traces. Use `bs=1024` when checking that the multi-block controller
path is actually selected.

## Corrected package

```text
/home/sw/Dev/k1-workspace/out/k1-bringup-emmc-stack8192
```

| Item | SHA256 |
|---|---|
| ELF | `38119bdd94a661ad88d7e40a8bc9f27293e0278d112a6daffdf4326bad47ef6a` |
| Flat payload | `fee1b4956b53e5ef6a555d44220899627a8ba83acc372ddee32bdbe664d7fcca` |
| Wrapper | `4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81` |

## Board procedure

Use the existing USB-TTL session at 115200 8N1. Stop both U-Boot watchdogs,
load the two files read-only from `mmc 2:5`, and start with the wrapper:

```text
wdt dev PMIC_WDT
wdt stop
wdt dev watchdog@D4080000
wdt stop
ext4load mmc 2:5 0x12000000 /musepi/k1-go-wrapper.bin
ext4load mmc 2:5 0x11000000 /musepi/contest-nuttx-flat.bin
go 0x12000000
```

Expected early markers are:

```text
K1 eMMC: sdio_initialize start
K1 eMMC: sdio_initialize done
K1 eMMC: mmcsd_slotinitialize start
K1 eMMC: mmcsd_slotinitialize done
NuttShell (NSH)
```

After NSH appears, the first read-only checks are:

```text
ls /dev/mmcsd0
ls /dev
dd if=/dev/mmcsd0 of=/dev/null bs=512 count=1; echo dd1=$?
dd if=/dev/mmcsd0 of=/dev/null bs=512 count=2; echo dd2=$?
dd if=/dev/mmcsd0 of=/dev/null bs=1024 count=1; echo dd1024=$?
```

For the last command, confirm the serial trace contains `data cmd=0x2452`
and `data mode=0x32`. Do not treat the two CMD17 traces from the preceding
512-byte command as multi-block evidence.

Do not run `saveenv`, `mmc write`, `mmc erase`, or any filesystem mount that
could write to the eMMC during first bring-up.

## Failure interpretation

| Last marker | Interpretation |
|---|---|
| `sdio_initialize start` | SDH2 clock/reset or register access issue |
| `sdio_initialize done` | SDHCI reset returned; inspect card command/response path |
| `mmcsd_slotinitialize start` | Card identification or polling/event path issue |
| `mmcsd_slotinitialize done` without NSH | Registration, stack, or memory corruption issue |
