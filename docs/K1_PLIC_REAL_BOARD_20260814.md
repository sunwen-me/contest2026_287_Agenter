# K1 PLIC Real-Board FDT Verification

## Result

PASS for the SpacemiT K1 MUSE Pi Pro real board, 2026-08-14.

This was a read-only U-Boot inspection of the board's existing DTB. The DTB
was loaded from the existing bootfs partition without changing the boot
environment or any persistent storage.

## U-Boot Commands

```text
ext4load mmc 2:5 0x31000000 /spacemit/6.6.63/k1-x_MUSE-Pi-Pro.dtb
fdt addr 0x31000000
fdt print /soc/interrupt-controller@e0000000
fdt print /soc/gpio@d4019000
```

## FDT Evidence

The PLIC node reported:

```text
compatible = "riscv,plic0"
interrupts-extended = <... 0x0b ... 0x09 ...>
reg = <0x00000000 0xe0000000 0x00000000 0x04000000>
riscv,max-priority = <0x00000007>
riscv,ndev = <0x0000009f>
```

The 16 interrupt-extension entries are eight pairs of machine/supervisor
external interrupts. The first pair is hart 0 `11/9`, so hart 0 S-mode uses
PLIC context 1. `0x9f` is decimal 159 devices.

The GPIO node reported:

```text
compatible = "spacemit,k1x-gpio"
reg = <0x00000000 0xd4019000 0x00000000 0x00000800>
interrupts = <0x0000003a>
interrupt-parent = <0x0000001e>
interrupt-controller;
```

`0x3a` is decimal 58, matching the K1 GPIO PLIC source used by the
experimental GPIO IRQ lower-half. The separate Pin 22 -> Pin 33 rising-edge
callback test is documented in `docs/K1_GPIO_BRINGUP.md`.

## Safety Boundary

The inspection did not run `saveenv`, `mmc write`, FDL flashing, or any
persistent boot configuration operation. `CONFIG_K1_PLIC` remains disabled in
the normal `nsh` profile and is only enabled by the explicit `gpio_irq`
experimental configuration.
