# K1 MUSE Pi Pro Ethernet Bring-Up

## Scope

This document records the Ethernet state of the K1 MUSE Pi Pro port. The
first implemented phase is intentionally limited to a safe hardware and PHY
validation path:

- EMAC0 RGMII pinmux, clock gate, reset release, and board delay-line setup;
- active-low reset of the RTL8211F PHY on GPIO110;
- Clause 22 MDIO reads/writes through EMAC0;
- RTL8211F identity check at MDIO address 1;
- one-second polling carrier state notification.

The phase does **not** allocate DMA descriptors, enable EMAC interrupts, or
transfer Ethernet frames. `ping`, DHCP, and any traffic test must wait for
the DMA TX/RX phase.

## Confirmed board wiring

| Item | Value |
| --- | --- |
| MAC | K1 EMAC0 at `0xcac80000` |
| PHY | Realtek RTL8211F |
| PHY MDIO address | `1` |
| Expected PHY ID | `0x001cc916` |
| PHY reset | GPIO110, active low |
| PHY reset timing | low 10 ms, then high for 100 ms |
| MAC mode | RGMII, PHY provides the reference clock |
| TX/RX delay line | TX `90`, RX `73` |
| Future EMAC interrupt | PLIC source 131 (unused in this phase) |

The pinmux and timing values were cross-checked against the vendor MUSE Pi Pro
U-Boot DTS and EMAC driver: GPIO0..GPIO14 and GPIO45 use function 1, 1.8 V
drive strength DS2, and the delay-line codes are TX `90` and RX `73`.

## Build

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/ethernet_polling \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_ethernet_polling \
  --jobs 8
```

The profile is separate from `configs/nsh`; ordinary K1 bring-up images do
not enable EMAC hardware.

## First board validation

Flash/load the resulting image only through the already-established K1
bring-up process. This document does not authorize modifying boot storage.
With an RJ45 cable connected to a router or switch, run in NSH:

```text
ifup eth0
mdio 1 2
mdio 1 3
mdio 1 1
ifconfig eth0
```

Expected results:

- Boot log contains an RTL8211F ID close to `0x001cc916`.
- `mdio 1 2` returns `0x001c`.
- `mdio 1 3` returns `0xc916` (the low revision nibble can vary by PHY
  revision).
- `mdio 1 1` bit 2 set means the PHY reports link up.
- `ifconfig eth0` reports the temporary locally administered MAC address and
  carrier state. It does not establish an IP connection in this phase.

To observe cable changes, leave `ifup eth0` active, then plug/unplug the
cable and rerun `ifconfig eth0` after at least one second.

## Next phase: DMA data path

The next implementation milestone requires a 16-byte descriptor ring,
aligned DMA buffers, cache clean/invalidate, physical-address conversion,
descriptor ownership handling, and EMAC source 131 interrupt handling. No
claim of successful Ethernet traffic is valid before those pieces are added
and tested on the board.
