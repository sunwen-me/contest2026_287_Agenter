# K1 MUSE Pi Pro Ethernet Bring-Up

## Scope

This document records the Ethernet state of the K1 MUSE Pi Pro port. The
implemented experimental path brings up EMAC0, validates the RTL8211F, and
uses DMA to exchange Ethernet frames:

- EMAC0 RGMII pinmux, clock gate, reset release, and board delay-line setup;
- active-low reset of the RTL8211F PHY on GPIO110;
- Clause 22 MDIO reads/writes through EMAC0;
- RTL8211F identity check at MDIO address 1;
- one-second polling carrier state notification;
- one cache-line-isolated 16-byte TX descriptor and one RX descriptor;
- 1536-byte, 64-byte-aligned TX/RX DMA buffers;
- cache clean for TX and cache invalidation for RX and descriptors;
- DMA polling every 10 ms on `LPWORK`; the NuttX netdev upper half also runs
  asynchronously on `LPWORK`, while EMAC interrupts remain disabled.

TX copies a NuttX packet into the aligned buffer, cleans the buffer, gives
the descriptor to DMA, and later releases the packet only after DMA clears
`OWN`. TX is limited to 1514 bytes; RX accepts only standard 64..1518-byte
wire frames, rejects hardware-reported frame errors, invalidates the DMA
buffer, copies the FCS-stripped frame into a NuttX packet, then returns the
descriptor to DMA. The simple one-descriptor design deliberately trades
throughput for an auditable first-board path.

This is not yet a claim that Ethernet works on the real board: a link,
ARP, and bidirectional `ping` test are still required.

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
| TX/RX delay line | TX `60`, RX `73` |
| EMAC interrupt | PLIC source 131 (intentionally unused: polling mode) |
| NuttX packet size | 1514 bytes (1500-byte MTU without FCS) |
| DMA frame/buffer size | 1518-byte wire frame with FCS; 1536-byte buffer |
| DMA poll period | 10 ms (`CONFIG_K1_EMAC_DMA_POLL_MSEC`) |

The pinmux, DMA-register layout, descriptor format, and timing values were
cross-checked against the vendor MUSE Pi Pro U-Boot DTS and EMAC driver:
GPIO0..GPIO14 and GPIO45 use function 1, 1.8 V drive strength DS2, and the
MUSE Pi Pro delay-line codes are TX `60` and RX `73`. K1 cache maintenance uses 64-byte
RISC-V Zicbom cache-block operations, matching the K1 OpenSBI cache helpers.

## Build

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/ethernet_polling \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_ethernet_polling \
  --jobs 8
```

The profile is separate from `configs/nsh`; ordinary K1 bring-up images do
not enable EMAC hardware. It explicitly enables `NETDEVICES` rather than
selecting it from `K1_EMAC`, avoiding a Kconfig cycle with configurations
that use late network initialization.

## Build validation

The target build completed on 2026-08-13 using the command above, with
`CONFIG_K1_EMAC_DMA_POLL_MSEC=10`, `CONFIG_NET_ETH_PKTSIZE=1514`, and
`CONFIG_IOB_ALIGNMENT=64`. The K1 ELF checks passed. The compiled
`k1_cache.c` object contains the three expected Zicbom instruction encodings
for cache clean, invalidate, and flush.

## First-board validation

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
  carrier state.

To observe cable changes, leave `ifup eth0` active, then plug/unplug the
cable and rerun `ifconfig eth0` after at least one second.

## Static IPv4 ping test

Connect the board to a switch or directly to a host on a quiet network. Give
the host a spare static address in the same subnet; for example, host
`192.168.50.1/24` and board `192.168.50.2/24`. In NSH:

```text
ifup eth0
ifconfig eth0 192.168.50.2 netmask 255.255.255.0
ping 192.168.50.1
ifconfig eth0
ifdown eth0
```

Pass criteria are: the console reports the expected PHY ID and link mode,
`ping` receives replies, and `ifconfig` remains responsive after `ifdown` /
`ifup`. Also ping the board from the host. If the first ping fails, capture
host ARP traffic and serial output; check that TX descriptor `OWN` clears,
that RX descriptors transition out of `OWN`, and that cache maintenance is
executed before handing ownership across the DMA boundary.

## Remaining work

- Verify the DMA path, including repeated cable replug and large packets, on
  the physical MUSE Pi Pro.
- Add DMA status/error recovery and descriptor rings before making any
  throughput claim.
- Move to EMAC PLIC source 131 only after the polling data path has passed
  on hardware.
