# K1 MUSE Pi Pro 实板交接记录

> 更新时间：2026-08-22（Asia/Shanghai）
>
> 这份文档记录当前这块真实 MUSE Pi Pro 的实测结果，优先级高于只描述
> 参考流程的 `K1_UBOOT_BRINGUP.md`。后续继续上板时先读本文件，不要重新猜
> 接线、MMC 编号、U-Boot 命令或启动方式。

## 1. 当前结论

已经在不刷写 eMMC、不执行 `saveenv`、不改默认启动项的情况下，成功把本项目
的 NuttX 临时启动到板子上，并打通了 NSH 双向交互。实测串口输出为：

```text
W
K1: entry
K1: hart=0x0000000000000000 dtb=0x0000000000000000
K1: initial sstatus=0x8000000200006600 satp=0x0000000000000000 stvec=0x0000000011000080
K1: bss-clear
K1: s-mode bare
K1: nx_start

NuttShell (NSH)
nsh>
```

这已经证明：

- 镜像能够加载到 K1 的 `0x11000000`；
- wrapper 能把 U-Boot 的 `go` 入口参数整理正确；
- NuttX 以 hart 0、S-mode、裸地址空间进入；
- BSS 清零、早期 UART、`nx_start` 和 NSH 初始化均已执行。

SSTC 版本实测通过：`help` 能完整回显并返回新的 `nsh>`，`uname -a`、`free`、
`ps`、`uptime` 均能执行并返回提示符；同一串口会话中的 `uptime` 已从 `00:08:09`
推进到 `00:10:24`，10 分钟交互式稳定性验收 PASS。`free`/`ps` 的 procfs 提示表示
当前实例尚未挂载 `/proc`，不影响 NSH 串口交互。临时跳入 NuttX 后 ADB 消失是正常
现象，因为 Linux 已经不在运行。详细证据见
`docs/K1_REAL_BOARD_STABILITY_20260812.md`。

以太网 polling 版本也已完成实板验收：EMAC0/RTL8211F 的 ARP、双向 ICMP 和 UDPv4
回显均通过。最终 UDP 验证覆盖 14、21（含 NUL）、128 和 1472 字节报文，后者是
1500-byte MTU 下的最大 IPv4 UDP 负载；原始日志、镜像哈希和复现命令见
`docs/K1_ETHERNET_DESIGN.md`。同样只使用 U-Boot `ext4load` 和 `go` 的 RAM-only
流程，未写入启动环境或 eMMC 原始分区。

2026-08-14 已完成 SDH2 eMMC 初始化复验。`k1-emmc-powerdiag` 通过 ADB 放入
`/boot/musepi/` 后，使用同一条 wrapper + `go` 的 RAM-only 路径启动，日志显示：

```text
K1 eMMC: mmcsd_slotinitialize ret=0x0000000000000000
/dev/mmcsd0
/dev/mmcsd0boot0
/dev/mmcsd0boot1
/dev/mmcsd0rpmb
```

这证明 SDH2 的 CMD1/卡识别已走通并完成 NuttX 设备注册；本轮没有执行
`saveenv`、`mmc write`、`mmc erase` 或原始 eMMC 写入。随后使用
`out/k1-emmc-multiblock-20260814` 载荷完成只读块读取：`bs=1024 count=1`
和连续两次 1024-byte 读取均返回 0，串口日志明确出现
`data cmd=0x2452`（CMD18 + MULTIBLOCK）和 `data mode=0x32`。注意
`bs=512 count=2` 会被 NSH `dd` 拆成两个 CMD17，不能单独作为 CMD18 证据。
驱动当前仍为只读，文件系统挂载和写入未验收；完整寄存器修复、SHA256 和复现
命令见 `docs/K1_EMMC_BRINGUP.md`。

