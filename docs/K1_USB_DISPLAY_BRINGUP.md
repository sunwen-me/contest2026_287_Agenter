# K1 USB and Display Bring-Up

## Scope

These are independent MUSE Pi Pro validation profiles. They are intentionally
separate from the default and combined hardware images so a display or USB
failure cannot obscure an already verified console, timer, network, or storage
path.

## Hardware facts

Linux mainline describes the DWC3 node in
[`k1.dtsi`](https://github.com/torvalds/linux/blob/master/arch/riscv/boot/dts/spacemit/k1.dtsi)
and configures it as a host with VBUS and two hubs in
[`k1-musepi-pro.dts`](https://github.com/torvalds/linux/blob/master/arch/riscv/boot/dts/spacemit/k1-musepi-pro.dts).

| Block | Mainline definition |
|---|---|
| DWC3 | `0xc0a00000`, 64 KiB resource, IRQ 125 |
| USB2 PHY | `0xc0a30000`, 512-byte resource |
| USB3 combo PHY | `0xc0b10000`, 4 KiB resource |
| Host-side dependencies | USB30 clock, AHB/VCC/PHY resets, 5 V VBUS GPIO 79, Hub power GPIO 127, Hub reset GPIO 123 |
| Inherited framebuffer | `0x7f700000`, observed from the running U-Boot setup |

The panel handoff data is a U-Boot observation, not a native Linux mainline
display binding. The PWR Type-C port is the FDL USB Device/burn path; its lack
of normal Linux enumeration must not be treated as a DWC3 failure.

## Display profile

Build and package the image from the contest repository:

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/display_fb \
  --build-dir cmake_out/muse_pi_pro_display_fb \
  --package --package-dir out/k1-display-fb
```

The profile registers `/dev/fb0` and includes the NuttX `fb` sample:

| Property | Value |
|---|---:|
| Base | `0x7f700000` |
| Resolution | `800x480` |
| Format assumption | RGB32/XRGB8888 |
| Buffer size | `1,536,000` bytes |
| Update path | `FBIO_UPDATE` cleans the changed cache range |

The framebuffer lower-half deliberately does not write DPU, HDMI, MIPI, or
panel-bridge registers. It relies on U-Boot continuing to scan out the same
buffer. Generic NuttX framebuffer registration clears that buffer first, so a
black panel after the NuttX banner is expected until the test is run.

After the normal RAM-only U-Boot handoff described in
[`K1_REAL_BOARD_HANDOFF.md`](K1_REAL_BOARD_HANDOFF.md), run:

```text
nsh> ls /dev/fb0
nsh> fb
```

The serial console must print the framebuffer geometry followed by `FB test
finished`; the panel must show the stepped colour rectangles. This proves node
registration, framebuffer addressing, CPU cache maintenance, and U-Boot scanout
handoff together. A successful `/dev/fb0` node by itself is not display proof.

The display profile was run on the MUSE Pi Pro on 2026-08-15. The complete
serial log is `out/k1-serial/k1-display-fb-20260815T052336Z.log`; the flat
payload SHA256 is
`e705a55a92b0be240bc6a01367faa72f69f17b7de34c4f68e93aa8adb1a42794`.
The board-side result was:

```text
K1 display: inherited /dev/fb0 registered
crw-rw-rw-           0 /dev/fb0
xres: 800
yres: 480
fbmem: 0x7f700000
fblen: 1536000
stride: 3200
bpp: 32
FB test finished
```

This is a real-board PASS for framebuffer registration, the inherited memory
window, mmap access, cache-clean/update handling, and the NuttX FB sample. It
is not a visual panel PASS: the same U-Boot log reported no HDMI HPD and a
failed MIPI panel probe, so a connected and recognized panel is still needed
for a photograph or direct colour check.

If the image is stable but colours are shifted, stop after saving the serial
log and photo. Rebuild with `CONFIG_K1_FB_BPP=16` only after confirming the
U-Boot pixel format; do not infer it from the panel resolution.

## USB probe profile

Build and package the read-only probe:

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/usb_probe \
  --build-dir cmake_out/muse_pi_pro_usb_probe \
  --package --package-dir out/k1-usb-probe
```

Run the RAM-only probe with the USB-TTL console attached:

```bash
python3 tools/run_k1_usb_probe_smoke.py --device /dev/ttyUSB0
```

Before reading the DWC3 window, the profile enables USB30 clock/reset and
initializes the USB2 PHY. This is necessary on K1: a DWC3 MMIO read while the
USB30 clock is gated raises an S-mode access fault instead of returning zero.
The profile then reads but does not select a DWC3 role, start xHCI, or control
VBUS/hub GPIOs:

- DWC3 `GSNPSID`, global control/status, and hardware-parameter registers;
- USB2/USB3 PHY controls and DWC3 device status;
- xHCI capability length and structural parameters;
- register zero of the USB2 and combo PHY blocks.

The expected DWC3 signature starts with `0x5533`. The read-only probe ran on the
MUSE Pi Pro on 2026-08-15 and reached NSH with these values:

```text
K1 USB: DWC3 GSNPSID=0x000000005533330a
K1 USB: GCTL=0x0000000032c92004
K1 USB: GSTS=0x000000007e800000
K1 USB: GHWPARAMS0=0x000000004020400a
K1 USB: GHWPARAMS1=0x000000000160c93b
K1 USB: GHWPARAMS2=0x0000000012345678
K1 USB: GHWPARAMS3=0x0000000010420085
K1 USB: USB2PHYCFG=0x0000000040102400
K1 USB: USB3PIPECTL=0x00000000010c0003
K1 USB: DCFG=0x0000000000080804
K1 USB: DSTS=0x0000000000520004
K1 USB: USB2 PHY REG0=0x0000000000000000
K1 USB: combo PHY REG0=0x0000000020200514
K1 USB: xHCI caplen=0x0000000000000020 hcsparams1=0x0000000002000140
```

The complete serial log is
`out/k1-serial/k1-usb-probe-20260815T130353Z.log`; the probe ELF SHA256 is
`7f283f8fd7c7ec57c753ecd1bcc59ad3d63da471e055401a0d86fda4cd5ee474` and the
flat payload SHA256 is
`a6f29521ef9757db963a7f018ae9893c6c1c3c553259f127e6293ff4ce955497`. This is
a real-board PASS for safe MMIO access and the recorded DWC3/PHY/xHCI register
baseline. It does not select a role, start xHCI, control VBUS, enumerate a
device, or provide a NuttX USB DCD. Do not insert a device into the PWR Type-C
port for this test.

This profile deliberately does not register `usbhost_connection_s`, enumerate a
USB device, or provide a NuttX USB DCD; those concerns are covered separately by
the `usb_host` profile or remain outside the current Type-C FDL firmware path.

## Host glue profile

`usb_host_glue` implements the first write-enabled USB stage. It follows the
mainline K1 sequence: enables APMU `CLK_USB30`, releases the USB30 AHB/VCC/PHY
resets, initializes the USB2 PHY PLL for the 24 MHz reference, and selects the
DWC3 Host role. It prints the xHCI capability length and structural parameters
but leaves `USBCMD.RS` clear because no non-PCI xHCI HCD has been integrated yet.

Build it with:

```bash
tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/usb_host_glue \
  --build-dir cmake_out/muse_pi_pro_usb_host_glue \
  --package --package-dir out/k1-usb-host-glue
```

The profile still relies on the board's inherited VBUS/Hub state: USB3 power
enable is GPIO 79, Hub power enable is GPIO 127, and Hub reset is GPIO 123.
It is a hardware-init checkpoint, not USB enumeration proof. Do not connect a
storage device for functional testing until the xHCI command/transfer/event
rings and root-port waiter are present.

## K1 xHCI host profile

The `usb_host` profile provides the fixed-address entry point for the existing
NuttX xHCI core. It uses DWC3 base `0xc0a00000` and K1 PLIC IRQ 125 directly,
so it does not require PCI or MSI. The board calls the host glue first, then
starts the xHCI command ring, event ring, interrupt worker, and root-port
waiter.

Build it with:

```bash
tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/usb_host \
  --build-dir cmake_out/muse_pi_pro_usb_host \
  --package --package-dir out/k1-usb-host \
  --experimental-irq
```

For the first board run, use the repeatable RAM-only smoke tool rather than
typing the U-Boot sequence manually:

```bash
python3 tools/run_k1_usb_host_smoke.py --device /dev/ttyUSB0
```

It stages only `/boot/musepi/contest-nuttx-usb-host-flat.bin` and
`/boot/musepi/k1-go-wrapper-usb-host.bin`, validates each file with
`sha256sum` on the board, stops both U-Boot watchdogs, and then uses
`ext4load mmc 2:5` plus `go 0x12000000`. It never uses `saveenv`, changes
`bootcmd`, or writes raw eMMC sectors. The board must first be booted into its
stock Linux image so ADB is available. If it is already stopped at U-Boot, the
tool issues only `reset` to reach Linux for staging; if neither ADB nor `=>` is
visible, press RST once and rerun the command. After `nsh>` appears, the tool
requires successful enumeration of both USB2817 functions: USB2 root port 1
and USB3 root port 2. This avoids mistaking controller startup for Hub
enumeration.

`CONFIG_K1_USB_IRQ=125` is the raw K1 PLIC source number from the device
tree. The board converts it to the NuttX IRQ namespace (`RISCV_IRQ_EXT + 125`)
before attaching the xHCI ISR; do not pass raw PLIC source numbers directly to
`irq_attach()` or `up_enable_irq()`.

Before xHCI is started, this profile follows the official K1 onboard-hub
driver: it first drives GPIO79, GPIO127, and GPIO123 inactive, then enables
the ordered Hub GPIO array (GPIO127 supply followed by GPIO123, which
deasserts the USB2817 active-low reset). It waits for the MUSE Pi Pro device
tree's `vbus_delay_ms = 200` and finally enables GPIO79 5 V VBUS before
starting xHCI. The Linux BSP then reports approximately 660 ms to the USB2
Hub connection and 940 ms to the USB3 Hub connection. NuttX keeps xHCI
running during that training window; delaying xHCI until after the Hub has
trained causes K1 to report `USBSTS=0x11` and remain halted.

This profile supports both functions of the board's USB2817 external Hub. The
shared xHCI driver allocates a distinct xHCI slot and EP0 for every
`usbhost_hubport_s`, passes Hub Slot Context data when configuring downstream
devices, and releases the slot on disconnect or failed enumeration. The Hub
class accepts the SuperSpeed Hub interface protocol (`3`), reads the USB3
SuperSpeed Hub Descriptor (`0x2a`, 12 bytes), and assigns SuperSpeed to a
downstream port of that function. USB2 Hub handling, including transaction
translator metadata, remains unchanged.

The RAM-only board run on 2026-08-15 completed USB2817 enumeration with this
evidence:

```text
K1 USB: waiter root port=0x0000000000000001
K1 USB: enumerate port=0x0000000000000001
K1 USB: enumerate ret=0x0000000000000000
K1 USB: waiter root port=0x0000000000000002
K1 USB: enumerate port=0x0000000000000002
K1 USB: enumerate ret=0x0000000000000000
```

The tested ELF SHA-256 is
`eaed951207c984ddf454ef84694bd460b64e2c84285b6d8df581df4edc5245f5`; the
complete Hub-enumeration serial log is
`out/k1-serial/k1-usb-host-20260815T102404Z.log`.

Do not apply the generic DWC3 `GCTL.CORESOFTRESET` plus PHY-soft-reset sequence
to this Host profile. The mainline DWC3 core intentionally skips that path for
`dr_mode=host`, leaving Host-block reset to xHCI. A RAM-only K1 test confirmed
that forcing it makes the xHCI capability window read zero and prevents the
HCD from starting. The supported K1 sequence is the APMU/PHY bring-up followed
by xHCI `HCRST`.

The `usb_host` profile includes `CONFIG_USBHOST_UAS=y` and `CONFIG_FS_FAT=y`.
The mass-storage class selects a compatible UAS alternate setting, sends
`SET_INTERFACE` for alt 1, maps Pipe Usage Descriptors to its Command, Status,
Data-In, and Data-Out endpoints, and uses xHCI stream ID 1 for status/data
traffic. The current implementation deliberately serializes one command at a
time; it does not claim command queueing or multiple stream support.

The Netac XS510 currently connected for the 2026-08-15 board test is an
appropriate UAS target: stock Linux reports `0dd8:55aa`, USB3 Hub port 1, and
interface class/subclass/protocol `08/06/62`. Its raw descriptors show BOT at
alt 0 and UAS at alt 1 with Command OUT endpoint `0x04`, Status IN `0x83`,
Data-In `0x81`, Data-Out `0x02`, and SuperSpeed stream companions. Do not bind
`0x62` to the BOT protocol as a workaround.

The initial debug build reached both USB2817 functions but failed while
configuring the USB3 Hub interrupt endpoint. `addr2line` mapped the exception
to `xhci_epfree()`: after a failed `DRVR_EPALLOC`, the Hub class tried to free
its still-null endpoint. The Hub cleanup path now releases an endpoint only
after successful allocation, and xHCI rejects a null endpoint with `-EINVAL`
instead of dereferencing it. The broad `CONFIG_DEBUG_USB` and
`CONFIG_DEBUG_USB_INFO` settings were removed because the failure only appeared
with that synchronous, high-volume serial logging enabled.

The final 2026-08-15 UAS-enabled K1 build used for the board run has flat
payload SHA256
`d1ba9a5bf0c5997444d860d0de95aac2f08ee00272c1298cd2c2d8056e33e857` and ELF
SHA256 `f77c157a7df42b223bfdb227cbc08877788bea72cfd61a62c9cc170d15fe2cd0`.
The strict smoke check completed on the real board and recorded:

```bash
python3 tools/run_k1_usb_host_smoke.py --device /dev/ttyUSB0 \
  --require-uas --require-msc
```

The command required the UAS-only xHCI stream context and stream transfer
markers, then verified `/dev/sda`. The complete serial log is
`out/k1-serial/k1-usb-host-20260815T125230Z.log`. This is a real-board PASS for
UAS alt 1, xHCI stream 1, asynchronous data/status submission, initial SCSI
bootstrap, and block-device registration. It did not mount or write the disk.
The implementation still serializes one SCSI command at a time and does not
claim command queueing or multiple stream support. Do not use the PWR
Type-C/burn port for this host test.

## U-Boot handoff boundary

The validation profiles boot from RAM using the wrapper and U-Boot `go` path.
They do not replace UEFI/Linux firmware or overwrite boot state. The display
profile validates a U-Boot framebuffer handoff; `usb_probe` remains a register
reconnaissance profile, while `usb_host` now has real-board Hub, UAS and
mass-storage evidence. None of these profiles claim a native K1 display
pipeline or USB Device/FDL implementation.
