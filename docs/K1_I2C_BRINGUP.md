# K1 MUSE Pi Pro I2C2

## Scope

The initial K1 I2C lower-half is a polling driver for MUSE Pi Pro I2C2. It
uses the board's on-board EEPROM, so its first hardware check requires no
40Pin wiring or external module.

| Item | Value |
| --- | --- |
| Controller | I2C2 |
| Controller base | `0xd4012000` |
| APBC clock/reset | `0xd4015038` |
| SCL/SDA | GPIO84 / GPIO85 |
| Pin mux | mode 4, 1.8 V, internal pull-up |
| Board device | Atmel-compatible 24C02 EEPROM |
| EEPROM address | 7-bit `0x50` |

The controller and pin assignments are cross-checked against the MUSE Pi Pro
DTS, the vendor U-Boot I2C driver, and Linux mainline
`drivers/i2c/busses/i2c-k1.c`. GPIO84/85 are not 40Pin GPIOs and must not be
reconfigured through the board GPIO driver.

## Implementation

`chip/k1/k1_i2c.c` implements the NuttX `i2c_master_s` lower-half for bus 2.
It supports 7-bit transactions at 100 kHz and 400 kHz, including a repeated
START after a message with `I2C_M_NOSTOP`. Transfers are mutex-serialized and
use polling only; no PLIC source or I2C interrupt is enabled.

The driver rejects 10-bit addressing, `I2C_M_NOSTART`, empty messages, and
unsupported frequencies. Timeout, arbitration loss, NACK, and bus errors
return standard NuttX errors. A failure resets the controller; if SDA remains
low, the hardware recovery request is issued for up to nine SCL pulses.

The board registers the bus as `/dev/i2c2` only when both `CONFIG_K1_I2C2` and
`CONFIG_I2C_DRIVER` are enabled. An initialization failure is logged and does
not block GPIO, procfs, or Ethernet bring-up.

## Build

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/i2c \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_i2c \
  --jobs 8
```

## Hardware Check

Boot the RAM-only I2C image through the existing safe U-Boot flow. At the NSH
prompt, run only the following read-only checks:

```text
i2c bus
i2c get -b 2 -a 0x50 -r 0 -w 8 -f 100000
```

The second command reads one 8-bit byte from EEPROM offset zero through a
write-offset plus repeated-START read transaction. It does not modify EEPROM.

Do not run `i2c scan`, `i2c set`, `i2c verf`, or any command that writes to
`0x50` or another address during this first validation. The `i2c` profile
restricts the tool address range to `0x50`, but that cannot make a write
command safe.

## Verification State

The first real-board command reached the K1 I2C lower-half on 2026-08-14 and
read EEPROM offset zero as `0x54`. Therefore the controller, I2C2 pinmux,
7-bit address, write-offset/repeated-START sequence, and read data path have
all been exercised on the board.

That run is not an acceptance result: the `i2c` builtin had the inherited
2048-byte default task stack (1840 bytes usable after task bookkeeping). It
corrupted its stack while returning from the command and faulted with an
instruction access exception. The fatal record identifies `i2c_main` as the
task and shows an invalid `sepc=0xffffffffffffffff`. This is an application
stack-size failure after a successful transfer, not an I2C controller error.

The dedicated configuration now sets
`CONFIG_DEFAULT_TASK_STACKSIZE=4096`. A clean build generated this actual
builtin registry entry:

```text
{ "i2c", SCHED_PRIORITY_DEFAULT, 4096, i2c_main },
```

The replacement RAM-only package is
`/home/sw/Dev/k1-workspace/out/k1-i2c-bringup-stack4096`:

```text
ELF SHA256:          a97d02a15fefb98c587e655e9756359beb1bbb319a79cb1a57eba6e11995b6e6
flat payload SHA256: 5c97dcb0178aed78d44aaee1b9c4bf588f3a4404d4f07daafe34592829e241a5
wrapper SHA256:      4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81
```

The replacement was booted on the real board on 2026-08-14. Both the initial
read and an immediate repeated read returned `Value: 54`, and each command
returned to `nsh>`. `uptime` then returned normally. This is the real-board
I2C acceptance result; detailed evidence is in
`docs/K1_I2C_REAL_BOARD_20260814.md`.

The RAM-only test used U-Boot `ext4load` and `go` only. It did not use
`saveenv`, `mmc write`, FDL flashing, or any persistent boot configuration
change.

The verification command has twelve tokens including `i2c`, so the dedicated
configuration sets `CONFIG_NSH_MAXARGUMENTS=12`. The NSH default of seven
rejects the command before it reaches the I2C lower-half; that parser error is
not an EEPROM or controller transfer failure.