无线当前检查点：完整 RTL8852BS2 U2 NIC firmware download 的 RAM-only profile 已完成
静态 CI、上游映像逐字节比对、干净构建、ELF 校验与 GPL 对应源码打包。待测包为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-full-download-static-tuning/`；当前修复版 ELF SHA-256 为
`c068a9c3a53e4d91dee492f37cc05c6ea867ed0cddf66bcb44b995864b91c607`，flat payload SHA-256 为
`bc8993e4a3e0e5acb0ceb21218343055917bab5767f505aac5d17c079175e144`。首次有效上板记录为
`out/k1-serial/k1-wireless-fw-full-download-static-tuning-20260822T100036Z.log`，SHA-256
`7088e1d82e4e12033b89b0431190477b36f7ff16bd6ce21214d34e08f513edc4`：它确认前置阶段和 H5 正常，
但完整 profile 错把 2048-byte FWDL packet 的 block-mode/ADMA 条件限定到旧的单包 config，故首个
section packet 在主机预检返回 `-EINVAL`，没有提交 CMD53 block transfer。该门控已修复并重建；之后
两次 60/180 秒监听日志均为 0 bytes，尚未实际运行修复版。下一次上板必须先开监听、再按 RST，并只
使用 `loadx + go`；不使用 `saveenv`、FDL、fastboot 或任何 eMMC/SPI/eFuse 写入。详细验收条件见
`docs/K1_WIRELESS_BRINGUP.md`。

## 2. 板卡和连接事实

### 2.1 板卡

| 项目 | 实测值 |
|---|---|
| 型号 | SpacemiT K1 MUSE Pi Pro |
| Linux | Bianbu 2.3.5 |
| U-Boot | `U-Boot 2022.10spacemit-gdb67f9dc4-dirty`，Sep 25 2025 |
| CPU ISA | `rv64imafdcv` |
| 内存 | 8 GiB |
| 板载 eMMC Linux 设备 | `/dev/mmcblk2` |
| U-Boot bootfs | `mmc 2:5` |
| ADB 序列号 | `BPMIM102080642256` |
| 主机串口 | `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`（当前指向 `/dev/ttyUSB1`；`ttyUSB*` 编号会随重新枚举变化） |

### 2.2 USB-TTL 接线

USB-TTL 颜色没有统一标准，不能按红黑白绿猜信号。当前已验证的接法是：

| 板子 40Pin | 功能 | USB-TTL |
|---|---|---|
| Pin 6 | GND | GND |
| Pin 8 | UART0_TX | TTL RX |
| Pin 10 | UART0_RX | TTL TX |
| 不接 | 3.3V/VCC | TTL VCC 不接 |

串口参数：`115200 8N1`。板子由 PWR Type-C 供电，USB-TTL 只负责串口信号和
地线。USB-TTL 必须是 3.3V 电平。

### 2.3 Type-C、ADB 和串口的区别

- PWR Type-C 正常启动时不一定向主机枚举 SpacemiT USB 设备；
- `lsusb | grep -i spacemit` 没有输出，不等于板子没有启动；
- 正常 Linux 状态下可以看到 `Spacemit K1 ADB` 和 ADB 设备；
- 临时跳入 NuttX 后，ADB 消失是预期现象，因为 Linux/Android userspace 已经不在运行；
- `adb reboot` 只会重启板子，不会自动停在 U-Boot。当前固件 autoboot 是 0 秒，
  必须在串口监听器已经打开后再执行 `adb reboot`，由监听器抢 U-Boot。

## 3. 主机侧文件和板端文件

### 3.1 主机文件

SSTC 版本项目 ELF：

```text
/home/sw/Dev/k1-workspace/out/k1-bringup-sstc/nuttx
```

```text
size:   2957256 bytes
entry:  0x11000000
sha256: cd01b9075237c77f5c89f76e62a9cc67f3f07f5779e18f8e3b09d0dfb7ad0e25
```

用于 `go` 的 flat binary：

```text
/home/sw/Dev/k1-workspace/out/k1-bringup-sstc/nuttx.flat.bin
```

```text
size:   177152 bytes
sha256: 3bee62b539b3feaa63337c13fe2b61be992ebc5c31691c3a8f6f49298e688b70
```

临时 U-Boot wrapper：

```text
/home/sw/Dev/k1-workspace/out/k1-bringup-sstc/k1-go-wrapper.bin
```

### 3.2 已放到板端的位置

通过 ADB 放到了 Linux 的 bootfs：

```text
/boot/musepi/contest-nuttx
/boot/musepi/contest-nuttx-flat.bin
/boot/musepi/contest-nuttx-sstc-flat.bin
/boot/musepi/k1-go-wrapper.bin
```

这一动作只是向已有文件系统复制文件，没有刷写 bootloader、eMMC 分区或启动环境。

## 4. 已验证的正确启动流程

### 4.1 必须先准备串口监听

串口日志工具位于：

```text
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/capture_k1_serial.sh
```

注意：它不是工作区根目录下的 `tools/capture_k1_serial.sh`，正确路径包含
`contest2026_287_Agenter/`。标准采集命令：

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/capture_k1_serial.sh \
  --device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --duration 120
```

