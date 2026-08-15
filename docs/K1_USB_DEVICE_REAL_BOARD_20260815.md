# K1 Runtime USB Device Real-Board Record

## Conclusion

The K1 legacy MV/ChipIdea USB Device Controller (UDC) implementation builds
and boots as a RAM-only NuttX image, but it is **not accepted as a working
runtime USB Device path on the MUSE Pi Pro PWR Type-C connector**.

The reason is a board-routing and boot-mode boundary, not a Fastboot class
registration failure. The official MUSE Pi Pro guide describes PWR Type-C as a
USB Device endpoint for BootROM/FDL flashing. In its normal boot state, that
connector is not specified as a general runtime gadget port. The vendor U-Boot
K1 base DTS also disables the legacy UDC node at `0xc0900100`; the MUSE Pi Pro
DTS does not enable it.

Do not use this result to claim NuttX Fastboot, ADB, or CDC-ACM enumeration
support on the PWR Type-C connector. The separate BootROM download mode was
identified below, but its write protocol and image layout remain outside this
project's implementation.

## Tested Image

The final RAM-only test image is `usb_fastboot_8m_fix7`:

| Item | Value |
|---|---|
| ELF | `cmake_out/muse_pi_pro_usb_fastboot_8m_fix7/nuttx` |
| ELF SHA-256 | `383e52837d7511703ef382ef07c0e864dff8e380729dc252e933b82e04e51b5b` |
| Flat payload | `out/k1-usb-fastboot-8m-fix7/contest-nuttx-flat.bin` |
| Flat SHA-256 | `d04f7c331cb294d895577874c708224a83fc53e601939f13b4e43b699450359b` |
| Persistent writes | None; U-Boot `ext4load` plus `go` only |

`fix7` keeps the endpoint-prime 100 ms timeout protection and carries over the
literal physical-interface setup from the vendor U-Boot gadget driver:

- UDC USB2 PHY bring-up at `0xc0940000`;
- `PORTSC = PTS(ULPI) | PFSC` before device mode is selected;
- `USBCMD.ITC = 8` before Run/Stop asserts the software connect;
- connect-time command, mode, port, and PHY state logging.

The implementation is in `chip/k1/k1_usbdev.c` and
`chip/k1/hardware/k1_usbdev.h`.

## Real-Board Procedure and Result

1. Boot the existing image through the established U-Boot RAM handoff.
2. Stop both U-Boot watchdogs before `go 0x12000000`.
3. Confirm that NuttX reaches `nsh>`.
4. Run `fastbootd &`.
5. Check the host with `lsusb`; do not run a flashing command.

NuttX stayed alive after both watchdogs were stopped. `fastbootd` was created,
but reported:

```text
fastbootd [4:100]
open [/dev/fastboot/ep2] error 2
```

No K1 device appeared in the host `lsusb` listing. Without a host USB reset
and `SET_CONFIGURATION`, the class driver cannot configure endpoint 2, so the
missing `/dev/fastboot/ep2` node is a downstream symptom. It does not prove
that Fastboot registration itself failed.

The captured console records are
`out/k1-serial/k1-usb-fastboot-20260815T150226Z.log` and
`out/k1-serial/k1-usb-fastboot-20260815T151218Z.log`.

## Scope Boundary

The following are three different transports and must remain separate:

| Transport | Status on MUSE Pi Pro |
|---|---|
| NuttX runtime legacy UDC at `0xc0900100` | Builds and reaches NSH; normal PWR Type-C enumeration not accepted |
| NuttX Fastboot protocol | Class and host tooling build; cannot be exercised without runtime UDC enumeration |
| BootROM/Titan FDL | Not implemented; VID/PID and protocol not yet recorded |

The validated USB work continues through the DWC3/xHCI Host path on the USB-A
ports. It has separate real-board Hub, UAS, and `/dev/sda` evidence in
`K1_USB_DISPLAY_BRINGUP.md`.

## Only Safe Next Device-Side Check

To identify the actual BootROM/FDL transport, hold **FDL**, press **RST** once,
then release FDL after the reset is accepted. The host may then run only
`lsusb` and record the resulting VID:PID. Do not run Titan, Fastboot, erase,
flash, `saveenv`, or any raw eMMC command in that mode.

This check verifies BootROM identity only. It does not validate, replace, or
use the NuttX runtime UDC path.

## BootROM Download-Mode Evidence

The FDL button sequence was completed on the real board on 2026-08-15. The
host observed:

```text
Bus 001 Device 018: ID 361c:1001 DFU USB download gadget
Negotiated speed: High Speed (480Mbps)
```

The USB descriptor identifies a vendor-specific bulk transport, not the USB
DFU class:

| Field | Value |
|---|---|
| VID:PID | `361c:1001` |
| Device class | `ff/00/00` vendor-specific |
| Interface | `ff/42/03`, `DFU download` |
| Bulk OUT | `0x02`, 512-byte packets |
| Bulk IN | `0x81`, 512-byte packets |
| Serial | `dfu-device` |

The official Android Platform Tools `fastboot` 37.0.1 recognizes the device
as `dfu-device DFU download`. Read-only queries returned:

```text
fastboot devices
dfu-device\t DFU download

fastboot getvar version
version: 0.4
```

`getvar all`, `getvar product`, `getvar unlocked`, and
`getvar current-slot` returned `Variable not implemented`; this is a vendor
download mode with a limited Fastboot command set. No image, partition, or
erase command was sent.

The official archive entry
`https://archive.spacemit.com/image/k1/flash-all.zip` was downloaded only for
inspection. Its SHA-256 is
`6b2a1c636e8ee3b62eab96e25450ee36177dd0b63f67fcaa877ad5e6ea5b7604`. The
contained scripts issue writes to `gpt`, `bootinfo`, `fsbl`, `env`, `opensbi`,
`uboot`, `bootfs`, and `rootfs`; they were not executed.
