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

The physical-board link, ARP, and host-to-board ICMP path have been verified.
The board-initiated ICMP test requires `CONFIG_NET_ICMP_SOCKET`; the
`ethernet_polling` profile enables it for the final bidirectional test.

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

`CONFIG_NET_UDP` is required by the `mdio` application because it creates an
`AF_INET`/`SOCK_DGRAM` control socket for network ioctls. `CONFIG_NET_ICMP_SOCKET`
is required by the `ping` application because it creates an `IPPROTO_ICMP`
socket for echo requests.

## Physical-board evidence

On 2026-08-13, the MUSE Pi Pro was loaded through the RAM-only U-Boot path;
no boot variables, bootloader regions, or eMMC raw blocks were modified. The
final tested ELF SHA256 was `1ab48cd1df9c81a494b8f5436893c54d5a9b273c990d07fedad06b7fbeaee19a`;
the loaded flat payload SHA256 was
`bcdc12eaf810f61719cf0c92f51ae7f1c8ab59f7c0ff90c1902d49827bd5dcff`.

- `ifup eth0` completed successfully.
- `mdio 1 2` returned `0x001c` and `mdio 1 3` returned `0xc916`, identifying
  the RTL8211F at PHY address 1.
- A USB Ethernet adapter was directly cabled to the board. The host interface
  was `192.168.50.1/24`; the board was `192.168.50.2/24` with MAC address
  `02:00:00:00:00:01`.
- The final board-to-host test returned three of three replies with no packet
  loss and 7 ms round-trip time.
- The final host-to-board test returned three of three replies with no packet
  loss and 2.350--2.676 ms round-trip time. Host ARP resolved the board MAC as
  `02:00:00:00:00:01`.
- The raw success transcript is
  `out/k1-serial/k1-ethernet-smoke-20260813T133140Z.log`.

The PHY carrier worker checks link state once per second. After `ifup eth0`,
allow roughly three seconds before starting a counted ping test. Earlier
packets can fail with `ENETUNREACH` while carrier has not yet been published.

The first ICMP-socket image exposed a separate application resource issue:
the inherited default `ping` task stack (2048 bytes, 1936 bytes usable) faulted
while formatting a received reply. The profile now explicitly sets
`CONFIG_SYSTEM_PING_STACKSIZE=4096`; the final three-packet board-to-host test
completed without a trap.

## Reproducible smoke test

The host USB Ethernet interface must have `192.168.50.1/24`, and the final
flat payload and wrapper must already have been copied to `/boot/musepi/` as
`contest-nuttx-ethernet-udp-flat.bin` and
`k1-go-wrapper-ethernet-udp.bin`. Then run:

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/run_k1_ethernet_smoke.py \
  --device /dev/ttyUSB0 \
  --host-interface enx3cab72bf00b4
```

The tool first requires the host interface to hold its configured static IPv4
address, then stops U-Boot, uses `ext4load` and `go` for a RAM-only startup,
waits for PHY carrier convergence, and requires three successful board-to-host
and host-to-board ICMP replies. It does not invoke `saveenv` or raw eMMC
writes.
When the board is already running the configured NuttX shell, rerun only the
network checks without rebooting:

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/run_k1_ethernet_smoke.py \
  --device /dev/ttyUSB0 \
  --host-interface enx3cab72bf00b4 \
  --resume-nsh
```

## UDPv4 echo validation

The `ethernet_polling` profile also builds the `k1_udpecho` NSH command. It
binds UDP port 33333, receives exactly four datagrams with a three-second
per-packet timeout, returns each datagram byte-identically to its source, and
prints `PASS` before exiting. The command is a bounded data-plane test rather
than a persistent service.

After copying an image built from the current profile to `/boot/musepi/`, add
`--udp-echo` to the smoke runner. It starts `k1_udpecho` after the bidirectional
ICMP checks, sends four tagged payloads from `192.168.50.1` (including embedded
NUL bytes, a 128-byte binary payload, and a 1472-byte binary payload), and
verifies source IP/port and byte identity for every echoed payload. The 1472-byte
case is the maximum IPv4 UDP payload in a standard 1500-byte MTU Ethernet frame
(1500 - 20-byte IPv4 header - 8-byte UDP header):

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/run_k1_ethernet_smoke.py \
  --device /dev/ttyUSB0 \
  --host-interface enx3cab72bf00b4 \
  --udp-echo
```

### UDP validation status

The final MTU UDP image was built and packaged on 2026-08-13. Its ELF SHA256
is `110310ab3baa2536a092dee206d5c6169de1eb1121e5d95fc3448c496a066c35`
and its flat payload SHA256 is
`c6542047b9f12caa56bd70c70d17bd228e9520ee7b73f1d7ce5e04a3b02b0878`.
The generated configuration, built-in command table, and ELF symbol table
confirm `k1_udpecho` is enabled as an 8192-byte-stack command on UDP port
33333. Its 1472-byte receive buffer is dynamically allocated so the maximum
datagram does not consume the command task stack.

The UDP image completed its physical-board run on 2026-08-13 using the
RAM-only U-Boot path. The host copied the flat payload and wrapper to bootfs
using ADB, verified their SHA256 values there, then loaded them with
`ext4load mmc 2:5` and entered NuttX through `go 0x12000000`. No boot
variables, bootloader data, or raw eMMC blocks were written.

- Board-to-host ICMP: 3/3 replies, no loss, 9.000--20.000 ms.
- Host-to-board ICMP: 3/3 replies, no loss, 6.009--6.939 ms.
- Host ARP resolved `192.168.50.2` as `02:00:00:00:00:01`.
- `k1_udpecho` received and byte-identically echoed four datagrams: 14 bytes,
  21 bytes with an embedded NUL, 128 bytes, and 1472 bytes.
- The command returned `k1_udpecho: PASS 4/4 datagrams echoed`.
- The raw success transcript is
  `out/k1-serial/k1-ethernet-smoke-20260813T145117Z.log` with SHA256
  `bcdb7ff609e86d7054b53c39a1df61c21dad7490cf43964323071c501f9600a1`.

During validation, NetworkManager briefly removed the host's static address,
which caused NuttX to cache an ARP-unreachable result for the host. Restoring
the address and deleting that transient ARP entry restored the link. The smoke
tool now fails before rebooting the board when the configured host IPv4 address
is absent. A pre-fix 1472-byte test also exposed an application stack issue;
the final image uses an 8192-byte command stack and a heap buffer, and passed
the same physical-board test.

The K1 board profile exposes the standard NSH `reboot` command through
`CONFIG_BOARDCTL_RESET`. The board-level reset handler first tries SBI SRST;
the supplied K1 OpenSBI returns `SBI_ERR_NOT_SUPPORTED`, so the handler then
programs the on-chip watchdog for its shortest interval. This returns the
RAM-only payload to the normal U-Boot/Linux boot chain without manual RST.
Use the physical `RST` button only when the serial console is no longer
responsive.

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
# Wait about three seconds for the PHY polling worker to publish carrier.
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