上面的脚本是只读日志抓取器，不会把键盘输入发给板子。要交互输入 NSH，使用：

```bash
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/console_k1_serial.sh \
  --device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
```

该控制台在 U-Boot 提示符下把 Enter 发为 `CR`，在 `nsh>` 下把 Enter 发为 `LF`。
这是当前 K1 最小 readline 配置的必要条件。

自动抢 U-Boot 时，监听器必须在 `adb reboot` 之前打开。该板从主 `U-Boot 2022`
banner 到 `Autoboot in 0 seconds` 约有 2.4 秒；使用
`console_k1_serial.sh --stop-autoboot` 时，脚本会在识别主 banner 后等待 2.2 秒，
然后只发送一个 `s`。不要在 banner 刚出现时发送该字符，且不要用多字符批量中断，
否则残留字符会被后续 U-Boot 命令拼接。

### 4.2 U-Boot 命令

当前实板已经验证的命令顺序如下：

```text
wdt list
wdt dev PMIC_WDT
wdt stop
wdt dev watchdog@D4080000
wdt stop

ext4load mmc 2:5 0x12000000 /musepi/k1-go-wrapper.bin
ext4load mmc 2:5 0x11000000 /musepi/contest-nuttx-sstc-flat.bin
go 0x12000000
```

命令必须逐条等待 `=>` 后再发下一条；`PMIC_WDT` 中间的下划线不能丢。U-Boot
命令使用 `CR` 结束，NSH 命令使用 `LF` 结束。

实测串口交互还有两个容易踩到的时序点：

- 必须等当前命令执行完成并出现新的 `=>` 后再发送下一条，不能把多条命令
  一次性粘贴进去；尤其不能把 `go` 和前面的加载命令连发。
- 发送命令时要检查回显，地址必须完整保持为 `0x12000000` 和 `0x11000000`。
  如果回显缺少数字，或 `wdt stop` 输出 `No device set`，应停止当前流程并
  重新抢 U-Boot，不要继续执行 `go`。

最后一条 `go 0x12000000` 前不要再发送 `Ctrl-C`；正确结果应依次出现 `W`、
`K1: entry` 和 `NuttShell (NSH)`。进入 `nsh>` 后，回车由控制台转换为 LF。

实际返回：

```text
54 bytes read in 5 ms
177152 bytes read in 8 ms
## Starting application at 0x12000000 ...
```

本轮中 `mmc dev 2` 在 U-Boot 命令行没有返回提示符，后来用串口 `Ctrl-C` 退出；
直接使用已经确认有效的 `mmc 2:5` 路径加载成功。因此后续临时启动不要把
`mmc dev 2` 作为必需步骤。

### 4.3 为什么用 wrapper 和 flat binary

