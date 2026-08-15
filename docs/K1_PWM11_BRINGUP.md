# K1 MUSE Pi Pro PWM11

## Scope

The initial K1 PWM lower-half exposes one PXA-style PWM channel for a safe
40-pin header measurement. It does not access QSPI, SD/eMMC, Ethernet, the
debug UART, or a board-mounted peripheral.

| Item | Value |
| --- | --- |
| Controller | PWM11 |
| Controller base | `0xd4020c00` |
| APBC clock/reset | `0xd40150c4` |
| Controller clock | PLL1/192, 12.8 MHz |
| SoC pad | GPIO41, mux mode 4 |
| Header output | Pin 3 |
| Header voltage | 3.3 V through the board level shifter |
| Device | `/dev/pwm11` |
| First check | 1 kHz, 50%, five seconds |

The controller address and PXA register format come from the K1 vendor Linux
and U-Boot PWM drivers. The APBC reset sequence and 12.8 MHz parent use the
vendor clock and reset drivers. The GPIO41 mux assignment comes from the
vendor K1 pinctrl DTS, where `pinctrl_pwm11_1` selects mux mode 4. The MUSE Pi
Pro GPIO table maps GPIO41 to 40-pin Pin 3 through the board's level shifter.

## Implementation

`chip/k1/k1_pwm.c` provides the NuttX `pwm_lowerhalf_s` interface. It supports
one channel, normal polarity, 16-bit NuttX duty values, and frequencies from
approximately 196 Hz to 12.8 MHz. The period is calculated as:

```text
period = 12,800,000 / ((prescale + 1) * frequency)
```

`prescale` is selected in the range 0 through 63 so the hardware period stays
within its 1024-cycle counter. DMA, PLIC, PWM interrupts, inverse polarity,
fixed-point frequency, multichannel output, and fixed-pulse-count generation
are deliberately not enabled.

On stop or device close, the driver writes a zero duty, gates the PWM clock,
and returns GPIO41 to a pulled-up GPIO input. It enables PWM mux mode only
after the clock and PWM registers have been programmed.

`/dev/pwm11` is registered only when both `CONFIG_K1_PWM11` and `CONFIG_PWM`
are enabled. The normal `nsh` configuration does not select K1 PWM and does
not change Pin 3.

## Build

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/build_k1.sh \
  --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/pwm11 \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_pwm11 \
  --jobs 8
```

The clean host build completed on 2026-08-14 and passed
`tools/check_k1_elf.sh`. Its ELF is:

```text
SHA256 5026922d26337e53384d302d36af865f0e9c58bc28be5c6556dcc28d205f3bb8
```

## Hardware Check

Do not connect any external device to Pin 3 or Pin 5 for this test. They are
the header pins that can be used as I2C4, and this image changes Pin 3 to PWM.
Do not run a GPIO command against `/dev/gpio1` while `/dev/pwm11` is active:
both names refer to GPIO41/Pin 3 under different pinmux functions.

With the board running the RAM-only PWM image, measure Pin 3 relative to a
header ground such as Pin 6. Use an oscilloscope or logic analyzer rated for
3.3 V signals. A multimeter only reports an average voltage and cannot verify
frequency or waveform edges.

At the NSH prompt run:

```text
ls /dev/pwm11
pwm -p /dev/pwm11 -f 1000 -d 50 -t 5
```

The command must report a 1000 Hz output and stop after five seconds. The
expected waveform is 3.3 V logic, approximately 1 ms period, approximately
500 us high and 500 us low. After the command stops, the output is released;
do not treat its idle voltage as a guaranteed high or low level.

The `pwm` command accepts `-f` in Hz, `-d` in percent, and `-t` in seconds.
For the first board test keep frequency between 500 Hz and 10 kHz and duty
between 10% and 90%. These values leave large timing margins and make a scope
measurement unambiguous. Do not use a motor, servo, relay, LED strip, or
other load as the first test. The header pin is a logic output, not a power
rail or motor driver.

As with other RAM-only K1 tests, `reboot` returns the board to the normal
U-Boot/Linux boot chain. Use RST only if the serial console is no longer
responsive.

## Verification State

The clean host build is complete. The PWM must not be described as real-board
verified until a serial capture contains the command above and a scope or
analyzer record confirms the Pin 3 waveform.

## Deferred Hardware Verification

As of 2026-08-14, the real-board PWM11 check is intentionally deferred because
no oscilloscope or logic analyzer is currently available. No PWM11 real-board
PASS is claimed, and the Pin 3 waveform remains unverified. Resume with the
Hardware Check section when a 3.3 V-capable measuring instrument is available;
the delivery checklist must remain unchecked until the frequency and duty
cycle are measured.

## Sources

- K1 Linux `arch/riscv/boot/dts/spacemit/k1-x.dtsi` and
  `drivers/pwm/pwm-pxa.c`;
- K1 vendor U-Boot `arch/riscv/dts/k1-x.dtsi`, `k1-x_pinctrl.dtsi`,
  `drivers/pwm/pwm-pxa.c`, `drivers/clk/spacemit/ccu-k1x.c`, and
  `drivers/reset/reset-spacemit-k1x.c`;
- MUSE Pi Pro expansion GPIO mapping and board schematic already recorded in
  `docs/K1_GPIO_BRINGUP.md`.
