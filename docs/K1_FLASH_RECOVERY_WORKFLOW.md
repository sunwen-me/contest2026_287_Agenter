# K1 Flash and Recovery Workflow

This document records the safe boundary for persistent flashing on the Muse Pi
Pro. The repository contains a read-only manifest preflight, a separately
selectable NuttX Fastboot write profile, and a guarded host-side write/reboot
verification tool. The write profile is not the vendor BootROM/Titan FDL
implementation.

## Device roles

- The USB-TTL adapter is a serial console only. Connect GND, board TX, and
  board RX; do not connect TTL VCC.
- The PWR Type-C connector is the official K1 power and BootROM/FDL path.
- The NuttX USB Device controller at `0xc0900100` is a runtime USB gadget
  controller. It is not the BootROM FDL controller.
- NuttX Fastboot is a separate application protocol. Its compatibility with
  the vendor Titan/FDL protocol is not assumed.

## Runtime USB Device Result

The 2026-08-15 RAM-only `usb_fastboot_8m_fix7` image reached `nsh>` and kept
running after both U-Boot watchdogs were stopped, but the PWR Type-C connector
did not enumerate as a host USB device. `fastbootd &` therefore could not open
`/dev/fastboot/ep2`, because no host `SET_CONFIGURATION` arrived. This is not
accepted as a runtime Fastboot, CDC-ACM, ADB, or USB Device result.

The official board guide assigns the PWR Type-C USB Device role to BootROM/FDL
mode. Treat the normal-boot PWR Type-C connector as unavailable for NuttX USB
Device until a board-specific physical route is documented and demonstrated.
The full record and artifact hashes are in
[`K1_USB_DEVICE_REAL_BOARD_20260815.md`](K1_USB_DEVICE_REAL_BOARD_20260815.md).

## Manifest

Create a JSON file with absolute paths and verified hashes. A write manifest
must also identify the expected Fastboot firmware and post-reboot serial
markers:

```json
{
  "board": "muse_pi_pro",
  "image": "/absolute/path/image.bin",
  "image_sha256": "...64 hex characters...",
  "protocol": "fastboot",
  "target_partition": "mmcsd0",
  "recovery_image": "/absolute/path/recovery.bin",
  "recovery_sha256": "...64 hex characters...",
  "write_enabled": true,
  "expected_getvar": {"product": "NuttX"},
  "serial_markers": ["NuttShell (NSH)"]
}
```

Run the read-only preflight:

```bash
python3 tools/k1_flash_manifest_check.py /absolute/path/manifest.json
```

The preflight tool checks file existence, absolute paths, SHA-256 values, board
name, protocol, target partition, and the write-disabled invariant. It never
calls `fastboot`, Titan, FDL, `mmc write`, `mmc erase`, or any other write
command. Use `write_enabled: false` for that read-only preflight manifest.
For the write manifest shown above, the same check remains read-only and must
be invoked as:

```bash
python3 tools/k1_flash_manifest_check.py \
  --allow-write-manifest /absolute/path/write-manifest.json
```

This preflight is intentionally required before using the writable profile;
it does not authorize a write by itself.

## NuttX Fastboot profile

Two profiles are available:

- `usb_fastboot`: K1 USB Device plus NuttX `fastbootd`, without eMMC writes.
  It is retained as an experimental build profile; it must not be used for
  enumeration or `getvar` validation on the normal-boot PWR Type-C connector.
- `usb_fastboot_emmc`: adds the writable K1 eMMC block path. Fastboot maps a
  target name to `/dev/<target>`, so the target must be an explicitly verified
  node such as `/dev/mmcsd0boot0`; the profile does not invent a partition
  table or select an offset.

Build the writable validation image with:

```bash
mkdir -p cmake_out/k1-tmp-usb-fastboot-emmc
TMPDIR="$PWD/cmake_out/k1-tmp-usb-fastboot-emmc" \
  tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/usb_fastboot_emmc \
  --build-dir cmake_out/muse_pi_pro_usb_fastboot_emmc
```

Only after a physical runtime USB Device path is independently demonstrated,
boot this image through the established RAM-only/U-Boot handoff and start the
daemon from NSH:

```text
fastbootd &
```

On the host, perform only discovery first:

```bash
fastboot devices
fastboot getvar all
```

For a manifest whose target, image layout, recovery image, and expected
post-reboot serial markers have already been independently confirmed, run the
guarded closed loop. The first command only probes; it does not write:

```bash
python3 tools/k1_fastboot_flash.py /absolute/path/write-manifest.json --probe
```

The write command requires both `--execute` and the exact confirmation token.
It verifies the image and recovery hashes, requires one Fastboot device and
the manifest's exact `getvar` values, starts USB-TTL capture before reboot,
flashes the explicit `/dev/<target_partition>` child, requests reboot, and
checks every `serial_markers` entry:

```bash
python3 tools/k1_fastboot_flash.py /absolute/path/write-manifest.json \
  --execute --confirm K1-FASTBOOT-WRITE \
  --serial-device /dev/ttyUSB0
```

The tool writes a JSON evidence record and a raw serial log under `out/` by
default. It never infers partitions or offsets. NuttX Fastboot writes from
offset zero, so the image must be verified for that exact target and must not
be an arbitrary Linux or vendor container.

Do not issue `fastboot flash` or `fastboot erase` until the exact eMMC node,
image format, partition offset, and independent recovery image have been
recorded. NuttX Fastboot writes the selected `/dev/<target>` from offset zero;
it is not a partition-aware replacement for Titan/FDL.

## Required evidence before a write path

Do not turn this into an automatic flashing command until all of these are
known and recorded:

1. The actual BootROM/FDL VID and PID observed on the PWR Type-C port.
2. The exact host utility and protocol revision used by the vendor image.
3. The image container format and signing requirements.
4. The eMMC partition table and the intended partition offset/size.
5. A recovery image that boots independently, with a verified SHA-256.
6. A serial-console recovery test after a deliberate power-cycle.

Until then, use USB-TTL for logs. Do not infer NuttX runtime USB Device support
from the FDL device, from a successful CDC or Fastboot enumeration, or from
`adb reboot` entering the normal system. The FDL read-only identity check is
complete: the board reports `361c:1001`, interface `ff/42/03`, and Fastboot
version `0.4`. The write protocol, image container and partition map are still
not independently verified.

## Current implementation status

- K1 MV/ChipIdea USB Device DCD: builds in the `usb_device` and
  `usb_fastboot` profiles; the final `fix7` RAM-only image reaches NSH, but
  normal-boot PWR Type-C enumeration is not accepted.
- CDC-ACM configuration: registered during board late initialization in the
  `usb_device` profile.
- NuttX Fastboot transport: builds in `usb_fastboot` and has a writable eMMC
  variant in `usb_fastboot_emmc`; neither is authorized for real-board writes,
  and neither has a working normal-boot PWR Type-C transport.
- FDL/Titan BootROM transport: the real-board device identity is recorded as
  `361c:1001` with Fastboot version `0.4`; write commands and image layout are
  not implemented or verified.
- Host-side guarded Fastboot write/reboot/serial verification: implemented,
  but real-board persistent write/restore is not executed; image layout and
  recovery evidence are still required.
- Manifest preflight: implemented and read-only.