wrapper 的核心逻辑是：

```text
a0 = 0                 # NuttX 入口把 a0 当 hartid
a1 = 0                 # 当前先不传 DTB
jump 0x11000000        # 进入 flat NuttX
```

它还先向 UART 打印一个 `W`，用于证明 wrapper 已经执行。

## 5. 已确认不能使用的路径

### 5.1 不要继续使用 `bootelf -p`

在当前板子的官方 U-Boot 上，直接执行：

```text
bootelf -p 0x12000000
```

会让 U-Boot 自己触发：

```text
Unhandled exception: Store/AMO access fault
```

没有进入 NuttX。这个错误不是 NuttX 入口本身导致的；实板上已经用 flat binary
加 wrapper 成功绕过。因此当前文档中的旧 `bootelf -p` 方案只能作为历史失败记录，
不要再把它当成默认命令。

### 5.2 不要直接 `go 0x11000000`

当前官方 U-Boot 的通用 `go` 入口参数/交接状态与 NuttX 入口不匹配，直接执行：

```text
go 0x11000000
```

不能可靠启动。必须先加载并执行：

```text
go 0x12000000
```

让 wrapper 设置 `a0/a1` 后再跳入 NuttX。

### 5.3 不要把 `adb reboot` 当成进入 U-Boot 的命令

它只做重启。正确关系是：

```text
先打开串口自动监听 -> adb reboot -> 自动抢 => -> 输入 U-Boot 命令
```

### 5.4 不要执行的危险操作

在稳定启动链和恢复方案确认前，禁止：

```text
saveenv
mmc write
flash
FDL 刷写
覆盖 bootcmd
```

也不要为了“停看门狗”自行猜测 MMIO 地址；优先使用 U-Boot 的 `wdt dev`/`wdt stop`。

## 6. 看门狗事实

U-Boot 启动时明确打印：

```text
WDT:   Started PMIC_WDT with servicing (60s timeout)
WDT:   Started watchdog@D4080000 with servicing (60s timeout)
```

所以进入临时 payload 前要尝试停止两个设备：

```text
wdt dev PMIC_WDT
wdt stop
wdt dev watchdog@D4080000
wdt stop
```

每个 `wdt dev` 和 `wdt stop` 都必须单独等待 `=>`。不能只看到了回显就认为
命令成功；`wdt stop` 的结果中不能出现 `No device set`。如果出现，重新执行
对应的 `wdt dev` 后再执行 `wdt stop`。

即使两个 `wdt stop` 都执行了，也不能单凭“不复位”宣布系统稳定；还必须验证
NuttX 的串口输入、timer、`uptime` 和至少数分钟连续运行。

## 7. 当前 NuttX 实现边界

当前配置和实现是最小单 hart bring-up：

- RV64、S-mode；
- `CONFIG_K1_PLIC` 关闭；
- UART 使用 K1 专用轮询 `/dev/console`；
- 不写 U-Boot 继承的 UART IER；
- timer 使用 K1 支持的 SSTC `stimecmp`，timebase 为 24 MHz；未启用 SSTC 时保留 SBI TIME fallback；
- 入口固定在 `0x11000000`；
- `a1`/DTB 当前为 0；
- 暂不启用 PLIC、SMP、外设中断和通用中断式 UART。

相关源码：

```text
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/chip/k1/k1_head.S
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/chip/k1/k1_start.c
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/chip/k1/k1_console.c
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/chip/k1/k1_timerisr.c
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/chip/k1/k1_irq.c
```

## 8. 本轮日志和证据

完整 Linux 启动日志（用于确认官方 U-Boot、MMC、看门狗和 autoboot）：

```text
/home/sw/Dev/k1-workspace/out/k1-serial/k1-20260812T081046Z.log
```

成功进入 NuttX 并执行 NSH 命令的完整串口证据：

