# MUSE Pi Pro K1 U-Boot 首启手册

> **实板交接优先说明（2026-08-12）**：当前真实 MUSE Pi Pro 已验证官方 U-Boot
> 上的 `bootelf -p` 会在 U-Boot 内触发 `Store/AMO access fault`，不能继续按本手册
> 的旧 ELF 流程操作。当前实板可用路径、文件哈希、看门狗、自动抢 U-Boot 方法和
> wrapper/flat binary 命令，以 [`K1_REAL_BOARD_HANDOFF.md`](K1_REAL_BOARD_HANDOFF.md)
> 为准；尤其使用 `go 0x12000000`，不要直接 `bootelf -p` 或 `go 0x11000000`。

## 1. 目标

本手册保留历史参考信息；当前真实板默认以
[`K1_REAL_BOARD_HANDOFF.md`](K1_REAL_BOARD_HANDOFF.md) 和最终包内的
`uboot-commands.txt` 为准。本手册说明的链路是：

```text
OpenSBI -> U-Boot -> NuttX payload -> K1 early log -> nx_start -> NSH
```

首轮只验证 hart 0、轮询 UART 和系统 timer。PLIC 外部中断、UART IER 和 SMP
保持关闭；当前实板版本启用了 K1 支持的 SSTC `stimecmp`，SBI TIME 保留为
代码 fallback。

## 2. 安全边界

- 保留一张能够启动原厂系统的 SD 卡；
- 首轮不执行 `saveenv`，不修改持久化 `bootcmd`；
- 不向整盘写入自动生成镜像；
- 不加载到低地址保留区、显示保留区或 OpenSBI 保留区；
- UART 保留 U-Boot 的 115200 8N1 配置；
- 当前实板路径将 wrapper 暂存于 `0x12000000`，flat NuttX payload 装到
  `0x11000000`，再执行 `go 0x12000000`；旧的 ELF/`bootelf -p` 路径仅作历史参考。

官方 MUSE Pi Pro 用户指南记录的产品默认启动路径是 UEFI，PWR Type-C 同时支持供电和 USB Device/FDL 烧录。本文是本项目的 **U-Boot 手工调试路径**，只有在串口上确认实际固件提供 U-Boot 命令行后才适用；不要因为 Type-C 没有 USB 枚举就直接进入 FDL 或假设 U-Boot 已经运行。

## 3. 主机侧准备

从 openvela 工作区根目录执行：

```bash
contest2026_287_Agenter/tools/build_k1.sh --clean --package
```

生成目录（最终实板包）：

```text
out/k1-bringup-final/
├── nuttx
├── contest-nuttx-flat.bin
├── k1-go-wrapper.bin
├── nuttx.sha256
├── elf-report.txt
├── uboot-commands.txt
├── K1_UBOOT_BRINGUP.md
├── K1_HOST_TOOLING.md
├── TEST_RECORD.md
├── SOURCE_AND_LICENSES.md
├── capture_k1_serial.py
├── decode_k1_trap.py
├── licenses/
└── PACKAGE_MANIFEST.txt
```

将 `contest-nuttx-flat.bin` 和 `k1-go-wrapper.bin` 复制到 bootfs 的 `/musepi/`；
当前实板使用 `mmc 2:5`。不要覆盖 bootcmd 或写入 eMMC 原始区域。

## 4. 串口准备

- 板端使用 40Pin：pin 6=`GND`、pin 8=`UART0_TXD_3V3`、pin 10=`UART0_RXD_3V3`；
- 板端 TX 接 USB-TTL RX，板端 RX 接 USB-TTL TX；
- 电平：3.3 V；
- 参数：115200、8 data bits、no parity、1 stop bit；
- USB-TTL VCC 不接，板子由 PWR Type-C 供电；
- PWR Type-C 在正常启动时不保证向主机枚举 USB 设备，只有 FDL 烧录模式才用于 Titan/fastboot；
- 同时保存完整日志，不只截取最后几行；
- 上电前启动日志采集，覆盖 BootROM、OpenSBI 和 U-Boot。

