# MUSE Pi Pro K1 U-Boot 首启手册

## 1. 目标

本手册用于首次验证以下链路：

```text
OpenSBI -> U-Boot -> NuttX ELF -> K1 early log -> nx_start -> NSH
```

首轮只验证 hart 0、轮询 UART 和 SBI TIME。PLIC 外部中断、UART IER、SSTC 和
SMP 保持关闭。

## 2. 安全边界

- 保留一张能够启动原厂系统的 SD 卡；
- 首轮不执行 `saveenv`，不修改持久化 `bootcmd`；
- 不向整盘写入自动生成镜像；
- 不加载到低地址保留区、显示保留区或 OpenSBI 保留区；
- UART 保留 U-Boot 的 115200 8N1 配置；
- NuttX ELF 文件暂存于 `0x12000000`，由 `bootelf -p` 把 LOAD 段放到
  `0x11000000` 起的链接地址。

## 3. 主机侧准备

从 openvela 工作区根目录执行：

```bash
contest2026_287_Agenter/tools/build_k1.sh --clean --package
```

生成目录：

```text
out/k1-bringup/
├── nuttx
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

将 `nuttx` 复制到启动介质。参考系统的专用 SD 布局使用 ext4 第 5 分区和
`/musepi/nuttx`；实际板上的 MMC 编号和分区必须先用 U-Boot 命令确认。

## 4. 串口准备

- 电平：3.3 V；
- 参数：115200、8 data bits、no parity、1 stop bit；
- 同时保存完整日志，不只截取最后几行；
- 上电前启动日志采集，覆盖 BootROM、OpenSBI 和 U-Boot。

不要连接 USB-UART 的供电引脚，只连接 GND、板端 TX 和板端 RX，并再次核对
板卡原理图或丝印。

## 5. U-Boot 操作

首先中断自动启动，然后执行包内 `uboot-commands.txt` 的“检查”部分：

```text
version
bdinfo
mmc list
mmc dev 0
part list mmc 0
help bootelf
help wdt
```

根据实际分区选择 ext4、FAT 或 TFTP 中的一种加载方式。不要连续执行三种加载
命令。加载后确认 `${filesize}` 与包内记录的 ELF 大小一致；若 U-Boot 支持
`hash`，再核对 SHA256。

参考 K1 U-Boot 使用以下两个 watchdog 设备：

```text
wdt dev PMIC_WDT
wdt stop
wdt dev watchdog@D4080000
wdt stop
```

设备名或命令不存在时，保留错误输出，不要猜测其他 MMIO 地址。最终执行：

```text
bootelf -p ${k1_elf_addr}
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

所有结果填写到 `TEST_RECORD.md`。只有完成以上实板验证后，才能把 UART、SBI
timer 和 NSH 标记为“实板通过”。

## 8. 恢复

首轮命令没有执行 `saveenv`，断电重启即可回到原有 U-Boot 默认流程。如果原厂
系统不能启动，立即换回保留的原始 SD 卡，不要在未知状态下继续写 bootloader、
环境分区或 eMMC。