```text
/home/sw/Dev/k1-workspace/out/k1-serial/k1-final-interactive.log
```

SSTC 版本 `uptime` 复核通过；之前较早的 120 秒日志
`k1-20260812T080606Z.log` 记录的是官方 Linux 关机，不是 NuttX 成功日志，
不要混淆。

RTL8852BS2 MSS eFuse selector 已完成 RAM-only 实板验收。使用
`out/k1-wireless-fw-mss-efuse-static-tuning/` 的 wrapper 和 payload 从 U-Boot `loadx + go`
启动，串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-mss-efuse-static-tuning-20260822T090959Z.log`
（SHA-256 `4b91ffb9b382b06a392578af612bb77b73e794d76f88910a9c6a27fc1d39a954`）。它确认 Wi-Fi
Function 1、DLE/SCC、HCI flow-control、SDIO pre-init、image layout、eFuse `0x5ec/0x5ed` 和
Bluetooth H5 local-version 均通过；selector 为 `ff/ff`，对应原厂 default MSS pool type `f`、
customer/key index `0/0`。注意：该输出是 newer key-pool 格式的 selector，而当前 U2 NIC image 的
`MSSC=2` 实际走 legacy `__mss_index()`；对应 profile
`wireless_fw_mss_legacy_signature_static_tuning_diag` 已完成 RAM-only 实板验收。串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-mss-legacy-signature-static-tuning-20260822T092901Z.log`
（SHA-256 `3876f1cd0a34f6ab70ac5875894b194508e59f901d1ffb0c6cc76b2a536a0789`）。它确认 `ff/ff`
按 legacy mapping 选择 index `0`，并验证 signature source `0x43440`、target `0x42e00`、长度
`0x200`、首尾 word、完成标志、Bluetooth H5 local-version 和 `nsh>`。两项 profile 都没有复制
signature、进入 FWDL preboot、写 eFuse、eMMC、U-Boot 环境或 firmware FIFO；Wi-Fi MAC/netdev
仍未实现，详见 `K1_WIRELESS_BRINGUP.md`。

串口采集目录：

```text
/home/sw/Dev/k1-workspace/out/k1-serial/
```

其中 `k1-stability-20260812T095000Z.log` 和 `k1-live-20260812T100000Z.log`
为空，只表示监听器没有捕获到新的字节，不是新的成功/失败启动证据。

## 9. 下一次继续时的最小步骤

1. 读完本文件，确认 `/dev/ttyUSB0` 存在且没有被其他串口程序占用。
2. 打开自动串口监听。
3. 若板子在 Linux，执行 `adb reboot`；若已进入临时 NuttX，执行 `reboot`。
   只有串口无响应时才短按 `RST`。
4. 自动抢到 `=>` 后执行第 4.2 节命令；跳过 `mmc dev 2`。
5. 看到 `nsh>` 后使用双向控制台发送 `help`，命令以 `LF` 结束。
6. 记录 `help` 输出和新的 `nsh>`；当前 SSTC 版本已通过。
7. 随后执行：

```text
free
ps
uname -a
uptime
```

8. `free`/`ps` 若提示 procfs 未挂载，属于当前最小配置预期，不是串口故障。
9. 继续保持“只临时加载、不持久化、不刷写”的安全边界。

10 分钟稳定性验收已经完成；eMMC 初始化也已通过实板复验。下一阶段转入 GPIO/40Pin 基础 Demo。GPIO 控制器地址、
GPIO 编号和 pinmux 必须先由 K1 DTS、厂商资料或实测 Linux 配置确认，再实现 NuttX
驱动。

## 10. 恢复原系统

本轮没有修改持久化 U-Boot 环境，也没有写入 eMMC bootloader。临时 `go` 跳转不会
改变默认启动链。板子卡在 NuttX 时，短按 `RST` 或重新上电即可重新进入原来的
U-Boot/Linux 启动流程；需要进入原系统时不要再执行临时加载命令即可。