不要根据 USB-TTL 线的红黑白绿颜色猜测信号，按板端 pin 6/8/10 和转接器丝印连接。

## 5. U-Boot 操作

先由 PWR Type-C 供电：若 STAT 熄灭，按 PWR 约 1 秒后松开；若板子已经运行，短按 RST。双向串口控制台开始后，确认出现 U-Boot 提示符，再执行包内 `uboot-commands.txt` 的真实板路径：

```text
version
bdinfo
mmc list
mmc dev 0
part list mmc 0
help bootelf
help wdt
```

当前实板直接使用 `ext4load mmc 2:5`。每条命令必须等待 `=>` 后再发下一条；
U-Boot 命令以 `CR` 结束，NSH 命令以 `LF` 结束。

参考 K1 U-Boot 使用以下两个 watchdog 设备：

```text
wdt dev PMIC_WDT
wdt stop
wdt dev watchdog@D4080000
wdt stop
```

设备名或命令不存在时，保留错误输出，不要猜测其他 MMIO 地址。最终执行：

```text
ext4load mmc 2:5 0x12000000 /musepi/k1-go-wrapper.bin
ext4load mmc 2:5 0x11000000 /musepi/contest-nuttx-flat.bin
go 0x12000000
```

## 6. 预期早期输出

进入 payload 后应依次出现：

```text
K1: entry
K1: hart=0x... dtb=0x...
K1: initial sstatus=0x... satp=0x... stvec=0x...
K1: bss-clear
K1: s-mode bare
K1: nx_start
```

含义：

| 最后输出 | 判断 |
|---|---|
| 没有 `K1: entry` | ELF 未进入、入口地址错误、运行模式错误或 UART handoff 不成立 |
| 有 entry 但 hart 非 0 | U-Boot handoff 与单 hart 假设不一致 |
| DTB 为 0 或异常地址 | `bootelf` 未传入预期 DTB；后续 PLIC/SMP 不得启用 |
| 只有 `K1: entry` | BSS 清理区间或早期内存访问异常 |
| 到 `bss-clear` | BSS 和初始栈基本可用 |
| 到 `s-mode bare` | `satp=0` 和 `sfence.vma` 已执行 |
| 到 `nx_start` | 芯片入口完成，问题位于 NuttX 初始化或后续驱动 |

同步异常会在 NuttX 通用 panic 路径之前打印：

```text
K1 EXCEPTION
  scause=0x...
  sepc=0x...
  stval=0x...
  sstatus=0x...
  satp=0x...
  sp=0x...
```

保存这些值，并使用包内同一份 ELF 执行：

```bash
tools/decode_k1_trap.sh \
  --log out/k1-serial/<log>.log \
  --elf out/k1-bringup/nuttx \
  --output out/k1-serial/<log>-traps.md
```

## 7. NSH 验收

进入 NSH 后至少记录：

```text
help
uname -a
ps
free
uptime
```

随后保持运行 10 分钟，观察：

- 是否发生重复 trap；
- `uptime` 是否持续推进；
- 串口输入是否丢失或卡死；
- shell 是否仍能响应；
- 是否出现 timer 漂移或 watchdog 复位。

当前已实测 `help`、`uname -a`、`free`、`ps`、`uptime` 均返回 `nsh>`；其中
`free`/`ps` 的 procfs 未挂载提示属于最小配置限制。长稳 10 分钟和 procfs
功能仍应作为后续增强验收记录到 `TEST_RECORD.md`。

## 8. 恢复

首轮命令没有执行 `saveenv`，断电重启即可回到原有 U-Boot 默认流程。如果原厂
系统不能启动，立即换回保留的原始 SD 卡，不要在未知状态下继续写 bootloader、
环境分区或 eMMC。

若板子实际进入的是官方 UEFI，而不是 U-Boot 命令行，本手册的 `bootelf` 流程暂不适用；先保存串口日志和 UEFI 版本，再单独设计 UEFI/SD 启动集成。
