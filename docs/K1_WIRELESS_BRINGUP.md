# K1 RTL8852BS2 Wireless Bring-up

更新时间：2026-08-25（Asia/Shanghai）

本文件记录 MUSE Pi Pro 板载 RTL8852BS2 的 openvela 首轮迁移边界。它只覆盖
无线供电、pinmux、Wi-Fi SDIO Function 1 传输层和 Bluetooth H5 链路诊断；不宣称
Wi-Fi 或蓝牙已经可用。

Wi-Fi 固件下载成功后，`wireless_fw_runtime_h2c_loopback_diag` 进一步执行原厂
`mac_fwcmd_lb()` 的 H2C CMD_PATH 回环：在 RAM 中构造 24-byte 递增载荷和 8-byte
H2C header，送入 SDIO channel 12 TX FIFO；随后轮询 `RX_REQ_LEN`，固定地址读取
`RXFF`，校验 RTL8852B 16-byte RX descriptor、C2H header 和原样返回的载荷。它是
固件运行时 H2C/C2H 的端到端验证，不启用 SDIO 中断、不注册 `wlan0`、不扫描或关联，
也不写入 eFuse、U-Boot 环境或任何持久化介质。

## 1. 硬件与协议事实

| 功能 | K1 资源 | 依据 |
| --- | --- | --- |
| Wi-Fi | SDH1 `0xd4280800`，GPIO15--20，4-bit 1.8 V SDIO | `spacemit-com/linux-6.6` 的 `k1-x_MUSE-Pi-Pro.dts` 与 `k1-x_pinctrl.dtsi` |
| Bluetooth | UART2 `0xd4017100`，GPIO21=TX、GPIO22=RX、GPIO23=CTS、GPIO24=RTS | Linux mainline `arch/riscv/boot/dts/spacemit/k1-pinctrl.dtsi` 的 `uart2_0_cfg` / `uart2_0_cts_rts_cfg` |
| Bluetooth 协议 | H5（3-wire），115200 8E1，关闭 RTS/CTS | `spacemit-com/buildroot-ext` 的 `board/spacemit/k1/plt_overlay/etc/init.d/S40hci`：`rtk_hciattach -n -s 115200 ttyS2 rtk_h5`；`spacemit-com/rtk_hciattach` |
| RF 电源 | GPIO67，高有效 | `rf-pwrseq/pwr-gpios` |
| Wi-Fi REG_ON | GPIO116，高有效 | `wlan-pwrseq/regon-gpios` |
| Wi-Fi wake | GPIO66，输入 | `wlan-pwrseq` pinctrl |
| Bluetooth RESET_N | GPIO63，高有效 | `bt-pwrseq/reset-gpios` |

GPIO64、GPIO65 不在该板 DTS 的无线电源序列中，不能作为无线控制线使用。

官方 `rtk_hciattach` 的 H5 补丁表将 RTL8852BS 映射到
`rtl8852bs_fw` 与 `rtl8852bs_config`。它们不在本仓、也不能由相近型号固件替代。
2026-08-20 已在原厂 Linux 根文件系统确认一组同名文件：
`/lib/firmware/rtlbt/rtl8852bs_fw`（181658 bytes，SHA-256
`a3a203199fd7cafaa5bd22d34b08b7ff5ad3ee0339d7a2bc95e02577d9d3d88a`）和
`/lib/firmware/rtlbt/rtl8852bs_config`（33 bytes，SHA-256
`efa8915db59c5bc30aaa23e1f264656bbf425fcaf091db0954c27752a1d8f7a0`）。同一根文件系统
还包含不同内容的 `/lib/firmware/rtl_bt/rtl8852bs_fw.bin`（94556 bytes）；在取得来源、
许可和 `rtk_hciattach` 实际选择规则前，任何一个文件都不得复制到本 Apache-2.0 仓库或
由 NuttX 加载。

## 2. 已实现

- `chip/k1/k1_sdio.c`：SDH1 实例、4-bit SDIO capability，以及标准
  CMD0/CMD5/CMD3/CMD7 卡选择、CCCR/FBR 的 CMD52 识别读取，Function 1 的
  512-byte block size、I/O enable/ready，以及受限的 CMD53 ADMA2 读写接口；
- `board/k1/muse_pi_pro/src/k1_wireless.c`：GPIO15--24 pinmux、无线控制线
  时序、SDH1 探测和 Bluetooth RESET_N 释放；
- `chip/k1/k1_bt_uart.c`：UART2 的 H5 诊断器。它按官方 attach 参数配置
  115200 8E1、关闭 RTS/CTS，轮询完成 H5 `SYNC -> CONFIG`，按协商结果校验 CRC、
  确认可靠事件，并发送标准 HCI `Read Local Version Information`；
- `board/k1/muse_pi_pro/configs/wireless/defconfig`：独立试验配置，默认
  `nsh` 配置不变。

上电时序固定为：`BT_RESET_N=0`、`WLAN_REG_ON=0`、`RF_PWR=0`，等待 100 ms，
再依次置 `RF_PWR=1`（100 ms）、`BT_RESET_N=1`（10 ms）、
`WLAN_REG_ON=1`（10 ms）。GPIO66 仅配置为输入，不驱动。

这 100 ms 的低电平停留用于 warm reset 后的外部无线模块重新进入关断状态；它不
改变 Linux pwrseq 的控制线定义，也不涉及持久存储。

其中 100 ms 不是任意保守值：厂商 Linux 的 `spacemit-rf-pwrseq` 在该 DTS 未提供
`power-on-delay-ms` 时采用此默认等待，再由子节点拉高 `WLAN_REG_ON`；其
`spacemit-wlan` 子驱动同样没有该属性，但该驱动的默认值为 10 ms。

Wi-Fi 卡枚举会把 CCCR 的 Bus Interface Control 设为 4-bit，并将主机切换为
4-bit 模式；随后读取 CCCR revision、SD revision、IOEN、IORDY、Bus Interface、
Card Capability 和已声明 function 的 FBR interface code。r19 针对 Function 1 写入
512-byte block size 和 IOEN bit，轮询 IORDY 后发起 byte-mode CMD53。本轮及后续实板
记录均表明 CMD53 的命令阶段可被卡接受，但数据阶段没有启动；因此它不证明 Function 1
数据通道可访问，也不访问 MAC 寄存器、不加载 Realtek 固件和不注册网络设备。

SDH1 在厂商 DTS 中标记为 `BROKEN_PHY_MODULE`。因此复位后的 SDIO 路径只设置
`TX_INT_CLK_SEL`，不会写 PHY、`LEGACY_PAD_CLK_ON` 或厂商 `MISC_INT` 74-clock
路径；后者只适用于不支持 SDIO 的主控。Linux 实板 `mmc1` 的初始状态为 400 kHz、
3.3--3.4 V VDD 和 1.8 V signalling，NuttX 对应使用 SDHCI 3.3 V power-select 与
Host Control 2 的 1.8 V signalling 位。

实板 SDH1 在无响应的 CMD0 后不会置 SDHCI `Response Complete` 状态位。NuttX 对
CMD0 改为仅确认 `CMD_INHIBIT` 已释放，再继续发送有响应的 CMD5；其余命令仍以
`Response Complete` 轮询完成。这是控制器完成语义的适配，不会放宽 CMD5/CMD3/
CMD7/CMD52 的错误检查。

当前 r8 验证将 SDH1 controller reset 放在 `RF_PWR=0`、`WLAN_REG_ON=0` 已稳定
10 ms 的窗口完成，随后才按既定时序拉高 RF_PWR 和 REG_ON。Linux 的 SDHCI host
在 WLAN power sequence 之前创建，NuttX 因而通过 `k1_sdio_wifi_prepare()` 复现
这个顺序；之后的 `k1_sdio_wifi_probe()` 复用已初始化的 host，不会二次 reset。
串口出现 `K1 Wi-Fi: SDH1 prepared while reset held` 后，CMD5 若获得 R4/OCR 即
证明原先的 host-reset 时序是根因；若仍无响应，应排除这个变量并转向采集 host
电源/时钟寄存器的上电前后差异。

2026-08-17 的 r8 实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r8-boot-20260817.log`。
它确认 preparation 标记已在 RF_PWR/REG_ON 拉高前输出，但 CMD5 仍为
`INT_STATUS=0`、R4/OCR=`0`。因此 host reset 相对无线电源时序不是本故障的根因；
它也使用了早期实现保留的 30 ms REG_ON 等待。Linux `spacemit-wlan` 的实际默认是
10 ms，因此 r9 仅将这个延迟改为 10 ms；不能再改变已验证的 SDH1 pinmux、host reset
顺序或 CMD0 完成处理。

2026-08-17 的 r9 实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r9-boot-20260817.log`。
它在 10 ms REG_ON 延迟下仍得到与 r8 完全相同的 CMD5 timeout（`INT_STATUS=0`、
R4/OCR=`0`），因此 REG_ON 后等待时间也不是根因。下一步只读 PMIC I2C8 的 SPM8821
DCDC3 enable/voltage 寄存器，再决定是否需要移植 I2C8 的诊断支持。

U-Boot 在 2026-08-17 对 SPM8821 的只读检查得到 I2C bus 3（SoC I2C8）、地址
`0x41` 的 `0x4d=0x01`、`0x4e=0xbc`，与 Linux 的 DCDC3 enable/1.8 V 状态一致，
因此 DCDC3 不再作为猜测根因。随后 Linux 实板 dmesg 与 `spacemit-bt.c` 证明
`set block: 0` 调用的是 `spacemit_bt_on(true)`：RF_PWR 先拉高并经过父节点的
100 ms，BT_RESET_N 拉高并等待默认 10 ms，之后才加载 Realtek 模块和枚举 SDIO。
r10 仅将这个已证实的 BT release 顺序提前至 CMD5 之前；r9 中的 BT_RESET_N 在
CMD5 失败后才释放，与 Linux 实际启动顺序不符。

2026-08-17 的 r10 与 r11 实板记录分别位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r10-boot-20260817.log`
和 `/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r10-20260816T171649Z.log`
（后者文件名沿用了工具的 r10 默认名，内容实际为 r11）。r10 提前释放
BT_RESET_N 后 CMD5 仍为 `INT_STATUS=0`、R4/OCR=`0`，因此排除了 Bluetooth reset
时序。r11 的 GPIO20 `DS3/down` 改动同样没有改变结果；随后复核实板 U-Boot 与
厂商 Linux 的 `pinctrl_mmc2` 发现该改动错误地复用了 `pinctrl_mmc1_fast` 的 CLK
配置，不能作为 SDIO 电气配置的依据。

r12 修正了 SDH1 APMU 时钟控制寄存器：此前代码错误地把它写成 `0xd42828dc`，而
厂商 `ccu-spacemit-k1x.c` 和 `reset-spacemit-k1x.c` 明确它应为 APMU `+0x58`，即
`0xd4282858`。在 Wi-Fi host 初始化中，r12 显式选择 `pll2_d8`（mux=2，375 MHz）、
divider=0，触发并等待 bit 11 的 frequency-change 自清，随后打开 bit 4 clock gate
并释放 bit 1 reset。SDHCI 的 identification divider `469` 因而产生约 400 kHz。
这一配置仅适用于 SDH1；eMMC SDH2 保持 U-Boot handoff 时钟配置不变。

2026-08-17 的 r12 RAM-only 实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r10-20260816T173703Z.log`
（工具文件名仍待单独修正）。它读回 SDH1 APMU 值 `0x52`，确认 mux=2、divider=0、
clock gate=1 和 reset deassert=1；SDHCI 也产生了 400 kHz identification clock，CMD5
命令寄存器读回 `0x0502`。但是 Response Complete 和 R4/OCR 仍均为零。因此 r12
排除了 SDH1 时钟寄存器地址、source mux、divider、gate 和 reset 作为 CMD5 无响应的
根因。

r13 将 GPIO20 恢复为已验证的 `pinctrl_mmc2` 定义：GPIO15--20 全部为 mux1、
1.8 V DS2、pull-up。它只修正 r11 的错误 pin-group 映射，保留 r12 的 SDH1 时钟配置。
2026-08-17 的 r13 RAM-only 实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r10-20260816T174259Z.log`
（工具文件名仍待单独修正）。它与 r12 一样读回 SDH1 APMU `0x52`、400 kHz card clock、
已写入 CMD5 `0x0502`，但 Response Complete/R4 仍为零。因此 r13 排除了 GPIO20 的
DS3/down 与 DS2/up 两种 pad 配置差异，后续只比较厂商 Linux SDH1 host 初始化中尚未
移植的控制寄存器、时钟和 reset 状态。

r14 只测试一个仍未覆盖的控制器位组：Linux mainline 的 K1 SDHCI reset 为非 MMC
host 设置 `OP_EXT[12:11]`（force clock on 和 override clock output enable）。r13
实测在 RESET_ALL 后继承 `OP_EXT=0x75090400`，这两个位均为零；r14 仅置这两个位，
仍保留厂商 6.6 `BROKEN_PHY_MODULE` 的 `TX_INT_CLK_SEL` 路径，不启用 PHY、legacy
pad 或 74-clock 序列。2026-08-17 的 r14 实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r10-20260816T175817Z.log`
（工具命名仍沿用 r10）；该寄存器回读已变为 `0x75091c00`，但 CMD5 仍返回
`-ETIMEDOUT`。因此这两个主线时钟覆盖位已排除，未保留在驱动中。

r15 复核 SDIO R4 位定义后定位并修复了实际根因：此前代码错误地把 bit 27 当作
I/O-ready；标准 R4 的 I/O-ready 是 bit 31，bit 27 只表示卡是否同时具有 memory
function。RTL8852BS2 是纯 SDIO 卡，实板 R4 为 `0x90ffffff`，其 bit 31 已置位而
bit 27 为零。此前记录的 101 次 CMD5 并非卡无响应，而是每次都收到有效 R4、随后被
错误的 bit 27 判断为未就绪，最终返回 `-ETIMEDOUT`。

2026-08-17 的 r15 RAM-only 实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r10-20260816T181056Z.log`
（工具文件名沿用 r10，实际载入的是 `out/k1-wireless-r15`）。修复后只发送一次
OCR 查询和一次带电压窗口的 CMD5，随后 CMD3、CMD7 及 CCCR/FBR CMD52 全部完成。
串口确认 `CCCR=0x43`、SD revision=`0xf3`、function count=`1`、F1 interface
code=`0x07`。这证明 SDH1、无线电源时序和 4-bit SDIO 卡枚举已在实板跑通；它不
表示 Realtek Wi-Fi MAC/firmware 或联网已经实现。

Bluetooth H5 诊断使用官方 attach 的 10 次、每次 500 ms 的同步窗口。只有收到并
验证 `SYNC` 响应后才发送 `CONFIG`；配置响应声明 DIC 时，后续帧会使用并验证
16-bit CRC。收到控制器主动 `SYNC` 或 `CONFIG` 请求时会回对应的 H5 控制帧。
协商完成后只发送标准 HCI `Read Local Version Information`（opcode `0x1001`），并
确认可靠的 Command Complete 事件；它不发送 HCI Reset 或任何 Realtek 厂商命令。

2026-08-17 的 r16 RAM-only 验证位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r10-20260816T182359Z.log`。
Wi-Fi SDIO 枚举仍通过，但 Bluetooth H5 在同步阶段超时。该版本曾错误使用
GPIO22--25；复核用户指定的 Linux mainline pinctrl 后确认 UART2 实际为 GPIO21--24。
r17 恢复该映射，并在超时时记录 UART2 时钟/格式/状态及 H5 TX/RX 计数，以区分
“控制器未接收”与“主机未接收或帧未通过校验”。

r17 的实板日志确认 `RX bytes=0`，且 4 个完整 H5 SYNC 帧发出后 UART2 的
`LSR=0`，发送器不再报告 THRE。Linux mainline 将同一 UART 声明为
`intel,xscale-uart`；8250 驱动对 XScale UART 即使清除中断也会保留 `IER[6]` 的
Unit Enable（UUE）位。此前诊断将 IER 写为零，因而在 r18 中只置 UUE，保持
IER[0:3]=0，不接入 PLIC，也不启用任何 UART 中断。

2026-08-17 的 r18 RAM-only 验证日志位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r10-20260816T184614Z.log`
（该历史日志仍使用旧工具文件名，r18 之后的日志会按实际产物目录命名）。Wi-Fi
再次读取 `CCCR=0x43`、F1 interface code=`0x07`；Bluetooth H5 完成
`SYNC -> CONFIG` 和标准版本查询，返回 HCI version/revision=`0x0b/0x000b`、
LMP version=`0x0b`、manufacturer=`0x005d`、LMP subversion=`0x8852`，并协商 CRC。
这完成了 UART2、电源时序、115200 8E1 和 H5 基础往返的实板验收，但不代表
Bluetooth firmware 下载、设备扫描或数据传输已实现。

r19 增加了 Wi-Fi Function 1 运行时启用和 CMD53 PIO 传输代码。每笔传输限制为
byte-mode 的 1--512 bytes；由于 K1 当前仅有 polling PIO 路径，它不调用假定 DMA
回调的 NuttX 通用 CMD53 helper。r19 实板确认 CMD53 命令和 H5 回归均正常，但 Function
1 数据阶段在先设 block size、后 enable 的顺序下超时（`-ETIMEDOUT`），因此 r19 不能
标为通过。r20 将顺序修正为厂商 Linux 的 `sdio_enable_func()`、`sdio_set_block_size()`，
并在 CMD53 前单独打印 `IOEN` 与 `IORDY`。只有 `IOEN=0x02`、`IORDY=0x02` 和一个非错误
的 `F1 local` 值同时出现后，才能把 Function 1 传输层标为实板通过。

r22 在正确的 Function 1 enable/block-size 顺序下再次实测，`IOEN=0x02`、
`IORDY=0x02` 和 CMD53 的 R5 均正常，但 PIO 数据阶段在一秒内始终保持
`DATA_INHIBIT | READ_ACTIVE`，没有 `BUFFER_READ_READY`、`TRANSFER_COMPLETE` 或硬件错误。
因此不能把这个问题归因于超时时间或卡枚举，PIO 数据路径已被排除。Linux K1 SDHCI 驱动
对同一主机启用 ADMA2，并声明 `BROKEN_64_BIT_DMA`、`NO_ENDATTR_IN_NOPDESC` 与
`32BIT_ADMA_SIZE` quirk。

r23 据此把受限 CMD53 接口改为单描述符的 ADMA2 32-bit 路径：描述符为
`transfer | valid | end`（`0x23`），表地址按 8-byte 对齐，DMA 位进入
`TRANSFER_MODE`，并选择 Host Control 的 ADMA32 模式。每笔数据经过一个 64-byte
cache-line-aligned 512-byte bounce buffer，因此任意调用方缓冲区都不需要满足 DMA
对齐或 cache 一致性条件；写前 clean，读完成后 invalidate 并复制给调用方。缓存 helper
也已同时加入 SDIO 的 CMake 和 Make.defs 构建条件。该版本已在 host 上完成交叉编译、
ELF 校验和 RAM-only 包生成。

2026-08-17 的 r23 RAM-only 实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r23-20260817T023533Z.log`。
CMD53 已读回 `TRANSFER_MODE=0x13`，证明 DMA bit 已实际进入 K1 SDH1；但一秒后仍为
`INT_STATUS=0` 和 `DATA_INHIBIT | READ_ACTIVE`，与 r22 的 PIO 故障相同。Bluetooth H5
版本查询在该轮仍通过。r24 只补充 Host Control、ADMA descriptor、ADMA address 和
ADMA error 的只读采样，以区分“控制器未取描述符”与“描述符已取但卡未开始数据阶段”；
在此之前不得宣称 CMD53 或 Wi-Fi MAC 已打通。

2026-08-17 的 r24 RAM-only 实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r24-20260817T024030Z.log`。
它确认 `HOST_CONTROL=0x12`（4-bit + ADMA32）、`TRANSFER_MODE=0x13`
（DMA + block-count + read），ADMA 表地址 `0x11033108` 为 8-byte 对齐，单描述符值为
`0x00040023`（transfer + valid + end），数据地址 `0x11033140` 为 64-byte 对齐。
超时后 `ADMA_ERROR=0` 且 ADMA 地址没有前移，`INT_STATUS` 仍为零。因此 DMA 位、描述符
格式、32-bit 地址限制和 cache 维护均不能解释故障；SDH1 没有进入描述符取数阶段。后续工作
必须以运行 Linux 时同一主机的只读寄存器快照为基线，逐项对比标准寄存器和
`OP_EXT`、`MMC_CONTROL`、`TX_CONTROL`、`RX_CONTROL`、`DLINE_*` 厂商寄存器，不能通过
延长超时或提前实现 Wi-Fi MAC/固件下载来掩盖该问题。

r25 先退回到 High-Speed，实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r25-20260817T032338Z.log`。
CCCR speed 成功读回 `0x03`，SDCLK 已切至约 46.875 MHz，但 CMD53 仍保持
`INT_STATUS=0`、`PRESENT_STATE=0x01f70206`，ADMA 地址也没有变化。这排除了仅因
SDR104 高频时序导致的 CMD53 卡死。

r26 恢复并按厂商 Linux 板级 profile 配置 SDR104：CCCR speed=`0x07`、UHS=`0x07`、
Host Control2=`0x400b`、TX control=`0x803700c5`、RX delay=`184`。实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r26-20260817T033200Z.log`；CMD53
仍以与 r25 完全相同的 data-inhibit 状态超时，Bluetooth H5 版本查询继续通过。因此
这些静态 DLINE 参数已被实际写入，却不是完整的 SDR104 调谐闭环；r27 起必须先用
CMD19 标准 4-bit pattern 验证 SDH1 的调谐数据路径与 Host Control2 调谐状态，成功后
才允许再次发起 CMD53。

2026-08-17 的 r27 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r27-20260817T034330Z.log`。
CMD19 已写入 command register `0x133a`，并使 `INT_STATUS=0x20`（Buffer Read
Ready）、`PRESENT_STATE=0x01f70800` 和 `RESPONSE0=0x1e00` 出现；这证明调谐卡确实
返回了数据阶段。K1 仍没有置 `Response Complete`，原先复用的通用命令等待在消费 PIO
buffer 前超时，Host Control2 因而保留 `EXEC_TUNING`。r28 必须把 CMD19 的完成条件改为
`CMD_INHIBIT` 释放，再读取 64-byte pattern 和检查 Transfer Complete/Tuned Clock；本轮
没有重试 CMD53。Bluetooth H5 版本查询仍通过。

2026-08-17 的 r28 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r28-20260817T034546Z.log`。
它按 r27 结论跳过了缺失的 Response Complete，实际收到了 `CMD19 data ready=0x20`，
且 150 ms 后 `PRESENT_STATE` 从 `0x01f70800` 回到 `0x01f70000`，表明 PIO buffer 已被
排空。K1 没有为 CMD19 提供 Transfer Complete，因此 r28 仍以 `-ETIMEDOUT` 结束；这不是
普通 CMD53 可接受的完成语义。r29 仅为 CMD19 采用“Buffer Read Ready 后 64 bytes 已读完”
的完成条件，保留 CMD53 对 Transfer Complete 的严格要求；然后才能观察 pattern 和
Host Control2 是否完成调谐。

2026-08-17 的 r29 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r29-20260817T034804Z.log`。
它确认一条容易复现的 lower-half 顺序错误：收到 `CMD19 data ready=0x20` 后，驱动先
W1C 了该状态位，`k1_sdio_pio()` 再看 Present State 时已不能从 buffer 取数，故记录
`CMD19 data remaining=0x40`。这不表示卡未返回 pattern。r30 将顺序改为“先 PIO 读
buffer，再 W1C Buffer Read Ready”，然后再判断 16 个字、`EXEC_TUNING` 和
`TUNED_CLK`。CMD53 仍未执行。

2026-08-17 的 r30 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r30-20260817T035031Z.log`。
修正 PIO 读取/W1C 顺序后，CMD19 的 16 个 32-bit 字全部与标准 4-bit 调谐模式匹配
（`mismatches=0`），静态 RX delay=`184` 因而已被实板验证可接收 SDIO 数据。Host
Control2 从 `0x400b` 变为 `0x404b`，即 `EXEC_TUNING=1` 且 `TUNED_CLK=0`；这不是模式
传输失败，而是 K1 没有完成标准 SDHCI 自动调谐状态机。Linux 和 U-Boot 对同一控制器都
通过 CMD19 模式匹配扫描 DLINE RX delay 窗口，未将这两个 Host Control2 位作为成功条件。

r31 据此将 CMD19 改为 K1 软件调谐诊断：不设置 `EXEC_TUNING`，也不要求
`TUNED_CLK`；当前静态 delay 读回的 pattern 完整匹配即可继续尝试 ADMA CMD53。若 CMD53
仍然不进入数据阶段，下一步是实现 Linux 的 RX-delay 全窗口扫描，而不是修改自动调谐位
或延长 CMD53 超时。此结论不改变 Wi-Fi MAC/联网尚未完成的状态。

2026-08-17 的 r31 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r31-20260817T035600Z.log`。
CMD19 在静态 RX delay=`184` 下再次得到 `mismatches=0`，且 Host Control2 保持
`0x400b`；r31 因而继续执行 ADMA CMD53。Function 1 仍为 `IOEN=IORDY=0x02`，但
CMD53 在一秒后仍为 `INT_STATUS=0`、`PRESENT_STATE=0x01f70206`，ADMA error=`0`、
ADMA 地址不前进，并返回 `-ETIMEDOUT`。Bluetooth H5 本地版本查询仍通过。这证明
“自动调谐状态位阻止 CMD53”不是根因。

2026-08-17 的 r32 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r32-20260817T040141Z.log`。
它实现并实测了 K1 Linux/U-Boot 的软件 RX-delay 扫描：在 DTS 指定的 `0..254` 范围中
得到最长有效窗口 `[110, 255)`（145 点，超过 MUSE Pi Pro DTS 的 50 点门槛）。但 r32 的
中点表达式错误地把窗口起点也除以二，因而选到 delay=`127`；正确的整数中点应为
`110 + (145 - 1) / 2 = 182`。该错误不影响 CMD19 在 `127` 下的 pattern 匹配，却使
CMD53 的超时结果不能用于排除正确采样点下的 timing 问题。

r33 修复了中点计算，使用 `best_start + (best_length - 1) / 2` 选择最长窗口的中点。
2026-08-17 的 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r33-20260817T040746Z.log`。
本轮最长窗口为 `[112, 255)`、长度 `143`，因此正确选择 delay=`183`；复验 CMD19
仍为 `mismatches=0`。在这个正确采样点下，CMD53 依旧一秒后 `INT_STATUS=0`、
`PRESENT_STATE=0x01f70206`、`ADMA_ERROR=0`，ADMA 地址 `0x11034108` 未前进，且
Bluetooth H5 本地版本查询通过。RX timing、中点计算、CMD19 PIO 路径和 ADMA 描述符
格式至此均不能解释 CMD53 故障；下一步只允许针对运行 Linux 时 SDH1 的只读寄存器和
MMC request 状态做差异采集，不得宣称 Wi-Fi MAC、firmware 或联网已可用。

随后对原厂 Linux `mmc1` 做了只读快照：实际时钟为 `187500000 Hz`，4-bit、1.8 V、
SDR104；APMU AXI/SDH1 分别为 `0x411b`/`0x52`，Host Control=`0x16`、
Host Control2=`0x400b`、RX Control=`0x4`、TX Control=`0x803700c5`，DLINE 为
`0xa8b90001`（RX=`185`，TX=`0xa8`）。这些值与 r33 的目标状态一致。Linux 的软件
调谐窗口为 `[0,68)`、`[76,87)`、`[116,255)`，选 delay=`185`，也与 NuttX 的宽有效
窗口相符。

在临时 ftrace 中捕获到原厂 `rtl8852bs` 成功的 F1 CMD53：`arg=0x14220804`、4-byte read、
R5=`0x1000`、`bytes_xfered=4`、命令和数据错误均为零。厂商
[`sdio_ops_linux.c`](https://github.com/spacemit-com/linux-6.6/blob/k1-bl-v2.2.y/drivers/net/wireless/realtek/rtl8852bs/os_dep/linux/sdio_ops_linux.c)
明确该地址为 HISR（`0x1104`）；r33 使用的 `0x1000` 只是 local window 基址，不能当作
已验证的探针寄存器。

2026-08-17 的 r34 RAM-only 实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r34-20260817T042007Z.log`。
该轮 CMD53 argument 已与 Linux 完全一致（`0x14220804`，Function 1、increment、
HISR=`0x1104`、4-byte byte-mode read），但结果仍是 `INT_STATUS=0`、
`PRESENT_STATE=0x01f70206`、`ADMA_ERROR=0`，且 ADMA 地址未前进。因此错误的探针
地址已被排除，CMD53 数据路径仍未启动。

Linux 对同一稳定状态的 SDH1 同时设置 `INT_ENABLE` 与 `INT_SIGNAL_ENABLE` 为
`0x03ff010b`；NuttX 默认配置将 signal-enable 清零以避免未移植的 PLIC 路径。r35 增加了
只限 `wireless` 配置的 `CONFIG_K1_SDIO_WIFI_LINUX_SIGNAL_DIAGNOSTIC`：仅在有界 CMD53
探针期间镜像 Linux 值，并先直接屏蔽 DTS 所示的 SDH1 PLIC source 100。它不启用
`CONFIG_K1_PLIC`、不安装 ISR，CMD53 返回或取消后立即再次清零 signal-enable。该项是隔离
的控制器门槛验证。

2026-08-17 的 r35 RAM-only 实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r35-20260817T042849Z.log`。
该轮确认 `INT_SIGNAL_ENABLE` 实际读回 `0x03ff010b`，SDH1 PLIC source 100 在写入前
已被屏蔽，且全程没有外部中断或 trap；CMD53 的参数、ADMA 描述符和 H5 回归保持正常。
但数据阶段仍以 `INT_STATUS=0`、`PRESENT_STATE=0x01f70206`、`ADMA_ERROR=0` 和未前进
的 ADMA 地址超时。因此 signal-enable/PLIC 路由不是 CMD53 未启动的根因；默认
`wireless` 配置已再次关闭该诊断选项，保留它仅供可复现实验。

之后已从实际运行的厂商 `sdhci-spacemit` 驱动确认其 compatible 为
`spacemit,k1-x-sdhci`，而不是 Linux mainline 的 `spacemit,k1-sdhci`。该驱动的 SDH1
reset、set-clock 和软件调谐路径与当前 NuttX 的 BROKEN_PHY、TX/DLINE 处理相符；它没有
额外的 CMD53 私有 launch 寄存器。唯一未实测的控制器输入是 NuttX 在诊断期间仍保留了宽
`INT_STATUS_ENABLE` 掩码，而 r35 只镜像了 signal gate。诊断选项现会在 CMD53 前同时
写入 Linux 的两个 gate=`0x03ff010b`，并在成功或取消后恢复原 status gate 和清零 signal
gate。默认配置不启用该实验；下一轮 RAM-only 实板记录必须据此判断是否仍为零状态超时。

2026-08-20 的 r37 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-linux-gates-20260820T140104Z.log`。
该轮启用了 `CONFIG_K1_SDIO_WIFI_LINUX_SIGNAL_DIAGNOSTIC`，并实际读回
`INT_STATUS_ENABLE=INT_SIGNAL_ENABLE=0x03ff010b`。在此之前 F1
`IOEN=IORDY=0x02`、CMD19 pattern `mismatches=0`、Host Control=`0x16`、Host
Control2=`0x400b`；CMD53 也仍是 Linux 已验证的 HISR read
`arg=0x14220804`、transfer mode=`0x13`、block size=`0x7004`、block count=`1`。
即使两个 interrupt gate 都精确对齐，CMD53 仍以 `INT_STATUS=0`、
`PRESENT_STATE=0x01f70206`、`ADMA_ERROR=0` 和未前进的 ADMA 地址超时。故 status
gate、signal gate 和 PLIC 路由均不是数据阶段未启动的根因；默认 `wireless` 配置继续关闭
该诊断项。Bluetooth H5 本地版本查询仍通过。

同一厂商 Linux 的 `sdhci_set_transfer_mode()` 在 K1 上没有
`SDHCI_QUIRK2_SUPPORT_SINGLE`，故 4-byte 单块 ADMA 请求同样使用 transfer mode
`0x13`，不能把 NuttX 的该值当作差异。r36 改为对齐另一个实际不一致：Linux 的
`sdhci_set_block_info()` 为每笔数据使用 `SDHCI_MAKE_BLKSZ(7, length)`，即
`BLOCK_SIZE=0x7000|length`（512 KiB SDMA boundary）；NuttX 原先只写 length。该
边界字段即使对 ADMA2 通常不参与地址递增，仍是 Linux 已证实的控制器输入，r36 仅在
Wi-Fi CMD19/CMD53 路径写入该编码后重新实板验证。

2026-08-17 的 r36 RAM-only 实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-r36-20260817T043255Z.log`。
CMD19 与 CMD53 分别读回 `BLOCK_SIZE=0x7040` 与 `0x7004`，证明该 Linux 编码已实际
送入 K1；CMD19 pattern 仍为 `mismatches=0`，Bluetooth H5 版本查询也通过。CMD53 的
完成状态仍与 r34/r35 相同：`INT_STATUS=0`、`PRESENT_STATE=0x01f70206`、
`ADMA_ERROR=0`、ADMA 地址未前进。故 SDMA boundary 字段也不是阻止数据阶段的根因，
保留该 Linux 一致性修正，但不将其标记为功能修复。

此后不得继续变更 Wi-Fi MAC、firmware 或网络层，也不应继续枚举静态寄存器。下一步
必须在原厂 Linux 运行真实 RTL8852BS2 CMD53 的时刻采集 SDH1 的 `BLOCK_SIZE`、
`BLOCK_COUNT`、`TRANSFER_MODE`、`COMMAND`、ADMA 地址、`INT_ENABLE` 和
`INT_SIGNAL_ENABLE`，再与 NuttX launch trace 做逐字段对比。

2026-08-20 的 r38 源码与原厂根文件系统审计进一步缩小了边界。厂商 Linux 的
`mac_hal_init()` 先执行 `mac_pwr_switch()`，再进行 HCI/DMAC pre-init（其中包含
`dle_init()` 和 HCI flow-control 初始化），随后才执行 SDIO 的 `sdio_pre_init()`。原厂
trace 在首笔 CMD53 前记录了 1945 笔 CMD52；首笔 CMD53 是
`w_indir_cmd53_sdio_8852b()` 发出的 12-byte 间接 MAC 寄存器写
（`arg=0x9420800c`），不是 HISR 读取。相反，`sdio_pre_init()` 本身只配置 HCI pad、
TX format、RX interrupt read mask 和 TX CRC report，不能替代完整电源、DLE、DMA 和
firmware 下载流程。

用户已明确选择 GPL-2.0 交付路线。r39 因而新增了边界清晰的
`k1_rtl8852bs_gpl.[ch]`：它通过 Function 1 CMD52 实现 Realtek 的本地/间接寄存器
访问，并移植 `mac_pwron_8852b` 中适用于 SDIO 的掩码读改写和轮询项，明确排除唯一的
PCIe-only `0x0071` 项。它在 Function 1 enable 后、既有 HISR CMD53 probe 前运行；所有
错误和轮询都有上界。无线 defconfig 启用该组件，且打包流程会随 ELF 提供 GPL 文本及
组件源码。

2026-08-20 的 r39 RAM-only 实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-gpl-package-20260820T145637Z.log`。
Function 1 已确认 `IOEN=IORDY=0x02`，CMD19 软件调谐通过，且日志完整出现
`K1 Wi-Fi GPL: RTL8852BS2 bootstrap begin` 和 `... bootstrap complete`；同轮 Bluetooth
H5 本地版本查询仍通过。因此上电表和本地/间接 CMD52 访问已完成实板验收。但随后的
HISR CMD53 仍返回 `-ETIMEDOUT`，超时快照保持 `INT_STATUS=0`、
`PRESENT_STATE=0x01f70206`，ADMA 地址未前进。r39 不能表示 Function 1 数据通道、
Wi-Fi MAC 或联网已完成。

r40 只增加厂商 `sdio_pre_init()` 中不依赖 DLE 的三项 SDIO 配置：HCI data-pad SMT、
CMD53 aggregate TX format/RX interrupt read mask，以及 TX CRC report。厂商
`sd_reset` 在本板 profile 中为 `IGNORE`，故不执行。它仍通过已有的 CMD52 本地/间接
路径执行，并位于 r39 power bootstrap 之后、HISR CMD53 probe 之前；尚未上板验收。

r41 补齐了厂商在 `sdio_pre_init()` 之前无条件执行、且不依赖 firmware/DLE 的 HCI/DMAC
pre-init：`R_AX_HCI_FUNC_EN` 仅置 HCI TX/RX DMA enable；
`dmac_func_pre_en_8852b()` 按原值写入 `R_AX_DMAC_FUNC_EN=0x60440000` 和
`R_AX_DMAC_CLK_EN=0x00040000`。它处于 r39 power bootstrap 与 r40 SDIO pre-init 之间，
仍通过 CMD52 间接访问。DLE FIFO 配额、HCI flow-control、firmware 下载、802.11 MAC
和网络层继续明确排除在本增量外。

2026-08-20 的 r41 RAM-only 实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-gpl-r41-package-20260820T151908Z.log`。
该轮已再次确认 Function 1 `IOEN=IORDY=0x02`、CMD19 pattern
`mismatches=0`、r39 power bootstrap complete，以及 Bluetooth H5 local-version
exchange 和 `nsh>`。但 HCI/DMAC pre-init 的第一项
`R_AX_HCI_FUNC_EN (0x8380)` 通过 CMD52 间接访问时在 ready 轮询中返回
`-ETIMEDOUT`；串口明确记录
`K1 Wi-Fi GPL: SDIO pre-init address=0x8380 error=0x6e`，随后板级 bring-up 返回
`-110`。因此 r41 未到达 DMAC、SDIO pre-init 或 HISR CMD53 probe，不能把这轮与
r39 的 CMD53 超时混为同一结果。厂商源码确认其 `SDIO_WAIT_CNT` 同样为 50；根因是
r41 在 `mac_pwr_switch()` 之后仍以 CMD52 间接访问，而厂商在 power-on 状态对这些对齐
32-bit MAC/SDIO 寄存器改用 CMD53。下一步是以有界 4-byte CMD53 访问复现该
post-power 路径，再重新执行 RAM-only 验证；不能先实现 firmware、MAC 或网络层。

r42 将 post-power 的 HCI/DMAC 和 SDIO pre-init 访问改为 Function 1、递增、对齐
4-byte CMD53，保留 r39 power table 的 CMD52 路径不变。2026-08-20 的 RAM-only
记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-gpl-r42-package-20260820T153418Z.log`。
本轮完整通过 Function 1 `IOEN=IORDY=0x02`、CMD19 `mismatches=0`、r39 bootstrap、
Bluetooth H5 local-version exchange 和 `nsh>`。HCI/DMAC pre-init 的首个 CMD53
确实以 Linux 对齐访问模型发出：`arg=0x15070004`（F1、increment、4-byte read、
`R_AX_HCI_FUNC_EN=0x8380`）、transfer mode=`0x13`、block size=`0x7004`、block
count=`1`、Host Control=`0x16`，但以 `-ETIMEDOUT` 结束；`INT_STATUS=0`、
`ADMA_ERROR=0`、ADMA 地址未前进。更重要的是 command launch 前的
`PRESENT_STATE=0x01f70206` 已含 `DATA_INHIBIT|READ_ACTIVE`，超时后仍相同。
因此 CMD52/CMD53 transport 选择已被排除；r42 未到达 DMAC、SDIO pre-init 或 HISR
probe，下一步应审计 CMD19 完成后的 SDHCI data-line cleanup/reset 与厂商流程的差异，
而不是继续实现 firmware、MAC 或网络层。

2026-08-20 的 r43 RAM-only 记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-gpl-r43-package-20260820T154200Z.log`。
它在每一笔成功的 CMD19 PIO 调谐后执行有界 `RESET_DATA`；日志中的
`CMD19 cleanup present=0x01f70000` 证明此前的调谐 data-active 状态已清除。但 r43
进入 GPL bootstrap 后的首笔 CMD53 仍以同一 `0x01f70206` 状态超时，因此 CMD19 残留
不是 CMD53/ADMA 未启动的根因。

r45 将 GPL 的 post-power 32-bit 寄存器访问改为四次递增 Function 1 CMD52。2026-08-20
的实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-gpl-r45-package-20260820T154801Z.log`：
`bootstrap complete`、`HCI/DMAC pre-init complete` 和 `SDIO pre-init complete` 均已
出现。这证明已移植的 Realtek 上电表及三个不依赖 firmware/DLE 的配置阶段可由 K1
CMD52 控制路径完成。该版本随后仍主动运行 HISR CMD53 探针，并再次得到原有的 ADMA
超时；因此这不证明 Function 1 数据通道已可用。

r46 将板级 HISR 验收也改为四次 CMD52，防止一个已知失败的 CMD53 诊断覆盖已完成的
上电状态。2026-08-20 的 RAM-only 记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-gpl-r46-package-20260820T155047Z.log`，
对应 ELF SHA-256 为
`33a09d05f3e0ee30e3144e4b159a2acaf2955e2e1f81c4dd951bbf2e5b5fe434`。本轮实测
`IOEN=IORDY=0x02`、CMD19 `mismatches=0`、上述三个 GPL 阶段完成、
`F1 HISR CMD52=0x00000000`、Bluetooth H5 local-version exchange 和 `nsh>`；未出现
`K1 RTL8852BS2 bring-up failed`。CMD53/ADMA、DLE、firmware、Wi-Fi MAC 和网络层仍
未实现或未验收，不能由 r46 的成功状态推断出来。

r49 以仅限临时诊断的 `CONFIG_K1_SDIO_WIFI_CMD53_COMMON_DIAGNOSTIC` 在 r46 的成功
基础上增加了一笔 Function 0、地址 0--3 的只读 CCCR CMD53。2026-08-20 的 RAM-only
记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-gpl-r49-package-20260820T160405Z.log`，
对应 ELF SHA-256 为
`986cc12ecde83bf3c98fd68396a163ed51986035c1714eec9249905d24cdfdd2`。它实际发出
`CMD53 arg=0x04000004`、`TRANSFER_MODE=0x13`、`BLOCK_SIZE=0x7004`、
`BLOCK_COUNT=1`，但仍以 `-ETIMEDOUT` 结束，且快照为
`PRESENT_STATE=0x01f70206`、`INT_STATUS=0`、`ADMA_ERROR=0`、ADMA 地址未前进。
Bluetooth H5 本地版本查询仍通过。

这排除了 Function 1 enable、Realtek 本地寄存器、GPL bootstrap 和 Realtek 寄存器地址
作为 CMD53 数据阶段未启动的根因：K1 SDH1 在 Function 0 的标准 CCCR 读上同样没有开始
数据阶段。该诊断选项已从默认 `wireless` defconfig 移除，保留为默认关闭的、RAM-only 的
复现实验。此结论成立前，不再实现 Wi-Fi firmware、MAC、`netdev`、关联或联网；应将后续
工作限制在 K1 SDH1 CMD53 传输根因的原始硬件/控制器证据上。

为区分 ADMA 与 PIO，新增了独立的
`CONFIG_K1_SDIO_WIFI_CMD53_PIO_DIAGNOSTIC` 配置和
`configs/wireless_cmd53_pio_diag` profile。它只发起 Function 0、地址 0、递增、4-byte
只读 CMD53，并在发命令前清除 Host Control 的 DMA selector；默认 `wireless` 和原有
ADMA/F0 profile 均不受影响。同时修正了通用 PIO 完成路径的顺序：必须先消费 Buffer
Read/Write Ready 对应的 buffer，再写一清状态位。该 profile 需要一次新的 RAM-only 实板
回归；在获得串口结果前，不把它称为 CMD53 修复。

2026-08-21 的 PIO profile 已完成一次有效 RAM-only 实板回归，记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-pio-20260821T073510Z.log`。
它先通过 CMD19 PIO pattern（`data ready=0x21`、`mismatches=0`）、Function 1 enable、
GPL CMD52 bootstrap 和 H5 local-version exchange；随后 F0 CMD53 的 Host Control 为
`0x06`，明确没有 ADMA selector，argument 为 `0x04000004`，transfer mode 为 `0x12`，
block size/count 为 `0x7004`/`1`。该请求仍在一秒后以 `-ETIMEDOUT` 结束，
`INT_STATUS=0`、`PRESENT_STATE=0x01f70206`，且从未出现 Buffer Read Ready。因此
CMD53 的问题不是 ADMA 描述符、缓存维护或 PIO buffer 消费顺序；K1 SDH1 在该标准
Function 0 请求上没有启动正常数据阶段。

同日尝试将 Linux `OP_EXT` 的 `OVRRD_CLK_OEN|FORCE_CLK_ON`（`0x1800`）置入 SDH1
全复位路径。寄存器确实变为 `0x75091c00`，但下一笔 CMD5 立即报 `INT_STATUS=0x18000`
超时，故该方案已撤回，不能进入默认 reset 路径。PIO profile 保留了仅在 CMD53 发起前
设置该位的隔离实验；尚未在一次干净的 CMD5 成功启动中取得该实验的结果。

2026-08-21（Asia/Shanghai）使用默认 `wireless` defconfig 的 GPL 包完成实板回归，串口
记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-gpl-default-20260820T173319Z.log`，
ELF SHA-256 为
`5fc851f1aca11972227c1273225b07bf10265693ba2a00a53321c32286840e82`。本轮确认
`CMD5 OCR=0x90ffffff`、`CCCR=0x43`、F1 `IOEN=IORDY=0x02`、CMD19
`mismatches=0`；GPL bootstrap、HCI/DMAC pre-init 和 SDIO pre-init 均输出
`complete`，F1 HISR CMD52 读取为零，Bluetooth H5 Local Version 返回
`HCI=0x0b`、`manufacturer=0x005d`、`subversion=0x8852`，最终进入 `nsh>`。
该包未启用 Function 0 CMD53 临时诊断，也没有宣称 CMD53、Wi-Fi MAC、固件下载或联网
成功。

2026-08-21 对 K1 SDH1 的 request recovery 做了一个最小驱动修正：超时或调谐事务
取消时，`k1_sdio_cancel()` 现在按 Linux `REQUEST_ERROR` 的边界同时复位 command 和
data 状态机，随后清除 pending interrupt、signal-enable 和 lower-half 的 buffer/
event 状态。该修正对应新的 wireless 包，ELF SHA-256 为
`bd1d39b76f307d6ae8b6b3373cded7a68978fd7e57e908e17dc9088e307331dc`；主机完整构建和
静态 CI 均通过。一次串口 RAM-only 回归因板卡未产生 RST/U-Boot 输出而未取得实板
结果，因此不能把该修正宣称为 CMD53 功能修复，也没有执行任何持久写入。

这仍不是完整 Wi-Fi 驱动：没有 DLE/HCI flow-control、firmware 下载、802.11 MAC、关联 AP
或 NuttX `netdev`。因此 SDH1/CCCR/H5 诊断和 GPL bootstrap 都不能表示可联网。

### 2026-08-21: 外部实现交叉核对与实板结论

重新核对可工作的 K1 实现后，不能把 Bianbu U-Boot 的 `HOST_CONTROL2` 扩展位直接当作
修复方向：其专用驱动会设置 bits 10--14，但从本板原厂 Linux 采集到的运行态
`HOST_CONTROL2=0x400b` 并未保留其 V4/addressing 位。更有价值的共同硬件证据来自
[Linux mainline `sdhci-of-k1.c`](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci-of-k1.c)：
对 `MMC_CAP2_NO_MMC` 的 SDIO host，在每次 `RESET_ALL` 后按顺序恢复 PHY/PAD，置
`LEGACY_CONTROL.GEN_PAD_CLK_ON`，并置
`OP_EXT.OVRRD_CLK_OEN|FORCE_CLK_ON` (`0x1800`)。

此前的失败实验只在 NuttX 复位路径中孤立写入 `OP_EXT`，下一笔 CMD5 即超时；它没有
同时恢复 Linux mainline 的 PHY/PAD/legacy-pad 状态，不能否定完整序列。新增默认关闭的
`CONFIG_K1_SDIO_WIFI_MAINLINE_RESET_DIAGNOSTIC` 和
`wireless_cmd53_mainline_reset_diag` profile，将该完整序列限制为既有的 Function 0 PIO
CMD53 RAM-only 探针。验收标准始终是出现 Buffer Read Ready 或 Transfer Complete，而
不是仅仅 CMD5/CMD52 成功。

该 profile 已于 2026-08-21 完成实板 RAM-only 验证，完整串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-mainline-reset-20260821T084339Z.log`。
实测复位后 `OP_EXT=0x75091c00`，包含 mainline 要求的 `0x1800`；CMD5
`OCR=0x90ffffff`、CMD52 枚举、4-bit SDR104 软件调谐（零 pattern mismatch）、Function 1
`IOEN=IORDY=0x02` 和 GPL CMD52 pre-init 均正常，因而本轮是完整 reset 序列的有效 A/B。
但随后只读的 Function 0、地址 0、4-byte PIO CMD53 仍在一秒后超时：
`INT_STATUS=0`、`PRESENT_STATE=0x01f70206`，没有 Buffer Read Ready 或 Transfer Complete，
最终为 `F0 CCCR CMD53 error=0x6e`。Bluetooth H5 本地版本查询仍返回
`HCI=0x0b`、`manufacturer=0x005d`、`subversion=0x8852`。因此 PHY/PAD、legacy pad
clock 与 `OP_EXT=0x1800` 的完整 Linux-mainline reset 也被排除为当前 CMD53 数据阶段的
根因；该配置必须保持默认关闭、仅用于诊断，不能进入默认 `wireless` 配置。

同一次复位返回原厂 Linux（`6.6.63`）后，ADB 的只读采集表明它也没有可用于 CMD53
比较的工作基线：`8852bs` 模块加载后，`mmc1` 连续四次报告
`tuning execution failed: -5`，随后 `Failed to initialize a non-removable card`。运行时
`/sys/kernel/debug/mmc1/err_stats` 的 `Tuning Error Occurred` 为 4，`mmc1` 最终
clock/power 都回到 0，`/sys/bus/mmc/devices` 中也没有任何 `mmc1:*` 卡设备或 WLAN
netdev。因此不能把该原厂内核称为可工作的 Wi-Fi/CMD53 参考实现。

该采集还发现运行时 FDT 与检出的厂商源码树不一致：实板 SDH1 的
`spacemit,tx_delaycode` 为 `<0xa8 0x78>`，`pinctrl-names` 只有 `default`；而源码中的
MUSE Pi Pro DTS 是单值 `<0x9f>`。厂商 `spacemit_sdhci_set_clock()` 在 SDR104 时会查询
`fast` pinctrl，故实板 dmesg 还出现 `could not get sdhci fast pinctrl state`。后续不得把
本地 DTS 当作原厂运行状态的唯一依据，应先从 `/sys/firmware/fdt` 或 device-tree sysfs
采集实际启动 DT。

最后，NuttX PIO CMD53 使用的 `TRANSFER_MODE=0x12`（block-count enable + read）与该
Linux `sdhci_set_transfer_mode()` 一致：K1 host 没有
`SDHCI_QUIRK2_SUPPORT_SINGLE`，即使单块请求也会保留 block-count enable。它不是此次
`INT_STATUS=0` / 数据阶段未启动的差异点。

本轮实际比对的来源和结论如下：

- [SpacemiT/Bianbu U-Boot `spacemit_sdhci.c`](https://github.com/spacemit-com/uboot-2022.10/blob/k1-bl-v2.2.y/drivers/mmc/spacemit_sdhci.c)
  与 Bianbu 镜像代码逐字一致，但其 compatible 是 `spacemit,k1-pro-sdhci`，并且板上
  U-Boot 启动日志只 probe 了 `sdh@d4280000` 和 `sdh@d4281000`，没有实际驱动 Wi-Fi 的
  `sdh@d4280800`。它是 SDHCI 控制器扩展位的参考，不是 SDH1/RTL8852BS2 已验证流程。
- 厂商 [Linux `sdhci-of-k1x.c`](https://github.com/spacemit-com/linux-6.6/blob/k1-bl-v2.2.y/drivers/mmc/host/sdhci-of-k1x.c)
  是 MUSE Pi Pro 原厂镜像实际使用的路径；现有 NuttX 的 BROKEN_PHY、TX/DLINE 和软件
  RX 调谐已按它对齐。它没有额外的 CMD53 launch 寄存器，不能解释标准 Function 0 PIO
  CMD53 完全不启动。
- Buildroot、OpenWrt、Arch 等 K1 发行版只封装或复用上述厂商 Linux 内核。公开
  `open-vela/vendor_SpacemiT` 目前也没有 K1 SDIO 源码，因此没有可直接移植的 NuttX、Zephyr、
  FreeRTOS 或 RT-Thread K1 SDIO 实现。后续应以这个受控 A/B 实验和实板 trace 推进，而非
  假定存在未找到的 RTOS 驱动。

## 3. 明确未实现

- RTL8852BS2 Wi-Fi firmware 下载、MAC、关联 AP、IPv4 配置和 `netdev` 注册；
- RTL8852BS2 Bluetooth 厂商 firmware/config 下载、HCI Reset、通用 HCI 命令/数据传输、
  Bluetooth host stack 与扫描验证；
- Wi-Fi/蓝牙低功耗唤醒、SDIO 中断、吞吐与长期稳定性验证；

上面三条是本项目开始时的范围声明，**不随进展改写**；哪些已经实板通过一律以第 5 节的
逐次运行记录为准（例如 firmware 全量下载、runtime MAC/BB/RF、2.4 GHz 1–13 被动扫描已在
运行 8/11/12 实板通过）。到 2026-08-29（续七）为止，`netdev` 的现状是：只有诊断 profile
`wireless_wlan0_scan_diag` 里的**只报告扫描结果**的 `wlan0`，且它**还没有实板验收**；
关联 AP、认证、密钥、RSSI、TX 与 IPv4 配置仍然全部未实现。

因此，CCCR/FBR 可读只证明 SDIO 卡选择、4-bit 总线设置和标准 function 描述符
可用；`K1 Bluetooth: H5 local version ...` 只证明 UART2、电源时序、H5 基础协商
和一次标准 HCI 事件往返在该次启动中成功。两者都不代表联网、关联 AP、可扫描设备
或数据传输。

`/dev/ttyHCI0` 由 K1 的 UART2 H5 lower-half 和 NuttX `bt_slip` 注册。专用
`wireless_bt_hci_only_diag` 配置关闭原始 local-version probe，让 `bt_slip` 从控制器
复位后独占 H5 初始化；它打开无线调试日志，仅用于实板确认 H5 active 状态，不代表已具备
厂商启动、通用 HCI 通信或扫描能力。

## 4. UART 风险隔离

历史实板记录表明 UART0 在 S-mode 写 IER 存在 APB 挂死风险。无线配置采取以下
隔离措施：

- 从不修改 `k1_console.c`，UART0 继续使用 U-Boot 继承的 polling console；
- UART2 仅在独立 `wireless` 配置中配置，IER 只置 XScale Unit Enable 的 bit 6，
  所有 UART 中断位保持为零，不接入 PLIC；
- H5 诊断的 TX 等待有 100 ms 上限，控制器未就绪时返回错误，不无限卡死；
- 同步失败仅使本次 `wireless` bring-up 返回错误，不写入 U-Boot 环境或持久存储。

## 5. 构建与实板验收

从参赛仓目录执行：

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless \
  --build-dir cmake_out/k1-wireless \
  --no-check \
  --jobs 8
```

上板前仍采用 `docs/K1_REAL_BOARD_HANDOFF.md` 的 RAM-only U-Boot 载入流程，
不得执行 `saveenv`、`mmc write`、`mmc erase` 或 FDL/fastboot 写入。首轮串口验收
应依次确认：

1. 正常进入 NSH，UART0 日志未退化；
2. 记录 Wi-Fi CMD5 OCR、CCCR revision、function 数量及 F1 interface code；
3. 出现 `K1 Bluetooth: H5 local version HCI=...`，记录 HCI revision、LMP
   subversion 和 CRC 标志，且未出现
   `K1 RTL8852BS2 bring-up failed`；
4. 如果 H5 或标准版本查询失败，保留完整串口日志和错误码，不继续发送厂商命令；
5. 只有取得与板卡匹配的 `rtl8852bs_fw`、`rtl8852bs_config` 且 H5 可靠传输已完成
   独立测试后，才实现固件下载与 Bluetooth stack 注册。

若 Type-C 没有枚举 ADB，但 USB-TTL 已接入 `/dev/ttyUSB0`，可在主机运行：

```bash
tools/run_k1_wireless_smoke.py \
  --manual-reset \
  --device auto \
  --wrapper out/k1-wireless-gpl-default/k1-go-wrapper.bin \
  --payload out/k1-wireless-gpl-default/contest-nuttx-flat.bin \
  --require-h5 \
  --boot-timeout 60
```

该命令可以在板子已停在 Linux `login:` 或 NuttX `nsh>` 时先打开；它会丢弃已有提示符并等待
下一次启动。手动复位模式会在串口监听已打开后默认播报“请按一下复位按钮”；只在听到该提示或输出
`press RST` 后短按一次板上 `RST`。工具识别到主 U-Boot 的
`In:    serial` 后等待 150 ms，只发送一个停止字符；抢到提示符后清除残留命令行，再通过
XMODEM 载入 wrapper 和 payload 并执行 `go`。它不使用 ADB，也不会写入 U-Boot 环境、eMMC、FDL
或 fastboot。正常启动的 PWR Type-C 不枚举 USB/ADB 是该板已知的硬件边界，不应作为失败判据。
`--device auto` 优先使用 `/dev/serial/by-id/`。手动 RST 等待和 NSH `reboot` 阶段会在
USB-TTL 重新枚举后重新打开这个稳定链接，因此从 `ttyUSB0` 切换至 `ttyUSB1` 不会丢失
后续 U-Boot 控制台。

### PIO A/B 诊断（已完成）

在继续实现 firmware、MAC 或 `netdev` 之前，先使用独立的 PIO profile 区分
“CMD53/ADMA 配置错误”和“K1 SDH1 根本没有启动 CMD53 数据阶段”两类问题：

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless_cmd53_pio_diag \
  --build-dir cmake_out/k1-wireless-cmd53-pio \
  --no-check --jobs 8

K1_ELF=cmake_out/k1-wireless-cmd53-pio/nuttx \
K1_CONFIG=cmake_out/k1-wireless-cmd53-pio/.config \
K1_PACKAGE_DIR=out/k1-wireless-cmd53-pio \
tools/package_k1_bringup.sh

./tools/run_k1_wireless_smoke.py \
  --nsh-reboot --device /dev/ttyUSB0 \
  --wrapper out/k1-wireless-cmd53-pio/k1-go-wrapper.bin \
  --payload out/k1-wireless-cmd53-pio/contest-nuttx-flat.bin \
  --require-h5 --boot-timeout 180
```

重点保存完整串口日志中的 `data PIO host=`、`data mode=`、`data ready status=`、
`data timeout present=` 和 `F0 CCCR CMD53=`。如果出现 `F0 CCCR CMD53=`，说明
PIO 数据阶段可用，应回到 ADMA profile 单独定位 DMA 描述符或缓存问题；如果 PIO
同样超时且没有 `data ready`，问题仍在 SDH1/卡片数据阶段启动之前，不能继续堆叠
Realtek firmware 或网络层；如果出现 `data ready` 但最终超时，则重点检查 PIO
消费顺序、状态位清除和 Transfer Complete。该 profile 还会仅在 CMD53 发起前置
Linux `OP_EXT` 的 output-clock 位；日志应出现 `data PIO OP_EXT=0x...1c00`。实板已证明
在全复位阶段置这些位会使 CMD5 超时，因此它们不得进入默认 reset 路径。该测试仍是
RAM-only，不执行持久烧录。
`--nsh-reboot` 会通过已启用的 NSH `reboot` 命令触发 K1 板级 watchdog reset 并自动
抢 U-Boot；若当前运行的是 stock Linux，应省略该参数，让工具经 ADB 重启，或改用
`--manual-reset` 后再按 RST。

### Legacy-clock CMD53 A/B（已完成，未解决 CMD53）

`wireless_cmd53_legacy_diag` 在 Function 1 已启用、block size 已设置后保留四位
legacy transfer divider（约 11.7 MHz），不写 CCCR High-Speed/UHS、不执行 CMD19
调谐；随后执行与 PIO profile 完全相同的只读 Function 0、地址 0、4-byte CMD53。
它用于把“高速/UHS 时序问题”和“任何已枚举时钟下都无法启动 CMD53 数据阶段”分开。
2026-08-21 的 RAM-only 实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-legacy-20260821T085453Z.log`。
日志确认 legacy CMD53 clock=`0x1007`、CCCR speed=`0`、High-Speed=`0`、UHS=`0`、
SDR104=`0`，并且 `data PIO host=0x02`，即请求实际采用 4-bit、非 High-Speed 的
legacy 时序。Function 0、地址 0、4-byte PIO CMD53 仍在一秒后以
`INT_STATUS=0`、`PRESENT_STATE=0x01f70206` 和 `F0 CCCR CMD53 error=0x6e` 结束，
没有 Buffer Read Ready 或 Transfer Complete；Bluetooth H5 本地版本查询仍通过。

因此 High-Speed/UHS/SDR104 时序不是当前 CMD53 数据阶段未启动的根因。该配置继续只适合
RAM-only 诊断，不能用于 Wi-Fi 功能宣称或默认无线配置；后续实验必须针对尚未排除的
更低层控制器差异。

### Linux-mainline reset A/B（已完成，未解决 CMD53）

这个 profile 保持 PIO、Function 0、地址 0、4-byte CMD53 不变，只把 SDH1 的 reset
follow-up 改为 Linux mainline SDIO 序列，用于分辨 `OP_EXT` 是否必须与 legacy pad 和
PHY/PAD 同时生效：

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless_cmd53_mainline_reset_diag \
  --build-dir cmake_out/k1-wireless-cmd53-mainline-reset \
  --no-check --jobs 8

K1_ELF=cmake_out/k1-wireless-cmd53-mainline-reset/nuttx \
K1_CONFIG=cmake_out/k1-wireless-cmd53-mainline-reset/.config \
K1_PACKAGE_DIR=out/k1-wireless-cmd53-mainline-reset \
tools/package_k1_bringup.sh
```

实板已确认 `OP_EXT=0x75091c00`（含 `0x1800`）、CMD5/CMD52/调谐均通过；Function 0
PIO CMD53 仍以零状态超时，详细证据见上文的“外部实现交叉核对与实板结论”。因此完整
Linux-mainline reset 已被排除，profile 继续保持诊断用途，不能写进默认 wireless 配置。

### PIO CMD53 controller-recovery A/B（待验收）

`wireless_cmd53_pio_retry_diag` 在原有 Function 0、地址 0、4-byte PIO CMD53
诊断上只增加一个变量：首笔返回错误后，`k1_sdio_wifi_read()` 已按 SDHCI 请求错误边界
复位 command/data 状态机；profile 随后重发完全相同的只读请求一次。它不写 CCCR、
不触碰 Realtek MAC 寄存器、不加载 firmware，也不修改默认 wireless 配置。

只有 `F0 CCCR CMD53 retry=` 才表示请求恢复后数据阶段开始；`retry error=` 则说明
controller recovery 不能解释首笔 CMD53 没有数据状态的故障。无论结果如何，它都不能单独
证明 Wi-Fi 可用。

2026-08-21 新增了默认关闭的
`CONFIG_K1_SDIO_WIFI_CMD53_WRITE_DIAGNOSTIC`。启用该项后，GPL bootstrap 在
CMD52 上电表、HCI/DMAC 和 SDIO pre-init 完成后，先通过 CMD52 读取
`HCI_OPT_CTRL (0x0074)`，再构造一个不改变该寄存器值的 12-byte Function 1
CMD53 写请求。请求固定为递增地址 `0x1040`、byte mode、length `12`，所以 SDIO
argument 应为厂商 Linux 已观测的 `0x9420800c`。它用于区分“控制器从未启动
CMD53 数据阶段”和“Realtek MAC 后续寄存器/固件问题”，不下载固件、不注册
`netdev`，也不代表 Wi-Fi 已完成。

启用方法是在无线 defconfig 中临时加入：

```text
CONFIG_K1_SDIO_WIFI_CMD53_WRITE_DIAGNOSTIC=y
```

然后重新构建并使用 `/dev/ttyUSB0` 的 RAM-only 流程上板。验收时重点保存
`CMD53 write diagnostic error=`、`data mode`、`data arg`、`data block size`、
`ADMA address`、`ADMA error`、`data timeout present` 等行；成功只意味着这笔
12-byte 写完成，仍不能跳过 DLE/HCI flow-control、firmware、MAC 和网络层。

该 ADMA profile 已于 2026-08-21 在原厂 Linux 的 ADB 重启基线完成 RAM-only
实板回归，记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-write-20260821T090701Z.log`，
ELF SHA-256 为 `b5c4067ccfabb722287d4bd718608ee805de80cf649dd2`。CMD5
`OCR=0x90ffffff`、CMD19 `mismatches=0`、F1 `IOEN=IORDY=0x02` 和 GPL CMD52
bootstrap/HCI-DMAC/SDIO pre-init 均通过。随后该写请求实际发出
`arg=0x9420800c`、`TRANSFER_MODE=0x03`、`BLOCK_SIZE=0x700c`、Host Control=`0x16`
（4-bit + High-Speed + ADMA32）；控制器报告 `INT_STATUS=0x00208003`，即 Command
Complete、Transfer Complete、Error 和 SDHCI Data CRC Error，最终返回 `-EILSEQ`
（日志中的 error=`0x54`）。这证明写方向的 CMD53 数据阶段已经启动并结束，但 CRC
校验失败；它不表示该间接寄存器写成功，也不能由此推出 Wi-Fi 可用。

为区分 ADMA 数据路径和写方向的 SDIO 物理/时序问题，新增
`wireless_cmd53_write_pio_diag`，它继承上述 profile 且只让同一笔 F1/`0x1040`/
12-byte 请求使用 PIO；它与 Function 0 PIO profile 互斥。2026-08-21 的 RAM-only
实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-write-pio-20260821T091146Z.log`，
ELF SHA-256 为 `5af4458be70ccd2c7b679826121134913e050d40c709616894177bdb79e67fe9`。
本轮同样通过 CMD5、CMD19、F1 enable 与 GPL CMD52 阶段；写请求为
`arg=0x9420800c`、`TRANSFER_MODE=0x02`、Host Control=`0x06`，明确没有 DMA。控制器先
报告 Buffer Write Ready=`0x10`，随后报告 `INT_STATUS=0x00208002`，即 Transfer Complete、
Error 与 SDHCI Data CRC Error，最终为 `-EIO`（error=`0x5`）。因此 ADMA 描述符、cache
维护和 DMA 位不是本次 CMD53 写 CRC 的根因；该 PIO 写也没有成功。

`wireless_cmd53_write_legacy_pio_diag` 保持相同 PIO 写请求，但在 Function 1 启用后保留
约 11.7 MHz 的四位 legacy 时钟，不选择 High-Speed/UHS/SDR104。2026-08-21 的 RAM-only
实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-write-legacy-pio-20260821T091745Z.log`，
ELF SHA-256 为 `73a01c904f42b4df41efe6f15bf466c7ea52a524d1a9852774ccca180105d8f2`。
该轮实际读回 legacy clock=`0x1007`、Host Control=`0x02`，且 PIO 写仍为
`TRANSFER_MODE=0x02`。控制器先报告 Buffer Write Ready=`0x10`，随后为
`INT_STATUS=0x00208002`（Transfer Complete、Error、Data CRC Error），最终返回 `-EIO`
（error=`0x5`）。

因此 High-Speed/UHS/SDR104 不是本次写方向 CRC 的根因；ADMA、cache、DMA selector 和
高速时钟均已排除。随后 `wireless_cmd53_write_legacy_tx_dline_pio_diag` 保持相同 legacy
PIO 写请求，但在 CMD53 前设置了原厂 Linux tuning path 的 TX MUX、DLINE power-up、TX
dline register=`0` 和 TX delay=`0xa8`。2026-08-21 的 RAM-only 记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-write-legacy-tx-dline-pio-20260821T092522Z.log`，
ELF SHA-256 为 `ff2c2f86b1faacfef270ecde7934d81d4c8eef45bf99d73dd98529b30d6d754c`。
日志实际读回 `TX_CFG=0xc03700c5`、`DLINE_CTRL=0xa8000001`、`DLINE_CFG=0`，而 legacy
clock 仍为 `0x1007`；这证明 A/B 的 TX/DLINE 设置已经生效。结果依旧是 Buffer Write
Ready 后的 `INT_STATUS=0x00208002` 和 `-EIO`，H5 local-version 仍通过。因此遗漏 TX
delay-line 初始化也不是当前写方向 CRC 的根因。

Function 0 CMD53 读并不是 Realtek 运行时的典型数据请求，下一项独立的
`wireless_cmd53_hisr_read_diag` 改为在 GPL CMD52 bootstrap 后只读 F1 HISR `0x1104` 的
四个递增字节。它复现原厂 Linux trace 的 `arg=0x14220804`，使用正常的 ADMA/SDR104
profile，不写寄存器、不加载 firmware，且只有出现 `F1 HISR CMD53=` 才可判定该笔传输
通过。无论结果如何，它仍不足以表示 Wi-Fi MAC、firmware 或联网可用。

### 2026-08-21：Function 1 HISR CMD53 读与 RX DLINE A/B（均失败）

`wireless_cmd53_hisr_read_diag` 已完成 RAM-only 实板验证，完整串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-hisr-read-20260821T093148Z.log`，
ELF SHA-256 为
`bc0e20fe559396c0ca82ff0f421a7ffa4eeb6dc3974a7829d54cda868fe0e42b`。
请求经过 CMD5、Function 1 enable、CMD19 调谐和 GPL bootstrap/HCI-DMAC/SDIO pre-init 后，
实际发出原厂捕获的 F1 HISR 请求：`arg=0x14220804`、`TRANSFER_MODE=0x13`、
`COMMAND=0x353a`、`BLOCK_SIZE=0x7004`、`BLOCK_COUNT=1`、Host Control=`0x16`。
控制器在一秒内没有置任何完成或错误状态，快照保持
`PRESENT_STATE=0x01f70206`、`INT_STATUS=0`、`ADMA_ERROR=0`，最终
`F1 HISR CMD53 error=0x6e`（`-ETIMEDOUT`）。H5 local-version 仍返回
manufacturer=`0x005d`、subversion=`0x8852`。

本轮从原厂 Linux 的 `/sys/devices/platform/soc/d4280800.sdh/tx_delaycode` 读取到 SDH1
当前 TX code 为 `0xa8`；CPU 是 1.6 GHz、1.05 V。原厂 `sdhci-of-k1x.c` 的规则也是高于
950 mV 时选择 DTS 数组的第一个值 `0xa8`。其闲置 SDH1 原始寄存器
`OP_EXT=0x75090400`、`TX_CFG=0x403700c5`、`HOST2=0x4008` 与 NuttX 调谐前状态一致，
而 NuttX 的 SDR104 路径已设置 TX MUX、TX=`0xa8`、DLINE register 0。因此 TX code 不是
这个 HISR 超时的遗漏项。

历史成功 trace 的 RX code 是 `0xb9`，本轮本地 CMD19 扫描选择了 `0xb7`。为排除这个唯一
剩余的可观测时序差异，新增默认关闭的
`K1_SDIO_WIFI_CMD53_HISR_READ_RX_B9_DIAGNOSTIC` 和
`wireless_cmd53_hisr_read_rx_b9_diag`；它仅在该只读 HISR 请求启动前把 RX code 覆盖为
`0xb9`。2026-08-21 的 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-hisr-read-rx-b9-20260821T094329Z.log`，
ELF SHA-256 为
`476cd705c480b34019663fdcb322a63e70d0f61f733199a4b61e8d9224f05155`。日志确认实际
`DLINE_CTRL=0xa8b90001`，但后续同一 `0x14220804` 请求仍以
`INT_STATUS=0`、`PRESENT_STATE=0x01f70206`、`ADMA_ERROR=0` 和 `-ETIMEDOUT` 结束。
故 RX DLINE `0xb7` 对 `0xb9` 不是当前 HISR CMD53 读失败的根因。

同日还临时卸载并重新加载原厂 GPL `8852bs` 模块，观察到它在 CMD19 调谐阶段连续四次
`tuning execution failed: -5`，没有创建 `mmc1:*` SDIO device，也从未走到 CMD53。
这再次说明当前原厂 Linux 镜像没有可用的 CMD53 成功基线，不能把它当作 NuttX 后续
firmware、MAC 或网络层实现的前提。下一步需要取得可工作的 SDH1 数据路径的外部信号
证据或可复现的厂商启动配置；在此之前不得开始 Wi-Fi firmware、netdev 或联网工作。

### 2026-08-21：排除厂商 74-clock 序列

曾临时构建一个仅在 CMD0 前运行的 RAM-only 74-clock A/B，记录在
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-hisr-read-74clk-20260821T095621Z.log`，
临时 ELF SHA-256 为
`cd6b377caed997dd63c56c8056f708e8ffb43817c3901e24367b7ee77c319134`。控制器确实在
19 次 10-us 轮询内置位 MISC_INT（`MMC_CTRL=0x0f000806`），但随后的 CMD5 立即以
`INT_STATUS=0x18000` 失败，bring-up 返回 `-ETIMEDOUT`；没有走到 CMD53。

复核 `spacemit_sdhci_gen_init_74_clocks()` 的首个条件后，确认它在
`!(host->mmc->caps2 & MMC_CAP2_NO_SDIO)` 时直接返回，也就是仅面向不支持 SDIO 的 host。
SDH1 是 RTL8852BS2 的 SDIO host，不满足这个前提。该临时 Kconfig、profile 和驱动代码
已删除，不能作为 Wi-Fi 路径的后续修复方向；保留本记录是为了避免再次误迁移该序列。

### 2026-08-21：公开 RTL8852BS 与板级 DTS 交叉核对

重新核对公开 [armbian/wifi-rtl8852bs](https://github.com/armbian/wifi-rtl8852bs)
的 `os_dep/linux/sdio_intf.c`、`sdio_ops_linux.c` 和
`platform/platform_spacemit_sdio.c` 后，确认其 Linux 流程是先 enable Function 1、设置
512-byte function block size，再让小型递增寄存器访问走 byte-mode CMD53。它的 SpacemiT
平台层只负责 RF 上电和触发 SDIO rescan，不会在 CMD53 前写入额外的 K1 SDH1 私有寄存器。
历史成功 trace 中 HISR 请求仍是 `blocks=1`、`block_size=4`，因此 NuttX 已设置 Function 1
block size 为 512、但该只读 HISR probe 使用 4-byte byte mode 的组合是正确的。

同一分支的 MUSE Pi Pro DTS 的 `sdhci1` 是 4-bit、non-removable、`no-mmc`、`no-sd` 的
SDIO host，输入时钟为 375 MHz，并带 `SDHCI_QUIRK2_BROKEN_PHY_MODULE`。`pinctrl_mmc2`
把 GPIO15--20 分别映射为 DAT3..DAT0、CMD、CLK，全部为 MUX1、上拉、1.8 V DS2。现有
`k1_wireless_configure_sdio_pins()` 在电源时序之前逐项写入相同设置，且 SDH1 已走
BROKEN_PHY、375 MHz 和 TX/RX DLINE 路径。因此 pinmux、Function 1 block-size 与厂商
Realtek 包装方式都不是当前 CMD53 故障的遗漏项。

### 2026-08-21：自动 RAM handoff 回归（CMD53 仍失败）

`tools/run_k1_wireless_smoke.py` 现使用原厂 Linux 的 `adb reboot` 自动重启。K1 的
U-Boot 横幅出现时串口输入尚未准备好；工具因此只在 `In:    serial` 出现 150 ms 后发送
一个中断字符，而不会向已经启动的 Linux 或 NSH 连续写入。随后每条 watchdog 命令都等待
新的 `=>`，再只使用 `loadx` 和 `go` 完成 RAM handoff；不执行 `saveenv`、FDL、fastboot
或任何 eMMC 写入。

2026-08-21 的回归记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-hisr-read-post74-20260821T105752Z.log`。
54-byte wrapper 和 218320-byte payload 均通过 XMODEM 完整传输，U-Boot 分别报告
`## Total Size = 0x36` 与 `## Total Size = 0x354d0`，随后 `go 0x12000000` 到达
`NuttShell (NSH)`。对应 ELF 的 SHA-256 是
`c40dd191340eaa43e8118c8cc517ea74014bc073da2397b4af074bdfc244c23d`。

该次启动再次确认 CMD5/CMD52、CMD19 pattern（`mismatches=0`）、Function 1
`IOEN=IORDY=0x02` 和 GPL CMD52 bootstrap/HCI-DMAC/SDIO pre-init 都完成。F1 HISR 的
4-byte ADMA CMD53 请求仍在数据阶段超时：`INT_STATUS=0`、
`PRESENT_STATE=0x01f70206`、`ADMA_ERROR=0`，最终返回 `-ETIMEDOUT`
（日志 `error=0x6e`）。Bluetooth H5 local-version 同次仍返回
manufacturer=`0x005d`、subversion=`0x8852`、CRC enabled。因此本回归只证明自动
RAM handoff 与现有枚举/H5 诊断可重复；Wi-Fi firmware、MAC、netdev、联网，以及
Bluetooth firmware 下载、协议栈注册与扫描仍未完成。

### 2026-08-21：Function 1 前调谐 A/B（未解决 CMD53）

`wireless_cmd53_hisr_pre_f1_tuning_diag` 将 High-Speed、SDR104 和 CMD19 调谐移动到
Function 1 enable 与 512-byte block-size 写入之前，以匹配 Linux SDIO card init 到
function probe 的顺序。2026-08-21 的 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-hisr-pre-f1-20260821T121335Z.log`。
54-byte wrapper 与 218320-byte payload 均完整传输，随后到达 `nsh>`；ELF SHA-256 为
`4438129bea65d5be79136accf969d92249732b5f4954afadc6a6fb9098d5fef0`。

同次启动确认 CMD5/CMD52、Function 1 enable、CMD19、GPL CMD52 bootstrap 与 Bluetooth H5
local version 均通过，H5 仍报告 manufacturer=`0x005d`、subversion=`0x8852`。但 F1/HISR
`0x14220804` 的 4-byte ADMA CMD53 仍以 `INT_STATUS=0`、
`PRESENT_STATE=0x01f70206`、`ADMA_ERROR=0` 和 `-ETIMEDOUT`（`0x6e`）结束。因此 Function 1
前调谐顺序不是当前数据阶段未启动的根因，默认 `wireless` 配置维持原有顺序。

### CCCR_ABORT reset A/B（待实板）

Linux mainline `sdio_reset()` 会读取 CCCR `0x06`（ABORT），保留原值并置 RES bit 3，再进入
CMD0/CMD5。独立 profile `wireless_cmd53_hisr_cccr_abort_reset_diag` 仅在 ID clock 后、CMD0
前执行该 CMD52 写入，其他 HISR CMD53 配置与普通 profile 相同。它不下载 firmware、不注册
MAC 或 netdev，并且仍通过 `loadx + go` RAM-only 启动。

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless_cmd53_hisr_cccr_abort_reset_diag \
  --build-dir cmake_out/k1-wireless-cmd53-hisr-cccr-abort \
  --jobs 8

K1_ELF=cmake_out/k1-wireless-cmd53-hisr-cccr-abort/nuttx \
K1_CONFIG=cmake_out/k1-wireless-cmd53-hisr-cccr-abort/.config \
K1_PACKAGE_DIR=out/k1-wireless-cmd53-hisr-cccr-abort \
tools/package_k1_bringup.sh
```

验收必须记录 `CCCR_ABORT reset=` 或 `CCCR_ABORT reset error=`，然后再比较 Function 1、H5
与 HISR CMD53。Linux `mmc_sdio_pre_init()` 会忽略 `sdio_reset()` 的返回值再进入 CMD0/CMD5，
因此早期 CMD52 不可用只作为观测结果，不能阻止这组对照完成。只有 CMD53 数据阶段完成才继续
firmware/MAC 层；无论结果如何，不把该实验写入默认无线配置。

### 2026-08-21：CCCR_ABORT reset A/B（未打通 CMD53）

首轮把早期 CMD52 的 `-ETIMEDOUT` 当成致命错误，因而在 CMD0 前退出；这不符合
Linux `mmc_sdio_pre_init()` 调用 `sdio_reset()` 后忽略返回值、继续 CMD0/CMD5 的行为。
诊断随后修正为只记录 `CCCR_ABORT reset error=`，并完整执行原有枚举和 HISR 流程。

修正后的 ELF SHA-256 是
`da20d69cf7cfe04be95825a7eba51f930c45adb4f7753203a2fb9d246dd13656`，通过当前 NuttX
的 `reboot` 自动 RAM handoff 上板，串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-hisr-cccr-abort-20260821T135207Z.log`。
最早的 CCCR read/write 仍返回 `-ETIMEDOUT`（`CCCR_ABORT reset error=0x6e`），因此本轮
没有成功写入 RES bit；但后续 CMD5/CMD52、CMD19（零 mismatch）、Function 1
`IOEN=IORDY=0x02`、GPL bootstrap 和 Bluetooth H5 local-version 均通过。最终同一笔 F1
HISR ADMA CMD53 仍保持 `INT_STATUS=0`、`PRESENT_STATE=0x01f70206`、`ADMA_ERROR=0` 并以
`-ETIMEDOUT` 结束。

这表明该时点的 Linux-shaped CCCR reset 尝试没有改善 K1 的 CMD53 数据阶段；由于卡未接受
CMD52 写入，不能把它扩大解释为“成功 CCCR reset 已被排除”。默认 wireless 配置不改变，仍
不得开始 Wi-Fi firmware/MAC/netdev/联网或 Bluetooth firmware/扫描的迁移。

### 2026-08-22：成功 CCCR_ABORT.RES 的回归与默认回退

在前一轮 NuttX RAM image 后，未执行 CCCR reset 的历史 r31 image 立即在 CMD5 收到
`INT_STATUS=0x18000` 并退出。随后 default `wireless` image 实际完成了
`CCCR_ABORT reset=0x08`，并恢复了 CMD5/CMD52 枚举（R4=`0x90ffffff`、CCCR=`0x43`）；
但 SDR104 的整个 CMD19 扫描窗口变为零。为了排除扫描循环，
`wireless_static_tuning_diag` 只使用 DTS 的 RX delay 184 发起一次 CMD19，仍得到
R1=`0x1e00` 后的 `INT_STATUS=0x00408121` data error，未读到有效 pattern。该实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-static-tuning-20260821T160039Z.log`，
ELF SHA-256 为
`c265e577ac9675bd6cffedc128c2e9eaea4be81b1cd911b920aba018d2a20a43`；同次 Bluetooth H5
local-version 仍通过。

Linux `mmc_sdio_pre_init()` 将 `sdio_reset()` 描述为“硬件已经真正断电时并不需要”的兼容
恢复步骤。上述实板结果说明它不能作为本板默认 RAM-only 上电策略：成功写 RES 后还缺少
可验证的 Wi-Fi 卡恢复条件，不能以 CMD5 成功替代 CMD19 数据通路验收。默认
`wireless` 配置已恢复为不写 CCCR_ABORT；`K1_SDIO_WIFI_CCCR_ABORT_RESET` 和其 HISR
profile 保留为默认关闭的诊断工具。

随后让原厂 Linux 完整启动一次，再从 ADB 重启、抢 U-Boot 并加载不含 RES 的新
`wireless_static_tuning_diag` 包。该包的 ELF SHA-256 为
`7163f305720201ba6829964ec76dde9c80400bdc56ccf14ed8610245664eb5da`，实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-static-tuning-20260821T161149Z.log`。
它重新通过 CMD5、固定 delay 184 的 CMD19 `mismatches=0`、F1
`IOEN=IORDY=0x02`、GPL CMD52 bootstrap/HCI-DMAC/SDIO pre-init 和 Bluetooth H5。
因此直接从已写 RES 的 NuttX image 重启时出现的 `0x18000` 只是该临时卡状态的恢复问题，
不是正常的 Linux-to-U-Boot RAM handoff 所必需的默认操作。

同一次原厂 Linux 启动则连续四次报告 `mmc1: tuning execution failed: -5`，没有成功初始化
SDIO card。这说明当前板载原厂镜像没有可工作的 Wi-Fi 基线，不能据其“含厂商驱动”推断
Wi-Fi 应该已经可用。CMD53、firmware、MAC、netdev 和联网仍未实现；在有一条可复现的
CMD53 成功基线或新的电气采集之前，不应继续叠加 Realtek firmware 或网络层代码。

### 2026-08-22：CMD53 恢复修正的正常路径回归

`k1_sdio_cancel()` 现在在超时/取消边界同时复位 command 和 data 状态机；这是与 Linux
`REQUEST_ERROR` 路径对齐的恢复修正。它必须先证明不会破坏不触发 CMD53 诊断的正常
无线 bring-up，不能在没有实际 CMD53 成功的情况下称为传输层修复。

2026-08-22 通过 ADB 自动重启、串口自动抢 U-Boot，并只用 `loadx + go` RAM-only 启动了
`out/k1-wireless-cmd53-cleanup` 包。ELF SHA-256 为
`bd1d39b76f307d6ae8b6b3373cded7a68978fd7e57e908e17dc9088e307331dc`；完整串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-cleanup-20260821T164114Z.log`
（SHA-256 `2f2751da5355c9f7ae7a5d104305032dc93994e3f3586c3e8704589c8fff98d2`）。

该轮再次确认 `CMD5 OCR=0x90ffffff`、`CCCR=0x43`、CMD19
`mismatches=0`、Function 1 `IOEN=IORDY=0x02`、GPL bootstrap、HCI/DMAC pre-init、
SDIO pre-init，以及 Bluetooth H5 local-version（manufacturer=`0x005d`、
subversion=`0x8852`）均通过，并进入 `nsh>`。默认包不发 CMD53，因此本结果仅是恢复
修正的正常路径无回归证据；它没有验证 `k1_sdio_cancel()` 的超时后恢复，也没有改变
CMD53、Wi-Fi firmware/MAC/netdev/联网或 Bluetooth firmware/扫描尚未完成的结论。

### 2026-08-22：Function 1 HISR CMD53 首次实板通过

从原厂 Linux 完整启动状态经 ADB 重启、U-Boot `loadx + go` RAM handoff 后，普通
`wireless_cmd53_hisr_read_diag` profile 完成了首次 Function 1 ADMA CMD53 读取。完整
串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-cmd53-hisr-read-20260821T165357Z.log`，
日志 SHA-256 为
`27d6ae24b4f66d038519dfa6df9682fdb0babf6725fab1d5e002624258aa5851`；实际 ELF
`nuttx` SHA-256 为
`e5a531f59577a86375bef1b3e57a58f07bd2ec9240ea9cff4b04d961922a70cc`。

该轮在 CMD5/CMD52、CMD19（`mismatches=0`）、Function 1
`IOEN=IORDY=0x02`、GPL power bootstrap、HCI/DMAC pre-init 与 SDIO pre-init 之后，发出
`arg=0x14220804` 的 F1/HISR 4-byte 递增 ADMA read；控制器完成请求并记录
`F1 HISR CMD53=0x00000000`，没有 timeout/error。该 profile 没有启用 PIO、Linux
interrupt gate 或 RX-B9 A/B 选项，因此它证明当前正常 SDR104/ADMA 路径至少可执行一笔
Function 1 运行时读，不把原因归因于任一先前的单变量实验。随后 CMD52 HISR read 与
Bluetooth H5 local-version 也通过并进入 `nsh>`。

这推翻了此前“所有 CMD53 数据阶段均不可用”的结论，但只构成一笔 4-byte read 的传输层
验收，尚不表示 CMD53 write、DLE/HCI flow-control、firmware 下载、802.11 MAC、netdev
或联网可用。后续顺序改为：先重放原厂首笔 12-byte Function 1 CMD53 write，再以每一项
都有界错误处理的方式移植 DLE/HCI flow-control，最后才接入 firmware 下载和网络层。

### 2026-08-22：完整首笔 Realtek 间接 CMD53 读回通过

`_r_indir_cmd53_sdio_8852b()` 的首笔访问不是孤立的 12-byte write：它先向 Function 1
`0x1040` 写入 `R_AX_HCI_FUNC_EN` (`0x8380`) 的 indirect-read 请求，再从 `0x1043`
以递增 CMD53 读取 8 bytes，轮询 byte 0 的 ready bit，最后从 bytes 1--4 取得 little-endian
32-bit 值。此前诊断只完成第一步，随后 CMD52 访问 `0x74` 失败，不能说明 Realtek 序列本身
可用。

`k1_rtl8852bs_cmd53_indirect_read32()` 现以 50 次、20 us 间隔的有界轮询完整复现该序列。
从原厂 Bianbu Linux 完整启动状态通过 ADB reboot，再自动抢 U-Boot 并只执行 `loadx + go`
RAM handoff，`wireless_first_vendor_cmd53_diag` 的实板记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-first-vendor-cmd53-20260821T171338Z.log`。
ELF SHA-256 为
`f43362f3bf235cd1ce89bff6e7c165252d453245b2d1288cc599e10752df4075`，日志 SHA-256 为
`7298cfd1093faf89aca95e0fabc38861fc44a285ea9795bebd611b4632ea272f`，记录：

```
K1 Wi-Fi GPL: first vendor CMD53 error=0x0000000000000000
K1 Wi-Fi GPL: first vendor CMD53 value=0x0000000000000000
K1 Wi-Fi GPL: RTL8852BS2 HCI/DMAC pre-init complete
K1 Wi-Fi GPL: RTL8852BS2 SDIO pre-init complete
```

同一轮还再次通过 CMD5/CMD52、CMD19（zero mismatch）、Function 1
`IOEN=IORDY=0x02` 和 Bluetooth H5 local-version（manufacturer=`0x005d`、
subversion=`0x8852`），随后进入 `nsh>`。这证明标准 SDR104/ADMA 可以完成原厂首笔
12-byte write 加 8-byte read 的间接寄存器事务，也证明此前后续 pre-init 失败由未读回
indirect request 的完成状态造成。

当前边界仍清晰：这不是 firmware 下载或 802.11 MAC/netdev；下一步是逐项移植原厂
DLE/HCI flow-control 初始化，并为每步保留可验证的 CMD53 事务和失败边界。测试工具的
`--require-first-vendor-cmd53` 现同时要求 success marker 和 returned value，
`--require-wifi-function` 也承认这笔完整的 Function 1 CMD53 read。

### 2026-08-22：RTL8852BS2 SDIO/SCC DLE 实板通过

`wireless_dle_scc_diag` 从 `wireless_first_vendor_cmd53_diag` 继承，并额外启用
`CONFIG_K1_RTL8852BS2_DLE_SCC_DIAGNOSTIC`。GPL 隔离组件按 Realtek 的
`dle_mem_sdio_8852b` 配置 WDE 126 页、PLE 688 页及 15 组 WDE/PLE quota；所有非本地
MAC 寄存器访问都使用完整的 Function 1 间接 CMD53 write/read，WDE/PLE ready bit 采用有界
轮询。任一步失败都会尽力清除 DLE enable，且整个 profile 只经 `loadx + go` 运行在 RAM。

从完整启动的原厂 Bianbu Linux 经 ADB reboot 自动抢 U-Boot 后，实板记录位于
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-dle-scc-20260821T172423Z.log`。
ELF SHA-256 为
`5bf4033556c79af6d88e829acf674aafa5f2c792ac26144b4cd7f8ce2a51963f`，日志 SHA-256 为
`5b2b639bc8789077609a74cf269d0ba2d8d4bfb8909ce545d0dadb40edab8006`。本次验收命令为：

```bash
python3 tools/run_k1_wireless_smoke.py \
  --device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --wrapper /home/sw/Dev/k1-workspace/out/k1-wireless-dle-scc/k1-go-wrapper.bin \
  --payload /home/sw/Dev/k1-workspace/out/k1-wireless-dle-scc/contest-nuttx-flat.bin \
  --require-h5 --require-wifi-function --require-first-vendor-cmd53 \
  --require-dle-scc --require-sdio-pre-init --require-bringup-success \
  --boot-timeout 90
```

日志依次记录首笔 vendor CMD53 write/read 成功、`RTL8852BS2 DLE SCC init complete`、
`SDIO pre-init complete` 与 Bluetooth H5 local-version，随后到达 `nsh>`。冒烟工具现在的
`--require-dle-scc`、`--require-sdio-pre-init` 和 `--require-bringup-success` 会分别检查
DLE complete marker、SDIO pre-init complete marker 以及没有 bring-up error。

随后的同一 payload 回归记录
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-dle-scc-20260821T173110Z.log`
仍通过 DLE complete，却在 SDIO pre-init 读取 `0x0074` 时返回 CMD53 CRC error `-84`
（`error=0x54`），并输出 `K1 RTL8852BS2 bring-up failed`。该轮的动态 CMD19 sweep 选择
RX delay `0xb7`，而上述完整成功轮为 `0xb6`；这只是待验证的时序相关性，不足以判定根因。
为隔离变量，新增 `wireless_dle_scc_static_tuning_diag`，它固定使用 MUSE Pi Pro DTS 的
RX delay `0xb8`；在该 profile 连续通过 DLE、SDIO pre-init 和无 bring-up failure 前，不进入
HCI flow-control。

这完成的是 DLE/SCC 硬件初始化验收，不是完整 Wi-Fi：HCI flow-control、Realtek firmware
下载、802.11 MAC、NuttX `netdev`、关联和联网仍未实现；Bluetooth firmware 下载、协议栈
注册和扫描同样尚未实现。

### 2026-08-22：固定 RX delay `0xb8` 的严格 DLE/SCC 回归

`wireless_dle_scc_static_tuning_diag` 固定采用 MUSE Pi Pro DTS 的 RX delay `0xb8`，不再
执行会在启动间改变采样点的动态 CMD19 sweep。以同一 ELF 连续进行两次从完整启动的原厂
Bianbu Linux 经 ADB reboot、自动抢 U-Boot、`loadx + go` RAM-only handoff 的严格验收，均
通过首笔 vendor CMD53、DLE/SCC 和 SDIO pre-init，且没有 `K1 RTL8852BS2 bring-up failed`。

实际 ELF 为
`/home/sw/Dev/k1-workspace/out/k1-wireless-dle-scc-static-tuning/nuttx`，SHA-256 为
`ed075946ad3ff42c94ca656d15fb4c87363733c1878f2d9f0c526b7086a6e225`。两轮完整串口记录为：

* `/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-dle-scc-static-tuning-20260821T173849Z.log`
  （SHA-256 `4d0c82b1931f28a965f1cbf4eb4b26902f5820a30b35807ef3605c5976339b27`）
* `/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-dle-scc-static-tuning-20260821T174018Z.log`
  （SHA-256 `71ccbe4234e6c17253614f478a16d86da76636baf69811baa14a1ef069913d46`）

两次均使用以下严格条件：

```bash
python3 tools/run_k1_wireless_smoke.py \
  --device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --wrapper /home/sw/Dev/k1-workspace/out/k1-wireless-dle-scc-static-tuning/k1-go-wrapper.bin \
  --payload /home/sw/Dev/k1-workspace/out/k1-wireless-dle-scc-static-tuning/contest-nuttx-flat.bin \
  --require-h5 --require-wifi-function --require-first-vendor-cmd53 \
  --require-dle-scc --require-sdio-pre-init --require-bringup-success \
  --boot-timeout 90
```

因此，固定 `0xb8` 是当前可重复的 SDIO transport/DLE 基线；动态调谐保留为独立的诊断问题。
这仍不等同于可用 Wi-Fi：下一项是按 GPL 参考逐项实现并验收 HCI flow-control，随后才是
firmware 下载、802.11 MAC、`netdev`、关联和联网。蓝牙侧的 firmware 下载、协议栈注册和
扫描也仍未实现。

### 2026-08-22：HCI flow-control 诊断实板验收通过

基于同一 revision 的 Realtek `hfc_init(adapter, 1, 1, 1)`、
`hfc_chcfg_sdio_8852b` 和 `hfc_pubcfg_sdio_8852b`，GPL 隔离组件现可在 DLE/SCC 后、
SDIO pre-init 前执行 8852B SDIO HCI flow-control 初始化。它配置六个 active channel 的
`min=2,max=102` 页、112 页公共池、CH0--11 pre-cost=1、H2C pre-cost=40 与 SDIO mode，
再打开 HCI/H2C FC 位。每个页控寄存器写入都会经过 Function 1 indirect CMD53 读回校验；
任一传输或读回失败会关闭 HCI/H2C FC enable 位并使 board bring-up 失败。

实现仅由 `CONFIG_K1_RTL8852BS2_HCI_FC_DIAGNOSTIC` 启用，后者依赖 DLE/SCC，测试 profile
为 `board/k1/muse_pi_pro/configs/wireless_hci_fc_static_tuning_diag`。它继承固定 RX delay
`0xb8` 的双轮通过基线，默认 `wireless` profile 不变。2026-08-22 已单独构建、ELF 验证和
RAM-only 打包；该 ELF SHA-256 为
`c0086a09559c32bcf7c254467ed86727ddfa25b50c7ef000688f57290574bf9e`，包位于
`/home/sw/Dev/k1-workspace/out/k1-wireless-hci-fc-static-tuning`。

2026-08-22 已在原厂 Linux 已完整启动、ADB 和 `wlan0` 均可见的前提下，完成以下严格实板
验收。工具经 ADB reboot 自动抢 U-Boot，并只使用 `loadx + go`，不会保存环境或写 eMMC/FDL：

```bash
python3 tools/run_k1_wireless_smoke.py \
  --device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --wrapper /home/sw/Dev/k1-workspace/out/k1-wireless-hci-fc-static-tuning/k1-go-wrapper.bin \
  --payload /home/sw/Dev/k1-workspace/out/k1-wireless-hci-fc-static-tuning/contest-nuttx-flat.bin \
  --require-h5 --require-wifi-function --require-first-vendor-cmd53 \
  --require-dle-scc --require-hci-flow-control --require-sdio-pre-init \
  --require-bringup-success --boot-timeout 90
```

该轮对应 ELF SHA-256 为
`c0086a09559c32bcf7c254467ed86727ddfa25b50c7ef000688f57290574bf9e`，串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-hci-fc-static-tuning-20260821T180302Z.log`
（SHA-256 `5afa4391f33d256676b32fd56ab0d55bd0c4ce07ee3a0d296cc14adc47b67717`）。日志依次确认：

* Function 1 已启用，SDR104 与固定 RX delay `0xb8` 生效；
* 首笔 vendor-shaped CMD53 write/read、DLE/SCC 初始化均成功；
* `RTL8852BS2 HCI flow-control init complete`；
* `RTL8852BS2 SDIO pre-init complete`；
* Bluetooth H5 local-version 往返成功，最终进入 `nsh>`。

因此 HCI flow-control 已取得实板初始化证据，但这不是 firmware 下载、Wi-Fi MAC、`netdev`、
关联或联网完成的证据。下一阶段仍是 Realtek firmware 下载；蓝牙 firmware 下载、协议栈注册
和扫描也仍不受本项覆盖。

### 2026-08-22：FWDL H2C preboot 诊断实板验收通过

`wireless_fw_preboot_static_tuning_diag` 在已验收的固定 RX delay `0xb8`、DLE/SCC、HCI
flow-control 和 SDIO pre-init 基线上，增加一个一次性的 WCPU 固件下载前置诊断。它从同一
GPL 参考的 `mac_enable_cpu(..., dlfw=1)` 与 `fwdl_phase0()` 改编：清除 LDM、SER halt
寄存器和 HISR0 锁存状态，设置 CPU clock、FWDL enable、boot reason 与 WCPU enable，随后
在最多 400000 次、每次 1 us 的轮询内等待 `R_AX_WCPU_FW_CTRL[1]`（H2C path ready）。

该诊断不含 firmware 数据，也不走 SDIO TX FIFO。无论 H2C-ready 成功、超时还是前置访问
出错，都会依照 `mac_disable_cpu()` 的顺序清除 WCPU enable、FWDL/H2C/FWDL-ready 位和 CPU
clock；成功路径还会逐项读回确认这些位均为零。它刻意不清除该参考函数未清除的 FWDL
status 字段。参考驱动还会执行
`fwdl_precheck()` 的 DLE debug-port 空队列查询，这一只读查询尚未移植，因此本项是明确的
受限前置验证，不能等价为完整厂商 firmware 下载流程。

构建和 RAM-only 验收命令如下；成功标记仅表示 WCPU 可进入并退出 H2C-ready 状态：

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless_fw_preboot_static_tuning_diag \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-preboot-static-tuning \
  --jobs 8 --package \
  --package-dir /home/sw/Dev/k1-workspace/out/k1-wireless-fw-preboot-static-tuning

python3 tools/run_k1_wireless_smoke.py \
  --device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --wrapper /home/sw/Dev/k1-workspace/out/k1-wireless-fw-preboot-static-tuning/k1-go-wrapper.bin \
  --payload /home/sw/Dev/k1-workspace/out/k1-wireless-fw-preboot-static-tuning/contest-nuttx-flat.bin \
  --require-h5 --require-wifi-function --require-first-vendor-cmd53 \
  --require-dle-scc --require-hci-flow-control --require-sdio-pre-init \
  --require-firmware-preboot --require-bringup-success --boot-timeout 90
```

在取得实板日志和对应 ELF SHA-256 前，本项目仍没有 firmware header/section 的 H2C TX
descriptor、SDIO TX FIFO 写入、firmware image、Wi-Fi MAC、`netdev`、关联或联网的验收。

2026-08-22 从原厂 Bianbu Linux 完整启动、`8852bs` 模块和 `wlan0` 均已出现的基线经 ADB
reboot 自动抢 U-Boot，完成上述 RAM-only 验收。最终 ELF SHA-256 为
`7d650a869f04725d21a7de7f0b3098913d2cf35047690e660f2487e5bb187544`，完整串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-preboot-static-tuning-20260821T183620Z.log`
（SHA-256 `1c99dbef80d91bc5d1b7e185a89b32d69ec569ce869a88600a720162a250e55c`）。

日志确认 `RTL8852BS2 HCI flow-control init complete`、`SDIO pre-init complete`、
`FW preboot begin` 和 `RTL8852BS2 FW preboot H2C path complete` 依次出现，随后 H5
local-version 成功并进入 `nsh>`。success marker 只在源代码确认 `R_AX_PLATFORM_ENABLE[1]`、
`R_AX_WCPU_FW_CTRL[2:0]` 和 `R_AX_SYS_CLK_CTRL[14]` 已清零后才输出；日志没有
`FW preboot error`、`FW preboot cleanup error` 或 `K1 RTL8852BS2 bring-up failed`。

同一 RAM-only NuttX image 直接以 NSH `reboot` 再次启动时，卡会在最早 CMD5 返回
`INT_STATUS=0x18000`，尚未进入 GPL bootstrap 或 FWDL 代码。这是已知的连续 NuttX
SDIO 卡暂态；让 U-Boot 正常启动一次原厂 Linux，确认 `8852bs`/`wlan0` 恢复后再从 ADB
重启，即可回到上述成功基线。该恢复过程不写 U-Boot 环境或任何持久存储。

因此当前通过的边界是：可重复地进入 WCPU 的 firmware-download H2C-ready 状态并清理返回，
不是 firmware image 传输或可用 Wi-Fi。下一步应先实现和验收 H2C TX FIFO 的资源检查与
firmware-header packet，不应宣称 MAC、`netdev`、关联或联网已经完成。

### 2026-08-22：H2C TX 资源与描述符诊断已完成主机构建

`wireless_fw_h2c_tx_static_tuning_diag` 继承已验收的固定 RX delay、DLE/SCC、HCI
flow-control、SDIO pre-init 和 FW preboot profile。它先执行原有的 WCPU H2C-ready
preboot 并完成原有 cleanup，再运行一项不写 FIFO 的传输前诊断：

1. 参考 `ud_fs_8852b()`，用一条 Function 1、递增、28-byte CMD53 **读**读取
   `R_AX_SDIO_TXPG_WP`（`0x1110`），从第一个 little-endian word 的 `[28:16]` 取得
   H2C channel 12 可用页单位。
2. 参考 `__fwhdr_download()`、`h2c_pkt_build_txd()` 和
   `txdes_proc_h2c_fwdl_8852b()`，仅在 RAM 中构造一个空 payload 的 FWDL H2C
   envelope：8-byte H2C header（MAC/FWDL/FWHDR_DL，total length 8）和 24-byte TX
   descriptor（DMA channel 12、TX packet length 8）。这不是可下发的 firmware header。
3. 参考 `tx_allow_fwcmd_ch()` 和 `tx_cmd_addr_sdio()`，以该最小 envelope 计算 2 个
   H2C 页单位、32-byte 总 SDIO 传输和 FIFO CMD53 address `0x1c004`。代码只打印该值，
   不向该地址或任何 TX FIFO 调用 `k1_sdio_wifi_write()`。

该 profile 的主机构建、ELF 检查和 RAM-only 打包已经通过。ELF 为
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-h2c-tx-static-tuning/nuttx`，SHA-256
为 `4680b3f35c6941dc0f5e07e4963ab6de012d2bbab928ef2038c4cee985f8e236`；包目录为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-h2c-tx-static-tuning`。flat image SHA-256
为 `b68368a90c5a7e61087ccfc515152ca895514b627d775a714cf140700dbea760`。

实板恢复到原厂 Linux、确认 `adb get-state` 返回 `device` 后，可以用下面命令做 RAM-only
验收；它不执行 `saveenv`、FDL、fastboot 或任何 eMMC/SPI 写入：

```bash
python3 tools/run_k1_wireless_smoke.py \
  --device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --wrapper /home/sw/Dev/k1-workspace/out/k1-wireless-fw-h2c-tx-static-tuning/k1-go-wrapper.bin \
  --payload /home/sw/Dev/k1-workspace/out/k1-wireless-fw-h2c-tx-static-tuning/contest-nuttx-flat.bin \
  --require-h5 --require-wifi-function --require-first-vendor-cmd53 \
  --require-dle-scc --require-hci-flow-control --require-sdio-pre-init \
  --require-firmware-preboot --require-h2c-tx-resource \
  --require-bringup-success --boot-timeout 90
```

实板成功的最低日志条件是先出现原有 `FW preboot H2C path complete`，再出现
`H2C TX resource available=... required=2 FIFO=1c004 TXD0=c0000 TXD2=8` 与
`H2C TX resource diagnostic complete`。任意 `H2C TX resource diagnostic error`、FW
preboot error 或 board bring-up failure 都必须保留日志并停止在本阶段；不能据此发送实际
firmware header。

2026-08-22 已按上述命令完成实板验收。开始前从完整启动的原厂 Bianbu Linux 经 ADB
确认 `8852bs` 模块已加载，`wlan0` 存在且 MAC 为 `84:fc:14:06:79:7b`；随后脚本自动
执行 `adb reboot`、抢 U-Boot，并只执行 `loadx + go`。完整串口记录是
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-h2c-tx-static-tuning-20260821T190503Z.log`
（232864 bytes，SHA-256
`7c325c9f1a6d58db96872890fa7cd14b158d994d244dd85b4bb86d79f60617cc`）。

实测结果为 H2C channel 12 `available=0x20`、`required=0x02`，并打印预期的
`FIFO=0x1c004`、`TXD0=0x000c0000`、`TXD2=0x8`，紧接着出现
`RTL8852BS2 H2C TX resource diagnostic complete`。同一记录还确认 FW preboot cleanup
完成、Bluetooth H5 local-version 成功（subversion `0x8852`）并进入 `nsh>`，没有
`K1 RTL8852BS2 bring-up failed` 或 H2C/FW preboot error。U-Boot 在启动时对无卡的
备用 SD 通道打印的 `CMD8` 错误不属于 NuttX Wi-Fi 诊断，不能与该阶段失败混淆。

这证明首个 32-byte FWDL H2C envelope 有足够的卡端 TX 资源，仍然**不**证明任何实际
FIFO 写入、firmware header/section 传输、firmware 下载、Wi-Fi MAC、`netdev`、关联或联网。
后续若实现第一笔真实 firmware header，必须只发送一个有界的 channel-12 packet，逐项记录
TX FIFO CMD53 返回值和 cleanup 状态，并取得新的实板日志后才能扩大传输范围。

### 2026-08-22：单包 firmware static-header 诊断实板验收通过

`wireless_fw_header_packet_static_tuning_diag` 继承已验收的 H2C resource profile，并只增加
`CONFIG_K1_RTL8852BS2_FW_HEADER_PACKET_DIAGNOSTIC`。它首先重新进入 WCPU H2C-ready 状态，
重新读取 channel 12 页资源，然后以一笔 Function 1、**固定地址** CMD53 将下列 112 bytes
送至 `0x1c00e`：24-byte H2C TX descriptor、8-byte FWDL H2C header（`0x0000000d`、length
`0x58`）及 GPL Realtek `array_8852b_u2_nic` 的 80-byte static header。Linux vendor path
对此 FIFO 使用 `sdio_writesb()`，所以此处明确使用不递增地址的 CMD53。

这不是完整 firmware 下载：不会发送 dynamic header、任何 section 或其 checksum，且成功或
失败都会执行 `mac_disable_cpu()` 对应的 WCPU/FWDL/clock cleanup。成功标记只有在
`R_AX_WCPU_FW_CTRL[2]` 已出现且 cleanup 成功后才会输出。预期最小日志为：

```text
K1 Wi-Fi GPL: FW header packet available=20 required=2 FIFO=1c00e bytes=70
K1 Wi-Fi GPL: RTL8852BS2 FW header packet complete
```

2026-08-22 已从完整启动的原厂 Linux 经 ADB reboot 自动抢 U-Boot，并只以 `loadx + go`
载入该 image。实际 ELF 为
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-header-packet-static-tuning/nuttx`
（SHA-256 `2901f18fc92fdc1ace6bfc791e983f5769ef179234a618a05862915a6e73656b`），flat image
SHA-256 为 `857852814fde446fd9401591640f29895b34fd2f53b5a7136e6159dfd4edb896`，完整串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-header-packet-static-tuning-20260821T192656Z.log`
（SHA-256 `915230502bbe7fc9dd3f1234e6454d7db159bcc2530bacd0503b74e83b444c32`）。

日志记录 H2C channel 12 `available=0x20`、`required=0x02`，随后出现
`FW header packet available=... FIFO=0x1c00e bytes=0x70` 与
`RTL8852BS2 FW header packet complete`。同一轮继续通过 Bluetooth H5 local-version
（subversion `0x8852`）并到达 `nsh>`；没有 `FW header packet diagnostic error`、
`FW header packet cleanup error` 或 `K1 RTL8852BS2 bring-up failed`。因此，固定地址的
112-byte static-header H2C packet 已获得实板传输和 WCPU `FWDL_PATH_READY` 的验收证据。

这仍不是 firmware 下载成功或 Wi-Fi 可用：任何 firmware section/checksum、firmware-ready
状态、MAC、`netdev`、关联和联网均未实现。上游 `fwdl_phase1()` 只发送
`hdr_len - dynamic_hdr_len` 的静态 80 bytes；dynamic header 位于 firmware blob 的
`0x50..0x9f`，仅供主机解析 section 信息，**不经 TX FIFO 发送**。因此下一阶段是一个新的、
有界的 section-zero 首包诊断，而不是发送 dynamic header；不得直接扩大为完整 image 传输。

### 2026-08-22：section-zero 首包诊断已完成主机构建前实现

`wireless_fw_section0_packet_static_tuning_diag` 继承已验收的固定时序、DLE/SCC、HCI
flow-control、SDIO pre-init 和 FWDL 前置阶段，但在板级顺序中跳过独立的 header-only
diagnostic，改为在**同一个** WCPU FWDL 会话内进行以下严格有界的两笔固定地址 CMD53 FIFO
写入：

1. 使用原有的 24-byte descriptor、8-byte FWDL H2C header 和 80-byte static firmware
   header（总计 112 bytes），等待 `R_AX_WCPU_FW_CTRL[2]`，随后按 `fwdl_phase1()` 清除
   `R_AX_HALT_H2C_CTRL` 与 `R_AX_HALT_C2H_CTRL`。
2. 从 GPL `array_8852b_u2_nic` 的 offset `0x00a0` 复制 section zero 的**仅第一个**
   2020-byte payload；它前置 24-byte `RTW_PHL_PKT_TYPE_FWDL` descriptor，设置 DMA
   channel 12 和 `AX_TXD_FWDL_EN`，故 `TXD0=0x001c0000`、`TXD2=0x000007e4`。descriptor
   加 payload 为 2044 bytes；上游 SDIO path 会按 8 bytes 补齐，所以实际 CMD53 传输为
   2048 bytes（末尾 4 bytes 为零）、FIFO `0x0001c100`。

该 packet 没有 8-byte H2C header：这是上游 `__sections_download()`/
`__sections_build_txd()` 的格式，而不是 `__fwhdr_download()` 的 header 格式。上游
`chk_rqd_pg_num()` 对 H2C 的页数只采用 descriptor 中的 `pkt_size + wp_offset`，所以此包
的 `ceil(2020 / 128) * 2` 为 32 页；实现按 `tx_allow_fwcmd_ch()` 的五次刷新上限重新读取
channel 12 的页状态，以避免 static header 暂占页面时作出过早结论。

无论 section CMD53 成功或失败，代码都会执行既有 `mac_disable_cpu()` 对应清理；它不会发送
第二个 section packet、任何 checksum、完整 image，亦不会注册 MAC、`netdev` 或联网。成功
标记仅表示首个 section packet 的 CMD53 已返回成功且 cleanup 成功，**不表示 firmware 已下载
或 Wi-Fi 可用**。首包 payload 的来源为同一 GPL `hal8852b_fw.c`，完整提取的 U2 NIC image
SHA-256 是 `c9a3f38bcbcc0e1341404dd0e2009adfedf5489950636ee7c969e8d1d8607ce5`，其
offset `0x00a0`、长度 2020 bytes 的切片 SHA-256 是
`3cd7931ac7918b8585f58c8d58a00b95b03727967744c460348a416fe453cebf`。

实板验收前仍需先按下列命令完成构建、ELF 检查和 RAM-only 打包；不得使用 `saveenv`、FDL、
fastboot 或任何 eMMC/SPI 写入：

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless_fw_section0_packet_static_tuning_diag \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-section0-packet-static-tuning \
  --jobs 8 --package \
  --package-dir /home/sw/Dev/k1-workspace/out/k1-wireless-fw-section0-packet-static-tuning

python3 tools/run_k1_wireless_smoke.py \
  --device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --wrapper /home/sw/Dev/k1-workspace/out/k1-wireless-fw-section0-packet-static-tuning/k1-go-wrapper.bin \
  --payload /home/sw/Dev/k1-workspace/out/k1-wireless-fw-section0-packet-static-tuning/contest-nuttx-flat.bin \
  --require-h5 --require-wifi-function --require-first-vendor-cmd53 \
  --require-dle-scc --require-hci-flow-control --require-sdio-pre-init \
  --require-firmware-preboot --require-h2c-tx-resource \
  --require-firmware-section-packet --require-bringup-success --boot-timeout 90
```

预期的新增日志是 `FW section0 packet available=... required=20 FIFO=1c100
bytes=800 TXD0=1c0000 TXD2=7e4`，随后是 `RTL8852BS2 FW section0 packet complete`。

### 2026-08-22：section-zero 首包诊断实板验收通过

首轮实板运行证明原有 CMD53 API 的 byte-mode 上限为 512 bytes：2048-byte section
packet 在发命令前返回本地 `-EINVAL`，没有到达卡。为保持一个卡端 packet，不能把它拆成
四笔 FIFO 写；`chip/k1/k1_sdio.c` 因而仅在本 profile 允许一个 2048-byte、固定为四个
512-byte blocks 的 CMD53 block-mode 请求，并将 ADMA bounce buffer 同步限制为 2048 bytes。
其他 profile 保持 512-byte byte-mode 上限。

从完整启动的原厂 Bianbu Linux 基线确认 `8852bs` 和 `wlan0` 后，脚本经 ADB reboot、自动
抢 U-Boot，并只用 `loadx + go` 做 RAM-only 启动。最终 ELF SHA-256 为
`a48738e080736bf9d673d12b21f0743083050ab8b823a98cecd45e82a3bae190`；flat image SHA-256
为 `13d8be65ad9b7c63fe54817d457f2d3a50edf2e3c3957467b46c09a50b3ec9ed`。完整串口记录是
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-section0-packet-static-tuning-20260821T201735Z.log`
（265 KiB，SHA-256 `0340e4d89c3554a9c747c71dbdaf08b655ea2d5b4d57495a294a5870678038d8`）。

记录确认 `CMD53 block count=4`、CMD53 argument `0x9b820004`、SDHCI transfer mode
`0x23` 与 block size `0x7200`，之后出现 `RTL8852BS2 FW section0 packet complete`。同一轮
Bluetooth H5 local-version 成功并到达 `nsh>`，没有 section packet error、cleanup error 或
`K1 RTL8852BS2 bring-up failed`。这证明首个 section packet 已被卡端成功接受并完成 cleanup；
它仍不表示完整 firmware image、Wi-Fi MAC、`netdev`、关联或联网已实现。下一阶段只能在新的
有界诊断 profile 中验证下一个 packet，不能直接扩大为完整 firmware 下载。

### 2026-08-22：28-byte post-first-packet 兼容诊断实板验收通过

这项 profile 的历史名称 `wireless_fw_section0_tail_packet_static_tuning_diag` 和
`CONFIG_K1_RTL8852BS2_FW_SECTION0_TAIL_PACKET_DIAGNOSTIC` 保留以兼容已有构建记录，但它**不是**
section 尾包。事后按 GPL `fwhdr_hdr_t`（32 bytes）和三个 16-byte `fwhdr_section_t` 重新解析
`array_8852b_u2_nic`，确认静态 header 结束于 `0x50`、host-only dynamic header 延伸至 `0xa0`：

* section zero header 在 `0x20`，`dword0=0xb8970000`、`dword1=0x4203f420`，实际长度为
  `0x3f420` bytes；
* section one header 在 `0x30`，长度 `0x3780` bytes；
* section two header 在 `0x40`，type 9，8852B 特例长度 `0x800` bytes。

所以 offset `0x00a0` 的首个 2020-byte packet 之后，offset `0x0884` 起的 28 bytes
（SHA-256 `38d1948469cb0724e452e5107043875d3295b5f16742c584b5b115099d57571d`）只是原厂
**第二个** 2020-byte packet 的开头。该兼容 profile 在同一个 WCPU FWDL 会话内重放 static
header 和首包，然后发送该 28-byte transport probe。24-byte FWDL descriptor 加 payload 为
52 bytes，按 8-byte FIFO 单位补齐为 56 bytes，固定 FIFO 地址为 `0x0001c007`、页需求为 2、
`TXD0=0x001c0000`、`TXD2=0x0000001c`。它不完成 section zero、不发送 checksum、不轮询
firmware-ready，也不创建 MAC 或 `netdev`。

构建、ELF 验收、静态 CI 和 RAM-only 上板均已通过。最终 ELF SHA-256 为
`f03a4a890b140ea1dbe470e2609bf6a91131d36878be899f4b09048c20a5a797`，flat image SHA-256 为
`d350aaad6f9800fd7ec24ebc879fbf431a96a4c1dd1b24f02c2bd3b1e6a309ce`，完整串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-section0-tail-packet-static-tuning-20260822T023913Z.log`
（SHA-256 `858c731e10ef649f8c8495269213df100032dd8223eac4cde9780884aa4101f6`）。测试先确认
原厂 Linux 的 `8852bs` 和 `wlan0`，再通过 ADB reboot、自动截停 U-Boot 和 `loadx + go`
临时加载；未执行 `saveenv`、FDL、fastboot 或任何 eMMC/SPI 写入。当前 K1 的 NuttX
`reboot` 路径会落到 `Power down`，不能作为自动回到 Linux 的机制；RAM-only 测试结束后应由
物理 `RST` 恢复原厂系统。

串口依次记录首包的 `CMD53 block count=4`，以及尾包
`FIFO=0x1c007 bytes=0x38 TXD0=0x1c0000 TXD2=0x1c`，随后出现
`RTL8852BS2 FW section0 tail packet complete`。同一轮 Bluetooth H5 local-version 成功并到达
`nsh>`，没有 cleanup error 或 `K1 RTL8852BS2 bring-up failed`。这只证明卡端接受了首个
2020-byte packet 后的一个 56-byte FIFO 传输并完成清理；**不能**证明 section zero 的前
2048 bytes 是一个原厂 packet 边界，更不能代表完整 firmware download、Wi-Fi MAC、`netdev`、
扫描、关联或联网。

### 2026-08-22：section-zero 第二 packet 诊断实板验收通过

`wireless_fw_section0_second_packet_static_tuning_diag` 在同一个 WCPU FWDL 会话中发送 static
header、section-zero 的第一个 2020-byte packet，以及原厂紧随其后的第二个 2020-byte packet。
第二 packet 精确来自 GPL image offset `0x0884..0x1067`，SHA-256 为
`0b1a1a77589a60e4dcb3a95be536dc2180a5b68eba18128a1061013fbdad34ad`。它同样使用
24-byte FWDL descriptor、4 x 512-byte block-mode CMD53、`FIFO=0x1c100`、`TXD0=0x001c0000`、
`TXD2=0x000007e4` 和 32 页资源请求。两笔之后立即 cleanup，不发送第三 packet、checksum 或
firmware-ready 查询，也不注册 MAC/netdev。

该 profile 与旧 28-byte compatibility probe 在 Kconfig 中互斥。2026-08-22 已通过主机构建、
ELF 检查、静态 CI、以及跳过独立仓库文件头路径归属检查后的单文件 nxstyle 与 RAM-only package
生成。payload 源码按 16 bytes/line 重排后仍为上述 SHA-256；当前 ELF SHA-256 为
`12d689c39edc2348a825bc9c0dbee4139df2e2d845541aa88f44282e51d8a836`，flat image SHA-256 为
`af5f8a1d9b3f0d442c989e02810f7f59fde32a9174994edba16b5eebad741304`，产物目录为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-section0-second-packet-static-tuning`。最初一次预先
启动的 120-second `--manual-reset` listener 未收到串口字节，未进入 U-Boot、未发生 XMODEM 传输；
该次空记录不构成失败。随后从原厂 Linux（`wlan0` 和 `8852bs` 已加载）执行 ADB reboot，脚本自动
中断 U-Boot、仅以 `loadx` 和 `go` 加载 RAM image。串口在同一轮记录第二包的
`available=0x20 required=0x20 FIFO=0x1c100 bytes=0x800 TXD0=0x1c0000 TXD2=0x7e4`、4 个
512-byte CMD53 block，随后出现 `RTL8852BS2 FW section0 second packet complete`、Bluetooth H5
local-version（subversion `0x8852`）和 `nsh>`，没有 cleanup error 或
`K1 RTL8852BS2 bring-up failed`。完整原始记录是
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-section0-second-packet-static-tuning-20260822T045429Z.log`，
SHA-256 为 `27f9c69b86088358c8ea722fac455d63cb41187bde6f8dc87a36a95882d40a2f`。

这证明卡端已依序接受 section zero 的前两个原厂 2020-byte packets，并在 cleanup 后仍可运行
Bluetooth H5。它仍不等价于完整 firmware download、Wi-Fi MAC、`netdev`、扫描、关联或联网；第三包及
之后的 checksum/ready 阶段必须继续逐步验证。

### 2026-08-22：section-zero 第三 packet 诊断已实现，待实板验证

`wireless_fw_section0_third_packet_static_tuning_diag` 只在已验收的前两包序列后追加一个原厂
2020-byte packet。它从同一 SpacemiT Linux `k1-bl-v2.2.y` revision
`31c449aeaad8c7759bc983ca0e26946e5b6746dc` 的 `hal8852b_fw.c` 提取 U2 NIC image offset
`0x1068..0x184b`，SHA-256 为
`f5930f842fafb7680a7d652b828f34f6b1621cbff5581cdcf050e765e6063463`。参考文件 SHA-256 为
`0976181dbd9e1f53576b51021e4765e84019dd5c448a47eeece94ba22d548e3f`；其
`FWDL_SECTION_PER_PKT_LEN` 明确为 2020。参考映像在 `0x0a0` 和 `0x884` 的前两包已分别与本
项目中实板通过的 payload 逐字节一致，因此第三包偏移不是猜测。

该 profile 在一个 WCPU FWDL 会话中发送 static header、第一包、第二包和第三包。三个 section
packets 都使用原厂 24-byte descriptor、`FIFO=0x1c100`、32 页资源请求和固定地址的 4 x 512-byte
CMD53 写入；第三包之后立即执行原有 cleanup。它不发送第四包、section checksum 或 firmware-ready
查询，也不注册 Wi-Fi MAC/netdev。第三包嵌入内容已再次提取并比对参考映像，主机构建、ELF 检查、
静态 CI、ShellCheck 和 GPL 文件主体 nxstyle 已通过。当前 ELF SHA-256 为
`49eafed4a13b97f0297dca4f3b6d0804604993ee8389e9624cdbd66914970930`，RAM-only 包目录为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-section0-third-packet-static-tuning`。在串口取得
`RTL8852BS2 FW section0 third packet complete`、Bluetooth H5 和 `nsh>` 前，不能声称第三包已被
卡端接受。

### 2026-08-22：第三 packet 实板前置失败与 CMD52 恢复待验证

第三 packet 的 RAM-only 验收实际已执行一次：U-Boot 被自动截停，54-byte wrapper 与
237776-byte flat image 均通过 XMODEM 写入 RAM，随后 NuttX 成功完成 CMD5、CMD19
`mismatches=0`、Function 1 `IOEN=IORDY=0x02`、bootstrap、DLE/SCC 与 HCI flow-control。
但是 SDIO pre-init 的第一笔 `0x0074` 读取产生 SDHCI status `0x000a8001`，返回 `-EILSEQ`
（`-84`），所以没有进入 FW preboot、header、section packets 或第三 packet。完整记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-section0-third-packet-static-tuning-20260822T052346Z.log`，
SHA-256 `7b16ea1bdea651bfbd8fe77c962c63826ff163fb38efbad845933df99e12c1b3`。此前另一轮在同一位置
失败的记录为 `k1-wireless-fw-section0-third-packet-static-tuning-20260822T050846Z.log`，SHA-256
`3875dfff845405be0cbb25889e4a25103a1a038752f4b1057074028964352491`。

历史日志把该错误称为 `post-power CMD53`，但 `0x0074` 属于 Function 1 CMD52 address space，代码实际
执行四笔单字节 CMD52 读取；诊断文本已修正为 `post-power F1 CMD52`。检查还发现旧
`k1_sdio_wifi_cmd52()` 在 command/response/R5 error 时直接返回，未像 CMD53 一样调用
`k1_sdio_cancel()`，故本轮新增统一的 command+data reset 收尾。仅无副作用的
`k1_sdio_wifi_f1_read_byte()` 会在 `-EILSEQ` 后最多重试三次、每次等待 10 us，并记录
`F1 CMD52 read CRC retry=`；CMD52 写和所有 CMD53（特别是 FW FIFO 写）均不重试，以避免重复提交。
该恢复仍未取得实板通过证据，下一轮必须同时看到 `SDIO pre-init complete`、第三 packet complete、H5
和 `nsh>` 才能更新第三 packet 验收结论。

恢复版已完成主机构建、ELF 检查、静态 CI、ShellCheck 和 RAM-only 打包，产物目录为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-section0-third-packet-cmd52-recovery`；ELF SHA-256 为
`3df06195a60845b52b4699cd3e5b85e7976f429ebb25ffb9958e4d9779a35500`，flat image SHA-256 为
`0c98082af8d9e03a5c5c97e4fcd8a08c3ddbe3a8346b6bf645807ef2ad134a07`。随后启动的 120-second
manual-reset listener 未收到任何 UART 启动字节，因此没有运行 U-Boot 截停、XMODEM 或 NuttX，
该空监听不构成恢复成功或失败的硬件证据。

### 2026-08-22：第三 packet 本地哨兵修正与实板验收通过

针对第三 packet 构造器的 `-EIO`，重新从上述同一 SpacemiT GPL revision 的
`hal8852b_fw.c` 提取 `array_8852b_u2_nic[0x1068..0x184b]`，并与本项目嵌入数组逐字节比较。
两者均为 2020 bytes，SHA-256 均为
`f5930f842fafb7680a7d652b828f34f6b1621cbff5581cdcf050e765e6063463`。因此 payload 并未损坏；
本地 self-check 的最后两项错误地期待倒数第 4 byte 为 `0x52`、末 byte 为 `0x9a`，实际值为
`0x58` 和 `0xd6`。修正这两个哨兵后，第三包可离开本地构造路径。

新构建仍使用原来的第三包有界诊断 profile，仅通过 `loadx + go` 运行于 RAM：
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-section0-third-packet-sentinel-fix`。
ELF SHA-256 为 `ccebbf3b87fe58befd70a0a60343984d39b47e633deff2df9220ca1b7a96094d`，flat image SHA-256 为
`583fd907ac1d37efc65077eceb6e306c919d7a96a697b7a5f8c4c3718c403dc9`。静态 CI、ShellCheck、构建和
ELF 检查均通过。最初两次 60-second manual-reset UART 监听均收到 0 bytes，未传输 image；这不构成
硬件失败。随后一次有效复位成功截停 U-Boot，54-byte wrapper 与 237776-byte flat image 均通过 XMODEM
仅加载到 RAM。完整串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-section0-third-packet-sentinel-fix-20260822T060216Z.log`
（SHA-256 `9a96123987e65e1dd7baad439833cd05c98bbd39125fa4fc4a0922d8f2b4002a`）。

该记录依次包含 `SDIO pre-init complete`、`FW preboot H2C path complete`、H2C resource diagnostic、
首包、第二包和第三包的 `available=0x20 required=0x20 FIFO=0x1c100 bytes=0x800`，每个 section
packet 都走 4 x 512-byte CMD53 block-mode transfer。最终出现
`RTL8852BS2 FW section0 third packet complete`、Bluetooth H5 local-version（subversion `0x8852`）和
`nsh>`，且没有 bring-up、cleanup 或第三包错误。因此卡端已按原厂顺序接受 section zero 的前三个
2020-byte packet 并完成 cleanup。它仍不表示完整 firmware 下载、firmware-ready、Wi-Fi MAC、`netdev`、
扫描、关联或联网已实现；后续第四包已通过新的有界 profile 获得实板验收。无论验收结果如何，
都不得发送第五包、checksum 或完整 firmware image。

### 2026-08-22：section-zero 第四 packet 实板验收通过

`wireless_fw_section0_fourth_packet_static_tuning_diag` 在已验收的前三包序列后，只追加一个
2020-byte 的第四包。payload 从 SpacemiT Linux `k1-bl-v2.2.y` revision
`31c449aeaad8c7759bc983ca0e26946e5b6746dc` 的 `array_8852b_u2_nic` offset
`0x184c..0x202f` 提取，大小为 2020 bytes，SHA-256 为
`c9f5153472dd1c6cb0b521c21d5f06d1bc9fcf8be31ba606d72ecf97f23f9d45`；来源参考文件
`hal8852b_fw.c` 的 SHA-256 为
`0976181dbd9e1f53576b51021e4765e84019dd5c448a47eeece94ba22d548e3f`。嵌入 payload 已与该
切片逐字节比对，首字节为 `40 ea`，末尾四字节为 `0e 61 01 6c`。

该 profile 在一个临时 WCPU FWDL 会话中只发送 static header 和四个 2020-byte section packets；
每包均为原厂 24-byte descriptor 加固定地址、4 x 512-byte CMD53 FIFO 写。第四包后执行原有
cleanup，不发送第五包、section checksum 或 firmware-ready 查询，也不注册 Wi-Fi MAC、`netdev`、
扫描、关联或联网。

2026-08-22 已用该 profile 完成 RAM-only 实板验收。U-Boot 被自动截停，仅通过 `loadx + go`
载入 54-byte wrapper 和 241872-byte flat image；没有执行 `saveenv`、FDL、fastboot、eMMC 或 SPI
写入。ELF SHA-256 为
`ecc40e292573df1d4a3af0d318b3236718ad00de76bf3e48b5f19b23fa833c68`，flat image SHA-256 为
`4ba8c6b70f3212642f24dbffdddbb3d3d1c2d72e08b3ca69cbbe22aeb14c75ab`。完整原始串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-section0-fourth-packet-20260822T062136Z.log`
（SHA-256 `70e06d7ab15c7e2452b5b354309b417c32ce64d6873fb9573230d2abd8cefc30`）。

同一份记录依次出现 `SDIO pre-init complete`、`FW preboot H2C path complete`，及首至第四包各自
`available=0x20 required=0x20 FIFO=0x1c100 bytes=0x800`。随后出现
`RTL8852BS2 FW section0 fourth packet complete`、Bluetooth H5 local-version（subversion
`0x8852`）和 `nsh>`；未出现 K1 Wi-Fi bring-up、FWDL cleanup 或第四包诊断错误。这证明卡端已按
原厂顺序接受 section zero 的前四个 2020-byte packet 并完成 cleanup。它仍不表示完整 firmware
下载、firmware-ready、Wi-Fi MAC、`netdev`、扫描、关联或联网已实现；不得据此发送第五包、checksum
或完整 firmware image。

### 2026-08-22：按原厂解析完整 U2 NIC 镜像布局

后续工作不再把前四个已验证的 2020-byte packet 扩展为一串手写切片。新配置
`wireless_fw_image_layout_static_tuning_diag` 按 SpacemiT Linux 6.6 revision
`31c449aeaad8c7759bc983ca0e26946e5b6746dc` 的 `fwhdr_hdr_parser()`、
`fwhdr_section_parser()` 与 `mac_get_dynamic_hdr_ax()` 解析 `array_8852b_u2_nic` 的 header
元数据。它不提交任何 CMD53 FIFO write，也不改变 WCPU 状态，因此是完整下载前的只读布局
验收点。

该 image 的 static header 为 `0x50` bytes；header 的 dynamic flag 已置位，dynamic header 也是
`0x50` bytes，故 section data 从 `0x0a0` 开始。header 给出三条 section：section 0 的下载
地址为 `0xb8970000`，长度为 `0x3f420`，checksum flag 未置位；section 1 地址为
`0xb8e12400`，长度为 `0x3780`，checksum flag 也未置位；section 2
地址为 `0xb8e11928`，类型为 secure (`9`)，长度 `0x800`，MSSC 为 `2`。三段按原厂顺序结束于
`0x43440`，完整 image 长 `0x43840`；其余 `0x400` bytes 正好是 MSSC 指定的两份 512-byte
secure signature，不能当作普通 section packet 发送。

完整下载必须保留这三段的精确边界，并对未来 checksum flag 置位的 image 应用原厂的 8-byte
扩展规则；全部 section 成功后才实现原厂
`check_fw_rdy()`，即轮询 `R_AX_WCPU_FW_CTRL` (`0x01e0`) 的 `[7:5] == 7`，同时处理 checksum、
security 和 cut mismatch 状态。此配置只验证这些前提，尚未导入完整 276544-byte firmware、Wi-Fi
MAC/netdev、扫描、关联或联网。

### 2026-08-22：MSS eFuse/key-pool 选择器只读诊断实板验收通过

此 profile 的 eFuse read 行为已经实板验证，但后续源码核对发现其不是当前 image 的实际签名分支。
`array_8852b_u2_nic` 的 secure section 为 `MSSC=2`，原始 trailer 内也没有 `MSSKPOOL` magic；
它应走原厂 `__mss_index()` 的 legacy two-signature 分支。`get_mss_keypool_index()` 只适用于
`MSSC` 低 byte 为 `0xff` 的较新 image。新配置
`wireless_fw_mss_efuse_static_tuning_diag` 仅实现此流程的最前两步：按原厂
`enable_efuse_sw_pwr_cut_8852b(..., false)`、`read_hw_efuse()` 与对应 disable helper 临时开放
eFuse read path，读取并打印 key-pool selector 格式的两个 byte 与解码结果。

这不是 eFuse 烧录：实现不写 `R_AX_PMC_DBG_CTRL2 + 3` 的 unlock code，不写 eFuse data field，
不访问 burn control，也不发送 firmware FIFO packet。无论两次读取是否成功，代码都会尝试先关闭
eFuse read power gate。该 profile 继承 layout profile，不执行 WCPU FWDL preboot；因此实板成功
标志仅为 `RTL8852BS2 MSS eFuse diagnostic complete`，不能据此声称签名、firmware、Wi-Fi MAC、
`netdev`、扫描、关联、联网或蓝牙 firmware download 已完成。

2026-08-22 已通过 `loadx + go` 的 RAM-only 实板验收。最终 ELF 为
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-mss-efuse-static-tuning/nuttx`，SHA-256
`e23ccd551d46f3dc482b228668fe88a21bca3745f8a8d508ec9d51c1925122b9`；flat payload 为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-mss-efuse-static-tuning/contest-nuttx-flat.bin`，
SHA-256 `fdb2d7a8ae8f9561e79af33ef1ea2942cec1a33e1278d94f092267adbf0299e2`。串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-mss-efuse-static-tuning-20260822T090959Z.log`，
SHA-256 `4b91ffb9b382b06a392578af612bb77b73e794d76f88910a9c6a27fc1d39a954`。

日志依次确认 Function 1 `IOEN/IORDY=2`、first vendor CMD53、DLE/SCC、HCI flow-control、SDIO
pre-init 与 U2 NIC layout 均通过，然后输出 `external-pn=ff`、`customer=ff`、raw/pool device
type `f`、customer index `0`、key number `0`，最后 H5 local-version 的 subversion 为 `0x8852`
并进入 `nsh>`。此 eFuse 组合也恰好按 legacy `__mss_index()` 映射到 index `0`。不过这个旧 profile
没有导入或验证 signature trailer，不能据此声称 MSS 签名选择、完整 firmware download、firmware-ready、
Wi-Fi MAC/`netdev`、扫描、关联、联网或蓝牙 firmware download 已完成。

### 2026-08-22：legacy MSS signature 选择诊断实板验收通过

`wireless_fw_mss_legacy_signature_static_tuning_diag` 更正了前一节的路径判断。它直接复刻原厂
`fwhdr_parser()` 在 `MSSC=2` 时的 legacy branch：section data 在 `0x43440` 结束，后随两份各
512-byte signature；eFuse `0x5ec/0x5ed` 经 `__mss_index()` 与 `otpkeysinfo` 的两条 mapping 选出
index，原厂随后会把所选 signature 从 `0x43440 + index * 0x200` 拷入 secure section
`0x42c40 + 0x1c0`。本 profile 验证这些边界与选中 signature 的首尾 word，但不会执行该 memcpy，
不会进入 WCPU download state，也没有任何 CMD53 FIFO write。

已从同一 GPL reference 的 `array_8852b_u2_nic[0x43440..0x4383f]` 导入精确 1024 bytes；与原始
slice 的 SHA-256 均为 `71632fde61d80f1230c387c354e72aba717d1cbdb864f79a7c35450d8b25ac48`。构建、
ELF 检查及 RAM-only 实板验收均已通过：ELF 为
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-mss-legacy-signature-static-tuning/nuttx`，
SHA-256 `8c08d0d3b5560d3bd552db992e0090606dbec83e405dd9ca9cd028974e3a6564`；flat payload 为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-mss-legacy-signature-static-tuning/contest-nuttx-flat.bin`，
SHA-256 `30b0835d864e4695a558b459cb8e0b9e44ae26a131dce4561309d0026a041b96`。串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-mss-legacy-signature-static-tuning-20260822T092901Z.log`，
SHA-256 `3876f1cd0a34f6ab70ac5875894b194508e59f901d1ffb0c6cc76b2a536a0789`。

日志确认 legacy eFuse selector `ff/ff` 解码为 index `0`、signature source/target 分别为
`0x43440/0x42e00`、长度 `0x200`，并出现 `RTL8852BS2 MSS legacy signature diagnostic complete`、
Bluetooth H5 local-version 和 `nsh>`。这只完成原厂 legacy signature 选择路径的只读验证；不复制
signature，不进入 FWDL preboot，不发送 firmware FIFO 数据，不写 eFuse 或持久存储。因此，完整
firmware download/ready、Wi-Fi MAC/`netdev`、扫描、关联、联网及 Bluetooth firmware download 仍未完成。

### 2026-08-22：完整 U2 NIC firmware download 已实现，待 RAM-only 实板验收

新配置 `wireless_fw_full_download_static_tuning_diag` 在已经验证的 static tuning、Function 1 enable、
GPL bootstrap、DLE/SCC、HCI flow-control、SDIO pre-init、image layout 和 legacy MSS selector 基础上，
实现了完整 `array_8852b_u2_nic` 的下载路径。映像由
`tools/extract_rtl8852bs_u2_nic_fw.sh` 从 SpacemiT Linux `k1-bl-v2.2.y` revision
`31c449aeaad8c7759bc983ca0e26946e5b6746dc` 的 `hal8852b_fw.c` 机械提取；嵌入文件为
`chip/k1/k1_rtl8852bs_u2_nic_fw.inc`，长度 276544 bytes，SHA-256 为
`f06291e7c20461f4ee597edf70d35e06338a9c16010b5dff5ab03811535eb861`。提取结果已重新生成并与
原始 source array 逐字节 `cmp` 验证。

实现按原厂 parser 校验 static/dynamic header、三条 section、checksum-tail 边界和 legacy trailer，
以 `__mss_index()` 的实际 fallback 规则选择 signature。在发送 secure section 时，仅修改当前
2048-byte packet buffer 中对应的 512-byte signature，原始映像不被改写。每个 packet 均通过已验证的
SDIO H2C FIFO path 发送；全部 section 完成后，代码轮询 `R_AX_WCPU_FW_CTRL[7:5]`，只在值为 `7`
时保留 WCPU 运行。任何下载失败均执行 `k1_rtl8852bs_fwdl_cleanup()`，关闭下载/WCPU/clock 状态。

该 profile 的 firmware 仅经 U-Boot `loadx + go` 载入 NuttX 的 RAM，再下载到 Wi-Fi WCPU 的易失 RAM。
实现和上板流程不调用 `saveenv`，也不写 eFuse、eMMC、SPI flash、FDL 或 fastboot。2026-08-22 已完成
最终干净构建、ELF 检查和 RAM-only 打包：ELF 为
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-full-download-static-tuning/nuttx`，SHA-256
`c068a9c3a53e4d91dee492f37cc05c6ea867ed0cddf66bcb44b995864b91c607`；flat payload 为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-full-download-static-tuning/contest-nuttx-flat.bin`，
SHA-256 `bc8993e4a3e0e5acb0ceb21218343055917bab5767f505aac5d17c079175e144`。包内已包含 GPL-2.0-only
文本及 `k1_rtl8852bs_gpl.[ch]`、`k1_rtl8852bs_u2_nic_fw.inc` 对应源码。

同日启动过一次 `--manual-reset` 验收监听，但在 45 秒窗口内收到 0 bytes；没有进入 U-Boot、没有执行
XMODEM、没有传输或写入任何镜像，因此这是一份空记录，不构成 firmware 下载失败。当前待做的是在
串口监听已经打开后按一次物理 RST。只有同时取得 `full FWDL ready status=7`、H5 local-version 和
`nsh>`，并保留完整串口日志与 SHA-256，才可声称 firmware-ready；在此之前，Wi-Fi MAC/`netdev`、
扫描、关联、联网及 Bluetooth vendor firmware/stack 均未完成。

随后的一次有效 RAM-only 上板在 `loadx + go` 后完成了 DLE/SCC、HCI flow-control、SDIO pre-init、
U2 layout、legacy MSS selector 与 Bluetooth H5，但完整下载在 `FIFO=0x1c100` 返回 `-EINVAL` 后清理并
进入 `nsh>`。记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-full-download-static-tuning-20260822T100036Z.log`，
SHA-256 `7088e1d82e4e12033b89b0431190477b36f7ff16bd6ce21214d34e08f513edc4`。日志证明失败发生在卡端
FWDL-ready 之前，且没有写入持久存储。源码复核确认当时 full-download config 缺少
`CONFIG_K1_RTL8852BS2_FW_SECTION_PACKET_DIAGNOSTIC`，但 `k1_sdio_wifi_cmd53()` 的 2048-byte
block-mode guard、`K1_SDIO_CMD53_MAX_BLOCK_COUNT` 和 ADMA bounce buffer 均错误地只由旧的
single-packet config 开启。因此 full profile 的首个 section packet 在主机预检阶段直接返回
`-EINVAL`；日志中 full FWDL begin 之后 `CMD53 block count=4` 的出现次数为 0，排除了卡端拒绝或
firmware payload 损坏。

该门控现已改为同时允许 full-download profile 的固定 `4 x 512 = 2048`-byte packet，仍不允许任何
通用的大块 CMD53 写。修复版已通过静态 CI、干净构建和 ELF 检查；ELF SHA-256 为
`c068a9c3a53e4d91dee492f37cc05c6ea867ed0cddf66bcb44b995864b91c607`，flat payload SHA-256 为
`bc8993e4a3e0e5acb0ceb21218343055917bab5767f505aac5d17c079175e144`。随后两次 60/180 秒
manual-reset 监听均收到 0 bytes，未运行新 payload；它们不构成修复成功或失败的硬件证据。下一轮
有效复位必须确认至少有一笔 `CMD53 block count=4`，再根据 section progress 或 `full FWDL ready
status=7` 继续诊断。

### 2026-08-22：完整下载短包 FIFO 传输修复已构建，待实板复验

上一版有效 RAM-only 记录在首个 2048-byte packet 后确实进入了 block-mode，但在 section 0 的尾包
报 `full FWDL error address=0x000000000001c047 status=0x0 error=0x16`。该 FIFO 地址的低位 `0x47`
表示 Realtek 的逻辑 packet 长度为 `0x47 * 8 = 568` bytes：544-byte firmware remainder 加
24-byte H2C descriptor。原来的 K1 FWDL transport 只接受恰好 `4 x 512` bytes，因此在这个短包的
主机侧预检中返回 `-EINVAL`；该行为与 Linux Realtek SDIO 层不一致。

当前修复增加专用 `k1_sdio_wifi_fwdl_write()`，它不放宽通用 CMD53 写接口，只允许 Function 1 的
固定 `0x1c000` FIFO、513 至 2048 bytes、8-byte 对齐且 FIFO 低位等于逻辑长度编码的 full-FWDL
packet。传输层保留逻辑 FIFO 地址，物理 CMD53 长度则向上取整到 512-byte block 边界，并对尾部
zero-pad；因此该 568-byte packet 应变为 `2 x 512`-byte CMD53，后续完整包仍为 `4 x 512`。ADMA
bounce buffer 由 2048-byte 物理长度驱动并明确清零 padding，不读取调用者 buffer 范围外的内容。

该版本已通过 `tools/ci_k1.sh --static-only`、`git diff --check`、干净构建、ELF 检查和 RAM-only
打包。当前产物为 ELF
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-full-download-static-tuning/nuttx`，SHA-256
`0af28ed375aa9ade8b1a44efe249febda0615e8e14bb968adcfcb8bc4c71a3be`；flat payload 为
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-full-download-static-tuning/contest-nuttx-flat.bin`，
SHA-256 `d955023daa9a620e6ceb96722d1bb74abe4d08d0efcae91d8254eec760847c8b`；54-byte U-Boot wrapper
SHA-256 `4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81`。构建工具也已修正比赛仓库
相对 `--config board/...` 路径的解析，不改变板端二进制。

两次新版本 `--manual-reset` 监听分别生成
`k1-wireless-fw-full-download-static-tuning-20260822T110755Z.log` 和
`k1-wireless-fw-full-download-static-tuning-20260822T111119Z.log`，均为 0 bytes；没有进入 U-Boot，
没有 XMODEM 传输，也没有任何 persistent storage 写入。这些空记录不构成实板通过或失败。下一次必须
在 listener 已启动后按物理 RST；成功证据应至少包含尾包处的 `CMD53 block count=2`、三条 section
完成、`full FWDL ready status=7`、Bluetooth H5 local-version 和 `nsh>`。在取得这些证据前，firmware
ready、Wi-Fi MAC/`netdev`、扫描、关联、联网及 Bluetooth vendor firmware/stack 仍未完成。

### 2026-08-22：section 0 完整下载实板通过，section 1 尾包规则已修正

有效 RAM-only 记录
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-full-download-static-tuning-20260822T112400Z.log`
（SHA-256 `744cbff606ebdc6bbb3a0638f63378221fcdab49126d7ad22995cd4e74072e83`）首次证明 568-byte
section-0 尾包的 block padding 修复正确：日志在 FIFO `0x1c047` 输出 `CMD53 block count=2`，随后输出
`full FWDL section bytes=0x3f420 packets=0x81 type=0x2`。这说明 section 0 的 81 个 packet 全部由卡端
接受，原先的 host-side `-EINVAL` 已排除。

同一记录继续进入 section 1，并在其最后一个 96-byte logical packet 的 FIFO `0x1c00c` 返回 `-EINVAL`。
对 GPL reference 的 `rtw_sdio_cmd53_align_size()` 复核确认原厂语义为：`len <= block_size` 时保留
byte-mode，只有 `len > block_size` 时才向上取整。因此 full-FWDL 专用接口已更正为允许 8 至 512-byte、
8-byte 对齐的固定 FIFO byte-mode packet；513 至 2048-byte packet 仍按 512-byte block 边界 zero-pad。
这保持了受限接口的 Function 1、固定地址和 FIFO 长度编码检查，不向通用 CMD53 写开放大包。

修正后的 RAM-only 包 ELF SHA-256 为
`63d6895892bc7ed3e4688762721ba246e5108ba104c17da341d1b247740d7d85`，flat payload SHA-256 为
`3e1abe8e17ba9f770bfd4616e594ec40571992ab546a7c7016f12891e97f6457`。其第一次实板启动在 FWDL 之前的
`R_AX_HCI_OPT_CTRL` (`0x74`) Function 1 CMD52 read 遇到瞬态 CRC/sequence error (`-84`) 并安全进入
`nsh>`；记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-full-download-static-tuning-20260822T112911Z.log`
（SHA-256 `25539331bd8904aee2c68e3fc93554fcb29878afbcf4aff55d23762f34d70a99`）。该地址和流程在上一份成功
section-0 记录中已通过，且 byte-mode tail code 尚未执行，所以此错误不能归因于尾包补丁。下一次同一包的
RAM-only 复测应确认 `0x1c00c` 使用 byte-mode、section 1/2 完成及 `full FWDL ready status=7`；在此之前
仍不得声称 firmware-ready、Wi-Fi MAC/`netdev` 或联网完成。

### 2026-08-22：完整 U2 NIC firmware download RAM-only 实板验收通过

修正 byte-mode 尾包规则后的有效记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-full-download-static-tuning-20260822T113319Z.log`
（503017 bytes，SHA-256 `e7689ca8599fa6a88ab77f8ba2af1172911815fe5866406ee512a668ec7f0204`）。U-Boot 自动截停，
两个 watchdog 均停止；54-byte wrapper 和 512208-byte flat payload 仅通过 `loadx + go` 载入 RAM。
没有执行 `saveenv`、FDL、fastboot、eMMC/SPI 写入或 eFuse 编程。

记录确认了完整的原厂 U2 NIC 下载序列：section 0 的 81 个 packet 在 `0x1c047` 尾包采用
`CMD53 block count=2` 后完成；section 1 的 8 个 packet 完成，其中最后 96-byte logical packet 保持
byte-mode；secure section 的两个 packet 也完成。随后 `R_AX_WCPU_FW_CTRL[7:5]` 返回
`full FWDL ready status=7`，Bluetooth H5 local-version 的 subversion 为 `0x8852`，最终进入 `nsh>`，
且没有 `K1 RTL8852BS2 bring-up failed`。这证明 Wi-Fi WCPU firmware 已在易失 RAM 中运行。

本次验收包 ELF SHA-256 为 `63d6895892bc7ed3e4688762721ba246e5108ba104c17da341d1b247740d7d85`，
flat payload SHA-256 为 `3e1abe8e17ba9f770bfd4616e594ec40571992ab546a7c7016f12891e97f6457`，U-Boot wrapper
SHA-256 为 `4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81`。此结果仅覆盖 firmware
download/ready；Wi-Fi MAC 初始化、802.11 `netdev` 注册、扫描、关联和 IP 联网，以及 Bluetooth vendor
firmware、HCI stack 注册和扫描仍待实现。

### 2026-08-22：eFuse logical MAC 与 WCPU runtime 诊断已构建，待实板验证

新增 profile
board/k1/muse_pi_pro/configs/wireless_fw_runtime_static_tuning_diag。它在完整
U2 NIC firmware download 前只读 Wi-Fi physical eFuse 的 1536 bytes，跳过原厂定义的
4-byte secure-control prefix，并按 Realtek WLAN two-byte header parser 解成 2048-byte logical
map。随后校验并打印 logical offset 0x41a 的单播、非全零、非全 ff MAC。该操作不包含
eFuse unlock code、data-field write 或 burn control。

完整 firmware download 成功后，该 profile 再读取 R_AX_WCPU_FW_CTRL、
R_AX_PLATFORM_ENABLE 和 R_AX_SYS_CLK_CTRL；只有 firmware status 为 7、WCPU enable
和 CPU clock enable 都保持置位时，才输出
RTL8852BS2 firmware runtime diagnostic complete。它仍不注册 Wi-Fi MAC 或 netdev，
不扫描、不关联、不联网，也不写 eMMC、SPI、U-Boot environment 或 eFuse。

静态 CI、干净构建、ELF 检查和 RAM-only package 已通过。ELF 为
/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-runtime-static-tuning/nuttx，
SHA-256 3c046d935313d86ec9125c7cad24c4addeee36562bc1c8c936f00ae5dcaf7e85；package 位于
/home/sw/Dev/k1-workspace/out/k1-wireless-fw-runtime-static-tuning。实板验收应运行
tools/run_k1_wireless_smoke.py --manual-reset，并要求 full-download、firmware-runtime、H5 和
bringup-success markers。三次已打开的 manual-reset 监听均在未收到任何 UART byte 的情况下超时，
因此它们没有运行新镜像，也不构成硬件通过或失败证据。下一次必须在 listener 已显示等待后按物理 RST。

### 2026-08-22：runtime eFuse/MAC/WCPU 实板验收通过

本 profile 的第一次有效启动已完成 bootstrap、DLE/SCC、HCI flow-control、SDIO pre-init 与
legacy signature 选择，但 180-second host timeout 在 1536-byte physical eFuse 的逐字节读取期间
关闭了串口。该次没有发生 SDIO 或 firmware 下载错误，但也没有得到 runtime 结论。原因是早期
`k1_sdio_sendcmd()` 会为每一笔成功的 CMD52/CMD53 输出寄存器转储；UART 日志自身成为瓶颈。

修复后，`k1_sdio_wifi_suppress_command_trace()` 只在此受限、只读的完整 eFuse 扫描期间抑制
成功命令的寄存器转储；SDHCI 错误输出、eFuse read begin/complete、logical MAC、FWDL 和 runtime
标记均保持。它不改变任何 SDIO 命令、eFuse read gate、eFuse 写入权限、firmware packet 或持久存储
行为。`tools/run_k1_wireless_smoke.py --manual-reset` 也改为默认先打开 listener、再播报“请按一下
复位按钮”；只有显式 `--no-voice-prompt` 才会静音，旧的 `--voice-prompt` 保留兼容性。

最终有效的 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-static-tuning-20260822T134836Z.log`
（537652 bytes，SHA-256
`6d12b660052edea64411eea4fadf359781fce98c1b71fd1af6029957aee10ec5`）。监听先于物理 RST 打开，
U-Boot 自动截停，两个 watchdog 均停止；54-byte wrapper 和 516304-byte payload 仅经 `loadx + go`
载入 RAM。没有使用 `saveenv`、FDL、fastboot，也没有写入 eMMC、SPI 或 eFuse。

记录依次确认：physical eFuse read complete、logical MAC
`84:fc:14:06:79:7b`、三段 full FWDL（81、8、2 packets）、`full FWDL ready status=7`、
runtime `FW status=7`、WCPU enable/CPU clock 仍置位、firmware runtime diagnostic complete、Bluetooth
H5 local-version（subversion `0x8852`）和最终 `nsh>`。因此 Wi-Fi WCPU firmware 的易失运行态与
eFuse MAC 读取均已实板验证。Wi-Fi MAC/802.11 `netdev`、扫描、关联、IP 联网，以及 Bluetooth vendor
firmware、HCI stack 注册和扫描仍未实现。

本次验证 ELF SHA-256 为 `0d2e567fa4ed54c5a284ceed778fb01e13fb9ee61caedb8b2bfba5550ea5bd36`，flat
payload SHA-256 为 `0032064117d08b0f8520d30311f4e418d7c741d8abee0cdd1691a82dcdde9311`，wrapper SHA-256
为 `4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81`。

### 2026-08-22：NuttX `/dev/ttyHCI0` H5 active 实板验收通过

专用 `wireless_bt_hci_only_diag` profile 关闭原始 local-version probe，并启用 NuttX
`DEBUG_WIRELESS_INFO`，使 `bt_slip` 从控制器复位后独占 H5 初始协商。有效 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-bt-hci-only-20260822T145034Z.log`
（同一 profile 的早期成功记录为 `...144715Z.log`）。
U-Boot 自动截停，54-byte wrapper 与 235152-byte flat payload 仅经 `loadx + go` 载入 RAM；没有
`saveenv`、FDL、fastboot、eMMC/SPI 写入或 eFuse 编程。

在 NSH 执行 `cat /dev/ttyHCI0` 后，日志依次记录 `H5 transport opened`、SYNC response、CONFIG
response、`h5 txwin:4, dipresent:1` 和 `h5: active`。这实证了 UART2 H5 lower-half、NuttX
`bt_slip` 收发 worker、CONFIG 中的 CRC 协商以及 `/dev/ttyHCI0` 注册均可在实板运行。

`cat /dev/ttyHCI0` 会在成功后继续等待 HCI 数据，因此 RAM-only smoke 工具以 `h5: active` 作为
通过标志并关闭主机串口，不再把前台读取尚未返回 NSH 误报为失败；后续测试通过物理 RST 重置板卡。
该 profile 没有发送厂商蓝牙 firmware/config，也没有验证通用 HCI 数据、host stack 或扫描，不能据此
宣称蓝牙功能可用。标准 HCI Reset 的单独验收见下一节。

### 2026-08-22：标准 HCI Reset RAM-only 实板验收通过

新增 `wireless_bt_hci_reset_diag` profile 在原始 H5 `SYNC -> CONFIG` 和 HCI Read Local Version
成功后，发送标准 HCI Reset（opcode `0x0c03`），并只接受该 opcode 的成功 Command Complete event。
实现遵循原厂 GPL-2.0 `spacemit-com/rtk_hciattach` 的 post-download 顺序，但本 profile 不导入或
发送其 `rtl8852bs_fw` / `rtl8852bs_config`，也不发送任意厂商 HCI 命令。

有效 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-bt-hci-reset-20260822T145909Z.log`
（136721 bytes，SHA-256
`f3d71552316765e819e8fbff1fa276181eb8b3b27d32bf1eba7aeafc1fb15195`）。监听先于物理 RST
打开，U-Boot 自动截停；两个 watchdog 均停止，54-byte wrapper 与 239248-byte flat payload 仅经
`loadx + go` 载入 RAM。没有执行 `saveenv`、FDL、fastboot、eMMC/SPI 写入或 eFuse 编程。

日志依次出现 `K1 Bluetooth: H5 HCI reset complete`、HCI local-version（manufacturer `0x005d`、
subversion `0x8852`、CRC enabled）、`H5 stack registered /dev/ttyHCI0` 和 `nsh>`，且没有
`K1 RTL8852BS2 bring-up failed`。验收 ELF SHA-256 为
`66675150998061e7058c58f5b73f61f0d07d636576f431b4caf076604c672ae1`，flat payload SHA-256 为
`2da1b3005be9f68b486ad21098690706c653a955fa1f03bdd75902f562a1e536`。

这证明原始、可靠、CRC-protected H5 会话能完成一个标准 HCI 命令及其完成事件；它仍不证明 Realtek
vendor firmware/config 下载、正常 HCI 数据、Bluetooth host stack、扫描、配对或连接。

### 2026-08-23：runtime FWDL 复验与快速 XMODEM 实板通过

`tools/load_k1_xmodem.py` 过去固定发送 128-byte SOH XMODEM blocks。对于包含完整
RTL8852BS2 firmware 的 533136-byte RAM payload，这需要 4166 次逐块 ACK；一次物理
RST 后的记录表明传输落在 U-Boot 约 60 秒 watchdog 窗口边界，payload 尚未执行时板子已
重启，不能归因为 Wi-Fi driver 错误。K1 的 U-Boot 2022.10 `common/xyzModem.c` 支持
1024-byte STX blocks，故工具现在仅对大于 128 bytes 的文件使用 1 KiB block；54-byte
wrapper 继续使用 128-byte block。该变更只修改主机 XMODEM framing，不修改 U-Boot、eMMC、
SPI、eFuse 或任何板端持久状态。

自动 ADB reboot 后的有效 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-cmd52-recovery-20260823T103524Z.log`。
它在 `loadx + go` 路径中于 U-Boot 时间 `51.423 s` 完成 533136-byte payload，随后通过
Function 1 SDIO bootstrap、`SDIO pre-init complete`、physical eFuse read 和 logical MAC
`84:fc:14:06:79:7b`。完整 WCPU download 的三段分别报告 81、8、2 packets，
`full FWDL ready status=7`；运行态再读回 firmware status `7`、WCPU enable 和 CPU clock
enable，输出 `firmware runtime diagnostic complete`，且 Bluetooth H5 local-version 回归到
subversion `0x8852` 与 `nsh>`。本轮没有出现 `F1 CMD52 read CRC retry` 或
`K1 RTL8852BS2 bring-up failed`。

因此，Wi-Fi SDIO transport、eFuse MAC、完整 firmware download 和 WCPU 易失运行态均已
重新实板验证。Wi-Fi MAC/802.11 `netdev` 注册、扫描、关联和 IP 联网仍未实现，下一步必须
基于 GPL 原厂 RX/TX 与 802.11 控制路径移植，而不是把这份 runtime diagnosis 误写为联网通过。

### 2026-08-23：运行期 SDIO transport 只读诊断已实现，待实板验收

新增 profile `wireless_fw_runtime_transport_diag` 继承已实板通过的完整 firmware download、
eFuse MAC 与 WCPU runtime profile。它在 WCPU firmware-ready 之后，通过 Function 1 CMD53
依次读取 Realtek `R_AX_SDIO_HIMR` (`0x1100`)、`R_AX_SDIO_HISR` (`0x1104`) 和
`R_AX_SDIO_RX_REQ_LEN` (`0x1108`，仅输出低 18 bit)，随后复用已验收的 28-byte
`R_AX_SDIO_TXPG_WP` (`0x1110`) CMD53 窗口读取。成功日志将包含
`RTL8852BS2 runtime transport diagnostic complete`。

寄存器和行为均按同一 GPL Realtek 参考实现交叉核对：`hci_reg_ax.h` 定义上述 offsets 与
`RX_REQ_LEN` mask；`hal_api_mac.c` 先读取该长度，仅在非零时读取 `RXFF`；
`rtl8852bs_halinit.c` 的运行期中断路径读取 HISR，并只对 Bluetooth bit 做 W1C clear。
因此该 profile 不写 HIMR/HISR、不启用或确认中断、不读 RX FIFO，也不注册 Wi-Fi MAC、
`netdev`、扫描、关联或联网；它不包含任何 eFuse、eMMC、SPI、FDL、fastboot 或 U-Boot
environment 持久写入。

构建和 RAM-only 验收命令如下：

```bash
tools/build_k1.sh \
  --config board/k1/muse_pi_pro/configs/wireless_fw_runtime_transport_diag \
  --build-dir cmake_out/k1-wireless-fw-runtime-transport \
  --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-fw-runtime-transport/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-fw-runtime-transport/k1-go-wrapper.bin \
  --require-firmware-full-download \
  --require-firmware-runtime \
  --require-runtime-transport \
  --require-h5 \
  --require-bringup-success
```

在取得包含该完成标记、H5 local-version 和 `nsh>` 的完整 RAM-only 串口记录前，本项只能称为
已实现、待实板验收。即使验收通过，也只证明运行期 SDIO 状态读取与 TX-page CMD53 window 可用；
RX descriptor parsing、调度 RX worker、TX path、802.11 MAC/netdev、扫描、关联和 IP 联网仍未实现。

### 2026-08-23：运行期 SDIO transport RAM-only 实板验收通过

有效记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-transport-20260823T110813Z.log`，
SHA-256 为 `f119bd89c69f812dca20fe153a86db178dbef41b4425f90b4f7260097230f1ee`。工具先通过
ADB reboot 自动截停 U-Boot、停止两个 watchdog，再仅用 `loadx + go` 将 54-byte wrapper 和
533136-byte payload 载入 RAM；没有调用 `saveenv`、FDL、fastboot，也没有写 eMMC、SPI 或 eFuse。

该记录依次包含 `full FWDL ready status=7`、firmware runtime diagnostic complete、
`runtime SDIO HIMR=0 HISR=0 RXREQ=0 H2C-pages=0x20`、runtime transport diagnostic complete、
Bluetooth H5 local-version（subversion `0x8852`）和最终 `nsh>`，没有 bring-up failed 或
transport diagnostic error。HIMR/HISR/RXREQ 均为零符合当前诊断没有启用 SDIO HCI interrupt、
也没有待处理 RX request 的状态；这验证了 Function 1 CMD53 状态读取和 TX-page window，**不是**
RX FIFO、RX worker、802.11 MAC/netdev、扫描、关联或联网通过证据。

验收 ELF
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-runtime-transport/nuttx` 的 SHA-256 为
`2ba01fd4af5ea213ebb2ed58235698e1ac421b6859d6c86812d596e061c96e97`；flat payload 的 SHA-256 为
`2569e8320f4d0aba2c88fb715d0f5a6ec74f6a0262b7cc2cb04ec2a5bf588a12`，wrapper 的 SHA-256 为
`4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81`。

### 2026-08-23：运行期 H2C/C2H loopback RAM-only 实板验收通过

`wireless_fw_runtime_h2c_loopback_diag` 在已经验收的 full FWDL、runtime state 和
runtime transport 路径之后，按 GPL Realtek `mac_fwcmd_lb()` 发送 TEST/CMD_PATH/H2C_LB
命令。它通过 Function 1 的固定 TX FIFO `0x1c007` 写入 56-byte normal H2C 包（24-byte
递增 payload），轮询 `R_AX_SDIO_RX_REQ_LEN`，再从固定 RX FIFO `0x1f00` 读取并校验 RX
descriptor、C2H header 和回显 payload。该实现没有启用中断、注册 802.11 MAC/netdev、扫描、
关联或联网，且不写 eFuse、eMMC、SPI、FDL、fastboot 或 U-Boot environment。

有效记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-h2c-loopback-20260823T115154Z.log`，
SHA-256 为 `174201122850c7adb8b0367a0d499c66669e3ce7b9f60f79920a494cd248165f`。启动前先由原厂
Linux 成功恢复 `mmc1` SDR104/`wlan0`，随后工具通过 ADB reboot 截停 U-Boot、停止两个
watchdog，并只用 `loadx + go` 加载 54-byte wrapper 和 537232-byte payload 到 RAM。日志依次
确认 `full FWDL ready status=7`、firmware runtime diagnostic complete、runtime transport
diagnostic complete，以及 `runtime H2C loopback pages=0x20 FIFO=0x1c007 RXREQ=0x30` 和
`RTL8852BS2 runtime H2C/C2H loopback complete`；Bluetooth H5 local-version 也通过并进入
`nsh>`，没有 bring-up failed。

验收 ELF 为
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-runtime-h2c-loopback/nuttx`，SHA-256 为
`fde554da4875a3b2ff7f92d26dc103e0b2be25a4cda17666acc6b22cd33113c9`；flat payload SHA-256 为
`2ac046aaac076f0f17eb53f8019d47e10d0ad65e266c630f8411396cb9ba2a20`，wrapper SHA-256 为
`4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81`。

完整 firmware 路径在高串口日志量下可超过原来的 45-second NSH 等待窗口；
`run_k1_wireless_smoke.py` 的默认 `--boot-timeout` 因而提高为 120 seconds。调用者仍可按
更短或更长的诊断需要显式覆盖该值。

### 2026-08-23：运行期 RX FIFO/descriptor 通用层已实现，待实板回归

新增 GPL-2.0-only API `k1_rtl8852bs_runtime_rx_read()` 和
`k1_rtl8852bs_runtime_rx_parse()`。前者先读取 Function 1
`R_AX_SDIO_RX_REQ_LEN`，只在长度非零时通过固定地址 `RXFF=0x1f00` 发起一次
CMD53 read；没有待收数据时返回 `-EAGAIN`，不会把空 FIFO 当成功。后者是纯内存解析，按原厂
`rxdesc.h` / `trx_desc_8852b.c` 解码 short/long RX descriptor、payload length、driver-info、
shift、packet type、CRC/ICV flag，并按 RTL8852B 的 8-byte RX aggregate 对齐生成下一帧 offset。
所有长度与 offset 均在读取到的 transfer buffer 内检查。

已验收的 H2C/C2H loopback 现在通过该通用层读取和解析它的 C2H response，而不是保留第二份
专用 RX FIFO 代码。这为后续 LPWORK RX worker 提供了可复用的 transport 边界：worker 可以遍历一个
RX aggregate 中的多个 descriptor，再把 data/C2H 分派给不同上层。

`wireless_fw_runtime_h2c_loopback_diag` 的通用 RX 重构已完成 RAM-only 实板回归；对应
ELF SHA-256 为 `f44e87bcad22c43eeb627a19c926497021f6be8e7f5c3fade19362018e1e8196`。
该结果只验证共享的 RX FIFO transport 和 descriptor parser，不能据此宣称 TX data path、
802.11 MAC/`wlan0`、扫描、关联或联网完成。

### 2026-08-23：运行期 RX worker RAM-only 实板验收通过

`wireless_fw_runtime_rx_worker_diag` 在已验收的 firmware runtime 与同步 H2C/C2H
loopback 之后，再提交一份易失的 H2C loopback。LPWORK worker 读取
`R_AX_SDIO_RX_REQ_LEN`，以固定地址从 `RXFF=0x1f00` 读取整个 aggregate，并逐帧执行
`k1_rtl8852bs_runtime_rx_parse()`。本板 loopback response 的**硬件 RX descriptor**
packet-type 为 `10`；这不同于 GPL PHL `enum rtw_rx_type` 中的上层 `C2H=6`，二者不能混用。
worker 对 CRC/ICV error 计数，成功看到 C2H 后停止；若 100 个轮询周期内没有 C2H，也会停止并
打印 timeout，避免空 FIFO 的无限 CMD53 轮询。

有效记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-rx-worker-20260823T124529Z.log`，
SHA-256 为 `b0a1e13fccda657361fe9ead321a07a88272828b9ef95870832fc54adbc1712d`。测试先从原厂
Linux 只读确认 `mmc1` SDR104、4-bit、1.8 V 和 `wlan0`，再经 ADB reboot 自动截停 U-Boot，
停止 watchdog，并只用 `loadx + go` 装载 RAM image。串口依次确认 full FWDL ready status `7`、
firmware runtime、runtime transport、同步 H2C/C2H loopback，以及
`runtime RX transfer=0x30 frames=1 data=0 C2H=1` 和 `runtime RX worker complete`；H5
local-version 与最终 `nsh>` 也通过，没有 bring-up failed。

验收 ELF 为
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-runtime-rx-worker/nuttx`，SHA-256 为
`66b8d3f84507b2f2b8175f9dd0c4b5a68bf74b8f8bb08aa5ddbd2a59c1629e5f`；flat payload SHA-256 为
`95485bed82f7295aa747dded7c71e1e41483e872ada1ba061cbd1f50d65d4d18`，wrapper SHA-256 为
`4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81`。

该验收尚未实现中断驱动的持续 RX、真实 TX data queue、802.11 MAC/netdev、扫描、关联、加密
或 DHCP；它仅证明运行期的 RX FIFO 和 aggregate parsing 可用于后续控制面与数据面的实现。

### 2026-08-23：C2H header parser/dispatcher RAM-only 实板验收通过

GPL chip layer 现提供 `k1_rtl8852bs_runtime_c2h_parse()` 与
`k1_rtl8852bs_runtime_c2h_dispatch()`。前者严格解码 Realtek 通用 8-byte firmware-command
header：delivery type 必须为 C2H (`1`)，声明的 total length 必须至少包含 header 且不能越过
当前 RX frame；它再交付 category、class、function、sequence、ACK flags 与受限 content span。
后者仅在解析成功后同步调用调用者提供的 callback。content 的生命周期被限定在下一次 RX FIFO
读取前，避免未来控制面保存已经被重用的 aggregate buffer 指针。

该格式和 type 检查与 GPL Realtek Linux reference 的 `mac_process_c2h()`/
`c2h_field_parsing()` 对齐；本实现额外补充了原厂代码没有在该入口执行的 length boundary check。
同步 H2C loopback validation 也复用了同一 parser，不再手写第二份 C2H header 解码。

RX worker 仍按硬件 RX descriptor packet-type `10` 识别 C2H，然后交由该 dispatcher；只有 callback
成功返回才递增 `C2H`，并新增 `dispatched` 计数。当前 callback 仅记录已经验证过的通用 metadata，
不擅自把任何 C2H category/class/function 当作 scan、association 或 netdev 事件。smoke tool 同时要求
`C2H=1 dispatched=1`，因此 descriptor 分类、header 验证与 callback 执行三项必须全部完成。

有效 RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-rx-dispatch-20260823T130410Z.log`，
SHA-256 为 `a6c9bd1e530a6cc68853bb2bdf1a68cfe610a15cebd6c73f48f85b9e7c9e2448`。加载前原厂 Linux
已确认 `mmc1` 为 SDR104、4-bit、1.8 V 且 `wlan0` 存在；工具随后以 ADB reboot 截停 U-Boot，
停止两个 watchdog，并只执行 `loadx + go` 的 RAM load。串口确认 full FWDL ready status `7`、
runtime transport、同步 H2C/C2H loopback，以及
`runtime RX transfer=0x30 frames=1 data=0 C2H=1 dispatched=1`、`runtime RX worker complete`、
Bluetooth H5 local-version 和最终 `nsh>`；smoke 输出 `PASS: K1 runtime RX worker dispatched C2H loopback`
与 `PASS: K1 wireless RAM image reached NSH`。

验收 ELF 为
`/home/sw/Dev/k1-workspace/cmake_out/k1-wireless-fw-runtime-rx-dispatch/nuttx`，SHA-256 为
`cb8bb2964fb572463725a6da1cb5d10adc1832568b8fe45383786c3339def6d1`；flat payload SHA-256 为
`4886b11f90e4281535bcc556892d57025b67eb09386711f86e05aff0990035e9`。该成功并不代表 C2H 的
功能类 handler、持续 RX interrupt、TX data queue、802.11 MAC/netdev、scan、association 或 IP
networking 已实现；下一步是基于 GPL reference 移植普通数据帧的 TX descriptor/data queue。

### 2026-08-23：普通数据 TX descriptor 与资源 preflight 已加入，待实板验收

`wireless_fw_runtime_data_tx_diag` 在已验收的 firmware runtime 和 H2C/C2H loopback 后，新增
normal-data 传输的最小底层接口：`k1_rtl8852bs_runtime_data_tx_build()` 以原厂
`txdes_proc_data_8852b()` 的 SDIO STF 模式构造 24-byte descriptor，按 8-byte 单位得到 ACH FIFO
address；`k1_rtl8852bs_runtime_data_tx_preflight()` 再读取完整 `TXPG_WP` window，使用原厂
`ud_fs_8852b()` 的 page-counter 编码和 `chk_rqd_pg_num()` 的 PLE/WDE 算法检查资源。

当前 HCI flow-control 配置仅有 ACH0--3、B0MG 和 B0HI 六条数据 queue，因此接口明确拒绝其余
DMA channel 与 B1 WMM。诊断以 24-byte header-sized input 检查 ACH0 的预期 TXD/FIFO/PLE/WDE 数值，
**不**向 TX FIFO 写入任何 802.11 frame：此时尚未有 MACID peer、association、key 或 mac80211/netdev
状态，发送伪造帧没有可验证价值且可能污染 firmware state。

待实板时使用：

```bash
tools/build_k1.sh \
  --config board/k1/muse_pi_pro/configs/wireless_fw_runtime_data_tx_diag \
  --build-dir cmake_out/k1-wireless-fw-runtime-data-tx \
  --package --package-dir out/k1-wireless-fw-runtime-data-tx --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-fw-runtime-data-tx/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-fw-runtime-data-tx/k1-go-wrapper.bin \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-data-tx-descriptor --require-h5 --require-bringup-success
```

验收日志必须包含 `runtime data TX preflight` 和 `runtime data TX descriptor complete`；这只证明
descriptor 和当时资源快照相容，仍不代表实际 TX、RX data、扫描、关联或联网。

### 2026-08-23：静态 MAC-core 控制字段已加入，待实板验收

`wireless_fw_runtime_mac_core_diag` 在已验证的 firmware runtime、H2C/C2H loopback 和
data-TX preflight 基础上，按原厂 `mac_hal_init()` 的顺序进入 `mac_trx_init()` 的可独立部分。
它从 `trxcfg.c` 移植 `mpdu_proc_init()`、`tmac_init()`、`trxptcl_init()`、`rmac_init()`、
`cmac_com_init()`、`ptcl_init()` 和 `cmac_dma_init()` 的固定 band-0、software-TX mode 字段，
并对 20 个写入字段逐一 CMD53 indirect readback。

它刻意**不**移植 `scheduler_init()`、address CAM、RX filter policy、security engine、MACID、
role/station、scan/association H2C、BB/RF 或 interrupt enable。这些部分依赖尚未实现的
802.11 upper-half/role policy；该 profile 也不会把任何 frame 写入 TX FIFO。因此完成标记只证明
firmware-ready 状态下的静态 MAC register transport 和 readback，不代表 `wlan0`、扫描、关联、
实际数据 TX/RX 或 IP 联网。

待实板时使用：

```bash
tools/build_k1.sh \
  --config board/k1/muse_pi_pro/configs/wireless_fw_runtime_mac_core_diag \
  --build-dir cmake_out/k1-wireless-fw-runtime-mac-core \
  --package --package-dir out/k1-wireless-fw-runtime-mac-core --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-fw-runtime-mac-core/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-fw-runtime-mac-core/k1-go-wrapper.bin \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-mac-core --require-runtime-data-tx-descriptor \
  --require-h5 --require-bringup-success
```

验收日志必须出现 `runtime MAC core fields=0x14` 和 `runtime MAC core init complete`。首轮实板若
任一字段读回失败，会打印该字段编号、MAC address 和错误码；应停在该字段，不应继续尝试 MACID、
scan 或 data FIFO 写入。

### 2026-08-23：角色控制 H2C 纯 RAM 序列化已加入，待实板验收

`wireless_fw_runtime_control_plane_diag` 接在 firmware runtime、H2C/C2H loopback 和静态
MAC-core 验证之后。它按原厂 `role.c` 的 `mac_fw_role_maintain()` 和
`mac_h2c_join_info()` 构造 `MAC/MEDIA_RPT/FWROLE_MAINTAIN` 的4-byte payload 与
`MAC/MEDIA_RPT/JOININFO` 的12-byte payload，并校验所有位域、尾部保留 dword 清零、
RTL8852B band-0 和 WMM 范围拒绝路径。

这个 profile 不把包写入 SDIO TX FIFO，不等待 C2H done-ack，不创建 firmware role，
不配置 address/BSSID CAM，不构造伪 MAC/BSSID，也不开始 scan/association 或创建 `wlan0`。完成
标记只证明控制面的两个原始序列化器可用，不代表入网成功。

待实板时使用：

```bash
tools/build_k1.sh \
  --config board/k1/muse_pi_pro/configs/wireless_fw_runtime_control_plane_diag \
  --build-dir cmake_out/k1-wireless-fw-runtime-control-plane \
  --package --package-dir out/k1-wireless-fw-runtime-control-plane --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-fw-runtime-control-plane/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-fw-runtime-control-plane/k1-go-wrapper.bin \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-mac-core --require-runtime-control-plane \
  --require-runtime-data-tx-descriptor --require-h5 --require-bringup-success
```

验收日志必须出现 `runtime control-plane role=0x203a23 join=0x696d523` 和
`runtime control-plane serialization complete`。实板上即使它成功，也应继续停在这个边界，直到
真实的 address/BSSID CAM 和 H2C done-ack 状态机完成。

### 2026-08-23：address/BSSID CAM H2C 纯 RAM 序列化已加入，待实板验收

`wireless_fw_runtime_address_cam_diag` 继承 role-control profile，按原厂 `addr_cam.c` 的
`fill_addr_cam_info()` 和 `fill_bssid_cam_info()` 构造完整的 60-byte
`MAC/ADDR_CAM_UPDATE/ADDRCAM_INFO` payload。它保留原厂的 SMA/TMA XOR hash、地址掩码、
BSSID CAM mask、aid、TSF 和 target indicator 位字段，并把 dword 0、7、10、11
和未移植的 security/WOL 字段明确清零。

构造器接受两种明确的形状：no-link STA 使用真实单播 self MAC 和全零 target/BSSID；已关联
STA/AP 则使用非零单播 self MAC、target MAC 和 BSSID。这个 profile 仍只是 RAM 中的形状
验证：它不会写入 SDIO FIFO、不分配 CAM index、不等待 C2H done-ack、不创建 firmware role、
不开始 scan/association。因此完成标记不代表实际 CAM 写入、扫描、连接或联网。

待实板时使用：

```bash
tools/build_k1.sh \
  --config board/k1/muse_pi_pro/configs/wireless_fw_runtime_address_cam_diag \
  --build-dir cmake_out/k1-wireless-fw-runtime-address-cam \
  --package --package-dir out/k1-wireless-fw-runtime-address-cam --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-fw-runtime-address-cam/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-fw-runtime-address-cam/k1-go-wrapper.bin \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-mac-core --require-runtime-control-plane \
  --require-runtime-address-cam --require-runtime-data-tx-descriptor \
  --require-h5 --require-bringup-success
```

验收日志必须出现 `runtime address CAM dword2=0x11133f5d dword13=0x1b0a2afd` 和
`runtime address CAM serialization complete`。不得因为这个纯 RAM 结果直接尝试写 CAM；首先需实现持久的
角色、CAM allocation 与 C2H done-ack 状态机。

### 2026-08-23：no-link role/CAM H2C done-ack 补齐原厂前置步骤，待复测

`wireless_fw_runtime_role_cam_done_ack_diag` 在已验证的 firmware runtime、H2C/C2H
loopback、静态 MAC-core 和 RAM 序列化之后，使用 eFuse 读取到的真实 self MAC。上一轮实板中
`FWROLE_MAINTAIN` 的 done-ack 等待一秒仍没有任何 C2H，故不能以延长等待或修改 parser 解决。

当前实现按 GPL Realtek Linux 的真实前置顺序补充：先执行 `addr_cam_init()` 的
`R_AX_ADDR_CAM_CTRL (0xce34)` search-range、enable、clear 和 clear-bit 轮询；随后按
`role_init()` 发送 `MAC/FW_OFLD/MACID_PAUSE_SLEEP`（category/class/function `1/9/0x28`）给
MACID 0 解暂停。原厂 `set_macid_pause()` 在 FWDL 就绪后明确转到 `set_macid_pause_sleep()`，
payload 是固定 **256 bytes / 16 dwords**：pause 和 sleep 均保持未暂停，只在两组的 group-zero
mask 置 bit 0，其余清零；原厂明确不请求 done-ack。此前误用的 32-byte `MACID_PAUSE`
（`1/9/0x08`）已恢复为原厂路径。

原厂的 `role.c` 与 `addr_cam.c` 都把这两条命令标记为 `agg_en = 1`。因此 MACID 完成后，当前实现
先构造 role 和 CAM 两个内部 H2C，再按 `h2c_agg.c` 发送一个外层
`MAC/FW_OFLD/H2C_AGG`（`1/9/0x15`）包。外层 content 是两个按 4-byte 对齐的
`little-endian sub-packet length + complete inner H2C`；role 和 CAM 的 inner H2C 均使用当前的
sequence。最初的 role/CAM 实板尝试使用 sequence `2`；在下述 aggregate loopback 预检加入后，
预检占用 `2`，role/CAM inner 和 outer 使用 `3`。单包 loopback 和 MACID 分别占用 `0`、`1`。

role/CAM 回执按 `MAC/FW_INFO/DONE_ACK` 的 4-byte payload 校验原
category/class/function/sequence 和 return code。两个回执在同一个 RX aggregate 时必须一次性消费，
不能在先等 role 时丢弃 CAM；所以诊断只在两份 done-ack 都收到且均为 0 后才成功。MACID 命令仍不请求
done-ack。

### 2026-08-24：H2C aggregate loopback 预检已加入，等待 Wi-Fi 实板重新上电

从 Realtek GPL reference 的 `phl/hal_g6/mac/mac_ax/h2c_agg.c` 复核后，聚合 content 的确是每个
inner H2C 前放置一个 little-endian 的 **4-byte-aligned inner length**，随后是完整的 inner H2C
header 和 payload；外层为 `MAC/FW_OFLD/H2C_AGG` (`1/9/0x15`)，不请求 ack。现有实现已把这部分
提取为通用双命令聚合器，role/CAM 与预检共用相同的生产封包路径。

在实际 role/CAM 之前，诊断现在先聚合两份已经在本板单包验收通过的 `CMD_PATH` loopback (`0/0/0`)
inner H2C，要求从 RX FIFO 收到并校验两份 24-byte 回显 C2H。它通过才会继续提交 role/CAM；因此结果
可以明确区分「aggregate envelope 错误」和「firmware role/CAM 前置状态不完整」。序列顺序也随之固定为：
单包 loopback `0`、MACID unpause `1`、aggregate loopback `2`、role/CAM inner 和 outer `3`。聚合
inner 命令共享外层序列号，这与 `mac_h2c_agg_tx()` / `h2c_pkt_set_hdr()` 的原始行为一致。

该实现已通过 `tools/ci_k1.sh --static-only`、RISC-V ELF 检查和完整构建。包
`out/k1-wireless-fw-runtime-role-cam-agg-loopback/` 的 ELF SHA-256 是
`3c289363160e4a1a116bf83d3793b36631a5574889947b7019e91a7249c34330`。

截至本文档更新，连续两次通过 NSH `reboot` 自动 RAM-boot 的实板尝试均在 firmware download 前的
RTL8852BS2 SDIO CMD5 初始化超时（host status `0x18000`，bring-up `-110`），因此没有运行到 aggregate
loopback。日志是 `out/k1-serial/k1-wireless-fw-runtime-role-cam-agg-loopback-20260823T160506Z.log` 和
`out/k1-serial/k1-wireless-fw-runtime-role-cam-agg-loopback-20260823T160706Z.log`。这两次结果不能用于
判断 aggregate 或 role/CAM：恢复实测前必须让无线芯片经历一次真实的断电重上电或等效硬件电源复位，之后
仍使用同一 RAM-only package 复测。

该 profile 的状态完全易失：只创建一个 no-link STA role 及其零 peer/BSSID CAM，绝不设置
SSID、信道、认证、加密、peer、MACID policy 或 netdev；不会扫描、关联、发普通数据帧或获取 IP，
也不写 eMMC、eFuse、U-Boot environment 或其他持久存储。它的成功只证明该板上这两条真实
H2C 能被 firmware 接收并成功应答，不能宣称 Wi-Fi 已联网。

待实板时使用：

```bash
tools/build_k1.sh \
  --config board/k1/muse_pi_pro/configs/wireless_fw_runtime_role_cam_done_ack_diag \
  --build-dir cmake_out/k1-wireless-fw-runtime-role-cam-done-ack \
  --package --package-dir out/k1-wireless-fw-runtime-role-cam-done-ack --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-fw-runtime-role-cam-done-ack/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-fw-runtime-role-cam-done-ack/k1-go-wrapper.bin \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-mac-core --require-runtime-control-plane \
  --require-runtime-address-cam --require-runtime-role-cam-done-ack \
  --require-h5 --require-bringup-success
```

成功日志必须包含 `runtime role/CAM done ack` 与
`RTL8852BS2 runtime role/CAM done-ack complete`。任何 timeout、RX descriptor/C2H 格式错误
或非零 firmware return 都应停止在此处，不应继续进入 scan、association 或 data FIFO。

### 2026-08-24：定位 role/CAM 前 H2C 页资源耗尽，已按原厂顺序修复

使用上述 role/CAM profile 的实板日志
`out/k1-serial/k1-wireless-fw-runtime-role-cam-done-ack-20260823T180832Z.log`
确认：SDIO、完整固件下载、WCPU runtime、H2C loopback、MAC core、control-plane 和 address CAM
均已通过。失败发生在第一条 `MACID_PAUSE_SLEEP` H2C 之前，原始错误为 `-ENOSPC`。

新增页数诊断得到确定证据：runtime MAC function enable 前 `H2C-pages=0x20`，enable 后
`runtime role preflight H2C-pages=0x0`；该 H2C 至少需要 `0x6` 页，随后 5 次轮询均保持
`required=0x6 available=0x0`。因此这不是 CMD53 超时、role payload 或 done-ack parser 问题，
而是全功能 enable 后没有恢复原厂的 runtime TX 资源初始化。

对照 GPL Realtek `mac_trx_init()`，当前 role/CAM 路径已在完整 DMAC/CMAC enable 后、address CAM
之前重新执行：

1. `dle_init()` 对应的 K1 DLE/SCC 初始化；
2. `hfc_init(adapter, 1, 1, 1)` 对应的 K1 HCI flow-control 初始化；
3. 重新读取 TXPG_WP，确认 H2C 页资源后再发送 role/CAM H2C。

该修复已通过 `tools/ci_k1.sh --static-only`、完整构建和 ELF 检查。最新 RAM-only 包的 ELF
SHA-256 为 `712d17170357a4246cb3dba0f0e0c5c3aa14afb99457a1f2757fb9fdae6e2f16`。
下一次实板复测必须使用真实 RST 重新启动并加载该包；2026-08-24 末次测试因未发生 RST，
没有产生新的硬件结果。

### 2026-08-24：完整 Wi-Fi runtime 与 raw HCI active 实板通过

有效 RAM-only 记录为
`out/k1-serial/k1-wireless-fw-runtime-role-cam-h5-active-20260823T202445Z.log`。该轮先让
厂商 Linux 正常启动并恢复板载无线模块，再由 ADB reboot 截停 U-Boot，只以 `loadx + go`
加载 54-byte wrapper 和 545424-byte payload 到 RAM；没有调用 `saveenv`、FDL、fastboot，
也没有写 eMMC、SPI 或 eFuse。

日志确认完整 Wi-Fi firmware download/WCPU runtime、runtime transport、H2C/C2H loopback、
MAC-core、role-control 和 address CAM 均通过。aggregate loopback 使用 sequence 2；直接
`FWROLE_MAINTAIN` 和 CAM H2C 分别使用 sequence 3 和 4，两个 firmware done-ack 的 return
均为 0，最终出现 `RTL8852BS2 runtime role/CAM done-ack complete`。这证明 H2C 页资源恢复、
no-link role 和 address/BSSID CAM 的 firmware 应答已在实板跑通，但不代表 `wlan0`、扫描、关联
或 IP networking 已实现。

同轮已注册 raw `/dev/ttyHCI0`。首次打开记录
`out/k1-serial/k1-wireless-fw-runtime-role-cam-h5-active-hci-open-20260823T202445Z.log` 包含
`K1 Bluetooth: H5 transport opened`，但正式 `wireless` profile 当时未启用
`CONFIG_DEBUG_WIRELESS_INFO`。NuttX `bt_slip` 的 `h5: active` 是该日志级别的唯一可观察标记，
因此该轮不能区分「已协商但未打印」与「协商未完成」，不能将 raw HCI active 标为通过。

为使 smoke 判据可观测，`wireless/defconfig` 仅增加了 `CONFIG_DEBUG_WIRELESS=y` 和
`CONFIG_DEBUG_WIRELESS_INFO=y`。最终使用的完整 RAM-only 包是
`out/k1-wireless-fw-runtime-role-cam-h5-active-logging-full/`，而不是先前仅含 235152-byte
payload 的简化 logging 包。该包的 ELF SHA-256 为
`075a954f8056fd25658ec51e54689843aac8ad3de036d0f5e19a28a107aed9ae`，flat payload SHA-256 为
`1437a1fccc2d350c55f6bca7cffb248fe8019752080015344798b21ac54d4bb5`。

最终端到端记录为
`out/k1-serial/k1-wireless-fw-runtime-role-cam-h5-active-logging-full-20260823T204625Z.log`。
该轮自动通过 ADB reboot 截停 U-Boot，仅以 `loadx + go` 将 54-byte wrapper 和 545424-byte
payload 放入 RAM。日志先后确认完整 FWDL/WCPU runtime、runtime transport、H2C/C2H loopback、
MAC core、control plane、address CAM 和 role/CAM done-ack；随后 `cat /dev/ttyHCI0` 触发
`K1 Bluetooth: H5 transport opened`，NuttX `bt_slip` 完成 CONFIG 协商并打印 `h5: active`。

烟测工具的 `--require-bt-hci-device` 判定已同时接受旧的
`H5 stack registered /dev/ttyHCI0` 和当前 raw-HCI profile 的
`H5 raw HCI registered /dev/ttyHCI0`，避免将有效的 raw HCI 注册误判为缺失。该修改后的
`tools/ci_k1.sh --static-only` 已通过。此记录仅证明 no-link Wi-Fi firmware/control plane
和 Bluetooth H5 传输层；仍不代表 Wi-Fi `wlan0`、扫描、关联、IP networking 或 Bluetooth host
stack/扫描已经实现。

### 2026-08-24：NIC firmware 的 scan-offload channel-list 位域修正仍返回 `0x04`

此前 `wireless_fw_runtime_scanofld_channel_diag` 已成功收到
`MAC/FW_INFO/DONE_ACK`，但 firmware return 为 `0x04`，所以不能把 channel list 标为已接受。
重新核对同一 SpacemiT Linux revision `31c449ae` 的 `phl_scanofld.c`、`hal_api_mac.c`、
`mac_def.h` 与 `mac_ax/fwofld.c` 后，发现先前的 28-byte 条目只设置了 period/central/primary
channel，错误地把原厂无 link 扫描的 dword1 控制字段清零。

修复后的条目保持被动且不请求 probe、null、data 或 additional frame，但按原厂序列化
`c2h_notify_enterCH=1`（dword1 bit 6）和 `pause_tx_data=1`（dword1 bit 13），因此 dword1
为 little-endian `0x00002040`。这两个 bit 只描述未来 scan 期间的 C2H/data-TX 行为；本 profile
仍然只提交 `ADD_SCANOFLD_CH`，不会发送 `SCANOFLD` start、切换 RF、扫描、关联、注册 `wlan0`
或写入任何持久介质。

修复版已通过 `tools/ci_k1.sh --static-only`、完整构建和 ELF 检查。RAM-only package 位于
`out/k1-wireless-fw-runtime-scanofld-channel-fixed/`，ELF SHA-256 为
`d89017cd1d59b996ecd7d23a9ab3718f4ab3875268695c8509b2f4b0b95a6a1b`。

首次从 NuttX `nsh>` 执行自动 `reboot` 后，Wi-Fi SDIO 重新初始化在 CMD5 超时；这是该板当前
软重启路径的已知限制，不能用来判断 scan-offload。改为先打开 UART 监听、再人工按 RST 后，
工具在 U-Boot 截停 autoboot，并仅通过 `loadx + go` 传入 wrapper 与 payload。完整实板记录为
`out/k1-serial/k1-wireless-fw-runtime-scanofld-channel-fixed-20260823T220002Z.log`。

该记录确认 SDIO、完整 FWDL/WCPU runtime、H2C/C2H、MAC core 以及 no-link role/CAM 均正常；
`ADD_SCANOFLD_CH` 也被 firmware 接收并产生 matching `MAC/FW_INFO/DONE_ACK`。但 done-ack
仍明确返回 `firmware-return=0x04`：

```text
K1 Wi-Fi GPL: passive scan channel-list H2C queued channel=1 sequence=5
K1 Wi-Fi GPL: passive scan channel-list done-ack error=0x5 firmware-return=0x4
```

因此 `0x00002040` 的位域修正不是根因；不能将 channel list、`SCANOFLD` start、RF channel
switch、扫描、关联、`wlan0` 或联网标记为完成。下一步是按原厂 NIC bring-up 顺序补足
scan-offload 之前的 MAC/PHY/RF 前置状态，并且每次只验证一个明确差异，继续保持 RAM-only。

### 2026-08-24：scan-offload 诊断切换到支持该能力的 U2 NICCE firmware

以上 `0x04` 记录使用的是 `array_8852b_u2_nic`。随后核对同一 SpacemiT GPL source revision
`31c449ae` 的 `hal8852b_fw_cap.h`：U2 `MAC_FW_CATEGORY_NIC` 不定义
`FW_CONFIG_SCAN_OFFLOAD`，而 `MAC_FW_CATEGORY_NICCE` 明确定义它。因此不能继续以 NIC 映像的
`ADD_SCANOFLD_CH` 回执推断 pinmux、MAC role/CAM 或 RF 配置问题。

当前实现将 active image 切为同 revision `array_8852b_u2_nicce`，由
`tools/extract_rtl8852bs_u2_nic_fw.sh` 机械生成
`chip/k1/k1_rtl8852bs_u2_nicce_fw.inc`。其 SHA-256 是
`64e6f0c5744f10be980d2031b5001f83d02074598b0b92dbafaaa9febe0834d0`，长度 341216 bytes。
实际解析确认下载格式兼容当前 loader：`0xa0` static+dynamic header，section 0/1/security
分别为 `0x4f108`、`0x3738`、`0x800`，security `MSSC=2`，并有 1024-byte legacy trailer。
loader 不再保留旧 NIC header、section 或 MSS 副本，所有 FWDL packet 都直接从这一份 NICCE
image 派生，所选 signature 只在当前易失 TX packet 内覆盖。

NICCE profile 随后已在实体板以 RAM-only 方式通过：完整记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-scanofld-nicce-20260823T223754Z.log`。
它确认完整 FWDL ready（`status=0x7`）、H2C/C2H loopback、role/CAM done-ack，以及
`RTL8852BS2 passive scan channel-list done-ack complete`。因此此前的 `0x04` 是普通 NIC
firmware 没有 scan-offload capability，不是 SDIO、pinmux、role/CAM 或该 channel-list 的失败。

下一步 profile `wireless_fw_runtime_scanofld_passive_diag` 才会向已接受的 table 发送一次
`SCANOFLD` start，并在同一个 RX 循环中要求 start 的 done-ack、channel-1 enter C2H 与 scan-end
C2H。它保持 100 ms 被动扫描，不发 probe、不解析 beacon、不注册 `wlan0`、不关联也不联网。此
profile 的结果必须以新的 RAM-only 实板记录为准；在完成标记出现前，不能宣称 RF scan、`wlan0`、
关联或联网完成。

### 2026-08-24：NICCE passive scan-offload 已完成单信道闭环

`wireless_fw_runtime_scanofld_passive_diag` 已在 MUSE Pi Pro 实体板上通过
U-Boot `loadx + go` 作 RAM-only 验证。此次包位于
`/home/sw/Dev/k1-workspace/out/k1-wireless-fw-runtime-scanofld-passive/`，ELF
SHA-256 为
`8c22036819cc0f164f9d7821910e7de893616eb335bdcada8750d432a4f66899`；完整串口记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-scanofld-passive-20260823T231059Z.log`。
整个过程只停止 U-Boot autoboot、XMODEM 加载 wrapper/payload 并执行 `go`，没有写 eMMC、SPI
flash、eFuse、U-Boot environment 或任何其它持久介质。

该记录依次确认 NICCE full FWDL ready (`status=0x7`)、runtime transport、H2C/C2H loopback、
MAC core、volatile no-link role/CAM 及 `ADD_SCANOFLD_CH` done-ack。随后 scan-offload
start done-ack 返回 `0`；firmware 的 channel-1 enter C2H 由同一 RX 循环接收，100 ms 被动
dwell 后 host 提交 `SCANOFLD_DRV_CTRL/NEXT_CH`，最后接收到 scan-end C2H。驱动仅在四个条件
均成立时打印完成标记，记录中的关键行是：

```text
K1 Wi-Fi GPL: passive scan-offload H2C queued channel=1 sequence=6
K1 Wi-Fi GPL: passive scan-offload next-channel H2C queued channel=1 sequence=7
K1 Wi-Fi GPL: passive scan-offload done-ack return=0x0000000000000000
K1 Wi-Fi GPL: RTL8852BS2 passive scan-offload complete
```

因此当前已证明 firmware-offloaded 的单次、被动 channel-1 scan 状态机可运行并能由 host
推进到结束。它不发送 probe request、不解析 beacon、不创建 `wlan0`、不关联 AP，也不配置 IP；
不能据此宣称普通 Wi-Fi 扫描结果、联网或量产 Wi-Fi 驱动已经完成。

同一包已随后以最小 smoke 条件
`--require-runtime-scanofld-passive --require-bringup-success` 再次通过，记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-scanofld-passive-20260824T050142Z.log`。
该工具同时确认上述 scan-offload 完成标记和 NSH，并最终报告
`PASS: K1 wireless RAM image reached NSH`。

### 2026-08-25：增加真实 Beacon/BSS RX 验收路径

此前 scan-offload RX 循环只处理 packet type 10 的 C2H，将 packet type 0 的普通
802.11 RX 帧直接跳过，所以 `scan-end` 只能证明固件状态机结束，不能证明空口收到了
Beacon 或 Probe Response。本轮按原厂 `rtw_recv.c`、`rxdesc.h` 和 `fwcmd.c` 的格式补齐
了最小解析路径：

- 读取 packet type 0 的 RX payload，识别 management type、Beacon subtype 8 和
  Probe Response subtype 5；
- 按 24-byte management header、12-byte Beacon 固定字段和 IE 列表读取一个 BSSID、
  SSID 长度与 DS channel IE；
- 解析 scan-offload C2H reason 6 的 per-channel report，记录 report 中的
  Beacon/Probe Response 计数；
- 只保留一个 bounded BSS 结果，不把 RX buffer 生命周期暴露给上层，也不注册
  `wlan0`。

新增 `wireless_fw_runtime_scanofld_rx_diag` profile 会在 scan-end、done-ack 等原有条件
之外要求至少一个有效 BSSID；成功日志会输出 `rx-data`、`mgmt`、`beacon`、`probe-rsp`、
`scan-report`、`report-rx`、`bssid`、`ssid-len` 和 `channel`。该 profile 仍然是
U-Boot `loadx + go` 的 RAM-only 诊断，只被动接收，不发 Probe、不关联、不做 WPA/DHCP，
因此即使该验收通过，也只表示真实 BSS RX 已打通，尚未表示 `wlan0`、关联或联网完成。

### 2026-08-25：补齐原厂 scan RX-filter 前置条件，待实板验收

此前 `wireless_fw_runtime_scanofld_rx_diag` 的 firmware scan 状态机可完整运行，但
`packet type 0`、Beacon、Probe Response 和 scan report 的 RX 计数均为零。回查同一
SpacemiT Linux revision `31c449ae` 的 `phl_scanofld.c`、`hal_rx.c`、`hal_api_mac.c`、
`mac_ax/rx_filter.c` 与 `mac_reg_ax.h` 后确认，原厂在 off-channel scan 开始前调用
`rtw_hal_scan_set_rxfltr_by_mode(..., true, ...)`，而不是只提交 scan-offload H2C。

本项目现在在一次性 scan 诊断开始前读取并记录 `R_AX_RCR (0xce00)`、
`R_AX_PLCP_HDR_FLTR (0xce04)`、`R_AX_RX_FLTR_OPT (0xce20)` 和
`R_AX_MGNT_FLTR (0xce28)`。只改动后两个寄存器：保留无关 bit，将扫描模式的
`A1/broadcast/multicast accept=1`、`unicast/broadcast CAM match=0`、`beacon check=0`
写入 `0xce20`，并按原厂 `RX_FLTR_FRAME_TO_HOST` 将 `0xce28` 写为 `0x55555555`，确保
management subtype 能进入 SDIO host RX FIFO。无论 scan-end、H2C 错误、超时还是 BSS 缺失，
代码都会读回并恢复两项原值；`0xce00/0xce04` 仅为日志快照，不写入。

该实现同时记录每个 RX aggregate entry 的原始 `RXD0/RXD3`、packet type、payload length、
CRC/ICV 标志，以及 scan report 的原始 dword3 与 content bytes。主机静态门禁、完整
`wireless_fw_runtime_scanofld_rx_diag` 构建和 ELF 检查都已通过；RAM-only 包在
`out/k1-wireless-fw-runtime-scanofld-rx-filter/`，ELF SHA-256 为
`2cd22a48f54a26c9fcfee1925f6882d8e1adec6094f30290a0b9032e3324e91b`。

该包尚未在实体板验证。因此当前结论仍是：firmware 的单信道被动 scan 状态机已通过，
但真实 Beacon/BSS RX、`wlan0`、关联、WPA、DHCP 和联网均未完成。下一次 RAM-only 验收
必须先确认 `scan RX filter before`、`scan`、`after` 三行寄存器日志；只有出现
`passive scan RX ... beacon` 或有效 `bssid` 后，才能把真实空口接收标为完成。

### 2026-08-25：将被动空口 RX 诊断扩展到 2.4 GHz 的 1/6/11

单信道 channel 1 的 scan-offload 控制链已经在实体板完成，但即使应用了原厂 scan RX
filter，SDIO RX FIFO 中仍未出现 packet type 0 的空口帧。单个 100 ms 信道 1 驻留不足以
区分 RX 路径错误与 AP 位于 channel 6 或 11 的普通情况。

原厂 RTL8852BS `mac_ax_scanofld_chinfo` 的 `period` 是一个 8-bit 毫秒字段，因此不能在一条
记录里表达 300-500 ms。本项目改为使用原厂同样的 28-byte 记录一次提交 channels 1、6、11，
每条记录的 period 为 250 ms。每收到一条 band-0 的 enter-channel C2H，主机等待 250 ms 并
提交携带该 channel 的 `SCANOFLD_DRV_CTRL/NEXT_CH`；只有三个 channel 的 enter 和 NEXT_CH
都完成且收到 scan-end，扫描控制链才成功。运行日志会逐条记录 C2H channel/reason/status、
firmware per-channel RX report 以及每个 SDIO RX descriptor。

该改动仍完全 RAM-only：不发送 Probe Request，不创建 `wlan0`，不关联、认证、DHCP 或写入
任何持久化存储。下一次实板使用 `wireless_fw_runtime_scanofld_rx_diag` 进行验证；只有出现
有效 Beacon/Probe Response BSSID 才能通过 `--require-runtime-scanofld-rx`。

### 2026-08-25：1/6/11 被动扫描控制链实板通过，空口 RX 仍为零

使用 `wireless_fw_runtime_scanofld_rx_diag` 的新 RAM-only package 在 MUSE Pi Pro 实板完成
验证。构建产物位于 `out/k1-wireless-fw-runtime-scanofld-rx-24611/`，ELF SHA-256 为
`63f0cdc7807d050695f0c09c85e87cec839bc8d41d4805339383cb75647d4d8d`；完整串口记录为
`out/k1-serial/k1-wireless-fw-runtime-scanofld-rx-24611-20260825T060458Z.log`。本次仅在
U-Boot 中停止 watchdog、以 `loadx + go` 加载 wrapper 和 payload，未写 eMMC、SPI flash、
eFuse 或 U-Boot environment。

Firmware 接受 88-byte 的三条 channel-info table（done-ack return 0），并依次报告 channel
1、6、11 的 band-0 enter C2H。主机在每次 enter 后完成对应的 250 ms 驻留和 `NEXT_CH`；最终
scan-end 到达，`enter-mask=0x7`、`next=0x7`，证明三信道 scan-offload 控制状态机完整运行：

```text
passive scan channel-list H2C queued channels=1,6,11 ...
passive scan C2H channel=1 reason=3 status=1 band=0
passive scan-offload next-channel H2C queued channel=1 ...
passive scan C2H channel=6 reason=3 status=1 band=0
passive scan-offload next-channel H2C queued channel=6 ...
passive scan C2H channel=11 reason=3 status=1 band=0
passive scan-offload next-channel H2C queued channel=11 ...
passive scan C2H channel=11 reason=5 status=1 band=0
```

scan RX filter 也在 scan 前后准确切换并恢复（`ce20 f0170001 -> f017000f -> f0170001`）。不过每一
个 SDIO RX aggregate 都是 packet type 10 的 C2H，packet type 0、Beacon、Probe Response、BSS
和 firmware scan-report RX count 都为零；因此 BSS RX gate 按设计返回 `-ENODATA`（日志中的
`error=0x3d`），随后 NSH 正常启动。结论是信道选择、H2C/C2H、RX filter 以及 RX FIFO 的
firmware-event 路径都已验证，但普通 802.11 空口 RX、`wlan0`、关联和联网仍未完成。下一步需要
对照原厂 Linux 的完整 BB/RF 初始化、校准与普通 RX 交付路径，而不是继续调整 scan-offload table。

### 2026-08-25：BB/RF release 实板通过，普通空口 RX 仍为零

原厂 `hal_start_8852b()` 在 MAC runtime 初始化之后、完整 BB/RF parameter image 和
calibration 之前，调用 `set_enable_bb_rf(..., 1)`。此前 K1 诊断只完成了 HCI/DMAC、DLE、
firmware、MAC-core 和 scan-offload，未明确执行这段释放 BB reset/AFE 的前置状态；这可能解释
firmware C2H 能回传而普通 802.11 RX 始终为零。

新增 `wireless_fw_runtime_scanofld_bb_rf_diag` 只复现该有界步骤：设置
`SYS_FUNC_EN[1:0]`、`SPS_DIG_ON_CTRL0[18:17]=1`，对 `WLRF_CTRL[17]` 执行原厂 `1 -> 0 -> 1`
edge，写入并读回 XTAL SI `0x80/0x81=0xc7`，最后设置 `PHYREG_SET=0x0e`。不导入大规模
BB/RF 表，不执行 calibration，也不写持久介质。

该 profile 已在 MUSE Pi Pro 以 U-Boot `loadx + go` 的 RAM-only 方式实测通过。构建包位于
`out/k1-wireless-fw-runtime-scanofld-bb-rf/`，ELF SHA-256 为
`1720aef4c88c6c5bbaf28522c50c7dda4b8a1d52c2c284531651c3e93fd9a774`，完整串口记录为
`out/k1-serial/k1-wireless-fw-runtime-scanofld-bb-rf-20260825T062029Z.log`。关键读回为：

```text
BB/RF release SYS_FUNC=0x03 WLRF=0x820000 XTAL=0xc7,0xc7 PHYREG=0x0e
RTL8852BS2 BB/RF release complete
```

随后 firmware-offloaded 的 channels 1/6/11 被动 scan 仍正常完成，但 packet type 0、Beacon、
Probe Response、BSS 和 firmware scan-report RX count 均仍为零。因此 BB reset/AFE release
不是普通 RX 缺失的唯一根因；下一步需要加载原厂 `halbb_init_reg()` 的 BB 参数镜像，并在其
独立实板验收后再决定是否需要 RF 参数镜像或 calibration。此结果不改变 `wlan0`、关联、WPA、
DHCP 和联网未完成的状态。

`wireless_fw_runtime_scanofld_bb_rf_rx_diag` 在此基础上再启用既有 Beacon/BSS gate；因此
BB/RF release 的成功与普通空口 RX 的成功可以分别判定。

### 2026-08-25：默认 BB PHY CR 参数镜像待实体板验证

原厂 `hal_start_8852b()` 在 MAC runtime 初始化和 BB/RF release 后，进入
`halbb_init_reg()`；RTL8852B 的默认分支由
`halbb_cfg_bbcr_ax_8852b()` 原样顺序写入
`array_mp_8852b_phy_reg`。该表有 1018 组地址/32-bit 数据，不含条件分支、控制指令或延迟项。

本仓将该表机械导入隔离的 GPL-2.0-only
`chip/k1/k1_rtl8852bs_phy_reg_8852b.inc`，并通过已经实板验证的 Function 1 间接 CMD53
32-bit 写入路径依原顺序提交。profile
`wireless_fw_runtime_scanofld_phy_cr_diag` 在 BB/RF release 后执行这一步，并读回最终
`0x0704=0x601e0502`、`0x49c0=0x800cd62d`、`0x0c14=0x85010000`、
`0xc1f8=0x00000001`、`0x1210=0xc0000c06` 五个哨兵。它不引入 RF 参数镜像、校准、
`wlan0`、关联、WPA、DHCP 或任何持久化写入。

该 profile 尚未做实体板验收。通过构建、ELF 检查后，只以 U-Boot
`loadx + go` 临时运行并使用
`--require-runtime-phy-cr --require-runtime-scanofld-passive` 验收；成功后再以
`wireless_fw_runtime_scanofld_phy_cr_rx_diag` 重新检查 Beacon/BSS RX。

### 2026-08-25：PHY CR 的 CRC 后 retune 重放，待实体板验证

完整 PHY CR 表的首次实板运行已越过 SDIO 枚举、full FWDL、WCPU runtime、MAC-core 和
BB/RF release，但在第 `0x32d` 项的 `0x0024` 写入收到 SDHCI
`INT_STATUS=0x00208003`。其中 `0x00200000` 是 Data CRC Error，因此
`k1_sdio_wifi_cmd53()` 返回 `-EILSEQ`，并已经按 K1 Linux SDHCI 的 request-error 顺序完成
command reset 后 data reset。

Linux 的 MMC core 对 CRC 错误标记 retune-needed，SDHCI 同样清除 command/data 状态；但
`mmc_io_rw_extended()` 不为 CMD53 设置通用重试次数。首次实板运行确认，仅做 SDHCI reset 后
等待 10 us 会让下一条 CMD53 在命令阶段超时，因此不能把短延迟当作恢复动作。

当前策略只用于 `array_mp_8852b_phy_reg` 的普通寄存器配置项及最终只读哨兵：在 `-EILSEQ` 后，
确认 Function 1 仍被选择且 I/O-ready，再重新运行既有 SDR104 CMD19 调谐扫描，最后重放同一个
操作一次。它不用于 H2C、固件、FIFO 或 write-one-to-clear 事务，因为它们可能已经被设备接收。
retune 或重放失败会立刻终止；成功也只证明该次链路恢复，不代表扫描、关联或联网完成。

### 2026-08-25：CRC 后原地 retune 与 High-Speed 回退均未恢复

`wireless_fw_runtime_scanofld_phy_cr_diag` 的两次 RAM-only 复验均完成
FWDL、WCPU runtime、H2C/C2H、MAC core 和 BB/RF release，随后稳定在 PHY CR
第 `0x32d` 项、地址 `0x24` 的 Data CRC Error (`INT_STATUS=0x00208003`)。

第一轮记录
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-scanofld-phy-cr-retune-sequential-reset-20260825T090750Z.log`
确认 K1 Linux 的顺序 command reset 后 data reset、以及 DAT0-ready 检查均实际执行；但
随后 CMD19 仍在数据阶段收到 `0x00018000`，无法完成 SDR104 调谐。

第二轮记录
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-scanofld-phy-cr-fallback-highspeed-20260825T091428Z.log`
确认降速写入已生效：`CCCR_SPEED=0x03`、`HOST2=0x4008`、`CLOCK=0xe0407`，且
K1 TX internal-clock bit 已置位。对同一条幂等 PHY CR 写的重放仍立即以
`INT_STATUS=0x00018000` 失败，因此不能把一次已运行 WCPU 的会话内降速误判为原厂
Linux 的完整恢复。

原厂 `sdhci-of-k1x.c` 在 CRC 后仅标记 `MMC_CAP2_QUIRK_BREAK_SDR104`；下一次
`init_card` 才撤销 SDR104 能力并重新走卡初始化/驱动加载。当前新增的
`wireless_fw_runtime_scanofld_phy_cr_highspeed_diag` 因而从枚举开始全程保持
High-Speed、跳过 SDR104/CMD19，用于验证这个完整边界。它仍不创建 MAC/netdev、扫描、关联或联网。

### 2026-08-25：`--nsh-reboot` 自动执行两阶段非持久恢复

实板确认，从 NuttX 直接 `reboot` 后的第一轮 RAM-only Wi-Fi 诊断可能在最早的 CMD5 超时；
这发生在 PHY CR 之前，不能当作 SDIO retune、firmware 或 PHY 表的结果。原因是直接从临时
NuttX 运行态重启时，RTL8852BS2 的 SDIO 电源状态没有回到已验证的冷启动状态。

`tools/run_k1_wireless_smoke.py --nsh-reboot` 因而固定执行：NuttX `reboot`、首次截停
U-Boot、U-Boot `reset`、再次截停 U-Boot、再 `loadx + go`。这两次 reset 都不执行
`saveenv`，加载仍只写 RAM；工具会在串口日志中打印每个阶段。这样后续 Wi-Fi 结果才可归因于
当前镜像，而不是软重启残留状态。

### 2026-08-25：完整 High-Speed 初始化实板边界

`wireless_fw_runtime_scanofld_phy_cr_highspeed_diag` 从 SDIO 枚举起保持
High-Speed（约 46.9 MHz），不选择 SDR104、也不执行 CMD19。通过
`--nsh-reboot` 的两阶段 U-Boot reset 后，RAM-only 记录为
`/home/sw/Dev/k1-workspace/out/k1-serial/k1-wireless-fw-runtime-scanofld-phy-cr-highspeed-clean-20260825T092016Z.log`。
它确认 `CCCR speed=0x03`、`UHS=0`、`SDR104=0`，并成功完成 eFuse MAC 读取、完整 WLAN
firmware download（`full FWDL ready status=7`）、WCPU runtime、MAC-core 及 BB/RF release。

PHY CR 表仍稳定在第 `0x32d` 项、地址 `0x0024` 失败。第一次 12-byte Function 1 间接 CMD53
写完成命令和数据阶段后，SDHCI 报告 `INT_STATUS=0x00208003`，其中
`0x00200000` 为 Data CRC Error；同一会话内 command reset、data reset 和 High-Speed 重放后，
下一笔则以 `0x00018000` 超时。因此，降低时钟可排除 SDR104/CMD19 调谐作为此边界的直接原因，
但不能恢复已出错的会话。

已逐行核对 SpacemiT Linux `31c449ae` 的
`halbb_cfg_bb_phy_8852b() -> halbb_set_reg() -> hal_write32()` 与
`_sdio_8852b.c:w_indir_cmd53_sdio_8852b()`：原厂同样使用 Function 1 `0x1040`、
12-byte 递增 CMD53 写入，再用 CMD52 轮询 `0x1043` 的 ready bit；当前报文布局和完成轮询
与其一致。下一项应在原厂 Linux 完整启动后的模块状态下重放同一 RAM-only profile，区分该
初始化状态依赖与 K1 SDIO 长序列写可靠性。当前主机未枚举 ADB，不能自动从该 Linux 状态抢回
U-Boot；这不是 PHY CR、firmware 或 Wi-Fi 功能成功证据。

### 2026-08-25：PHY CR firmware I/O-offload 诊断待实体板验证

对照同一原厂 revision 的 `halbb_fw_set_reg()`、`rtw_hal_mac_add_cmd_ofld()` 与
`mac_ax/fwofld.c` 后，确认 firmware I/O-offload 的 BB 写入不是另一种间接 CMD53 格式。
每项是 16-byte `fwcmd_cmd_ofld`：source=BB、type=write、完整 32-bit mask、表地址和数值；
同一 H2C 的最后一项设置 `LC`。运行时 H2C 使用 `MAC/FW_OFLD/CMD_OFLD_PKT` (`1/1/0x13`)，
firmware 通过 `MAC/FW_OFLD/CMD_OFLD_RSP` (`1/1/0x08`) 返回结果、失败项编号、地址、期望值和
实际值。

新增 `wireless_fw_runtime_scanofld_phy_cr_fwofld_diag` 仅能编码已导入的 1018 条 PHY CR
常量表，每批 16 条并等待匹配 C2H 后才继续。它不开放任意寄存器
命令，不发送 scan/association/data，不创建 `wlan0`，且保持 U-Boot `loadx + go` RAM-only
流程。该 profile 尚未在实体板验收；任何 CMD_OFLD response timeout、非零 result 或哨兵不符都
不能作为 PHY CR、空口 RX 或联网成功的证据。

### 2026-08-25：PHY CR firmware I/O-offload 首次实板结果

修正 CMD_OFLD C2H 解析后，`wireless_fw_runtime_scanofld_phy_cr_fwofld_diag` 已在 MUSE Pi Pro
完成 1018 项表的 64 批提交；最后一批为 `index=1008`、`entries=10`、`sequence=0x7f`，每批
C2H 均为 `result=0`。此前固定要求 C2H 必须为 16 bytes 的解析错误已被排除；原厂
`c2h_cmd_ofld_rsp_hdl()` 只读取成功响应的第一个 dword，失败时才读取 offset/expected/read
附加字段。

首次运行随后在宿主间接 CMD53 读回 `0x0704` 时得到 `0xdeadbeef`。这不是 CMD_OFLD 失败：原厂
`halbb_fw_set_reg()` 的完成合同是 `rtw_hal_mac_add_cmd_ofld()` 返回成功和 C2H `result=0`，并不在
WCPU runtime 启动后通过宿主间接窗口读回 PHY CR。现将 profile 的验收条件调整为全部批次的
CMD_OFLD C2H 成功，不再把该状态下的宿主读回作为硬性条件。该结论仍只证明 PHY CR firmware
offload 已执行，不代表 BB/RF 校准、普通 802.11 RX、`wlan0`、关联或联网已完成。

### 2026-08-29：板级 RF 上下文与 RF radio A/B 参数镜像（待实体板验收）

`hal_start_8852b()` 的顺序是 `enable_bb_rf` → `bb_early_init` → `init_bb_reg` →
`init_rf_reg` → `bb_dm_init` → `rf_dm_init`。已完成的 PHY CR firmware offload 对应
`init_bb_reg`；本节新增的是紧随其后的 `init_rf_reg`，即
`halrf_config_rf_parameter() -> halrf_config_radio()`。已按原厂确认 8852B 的顺序是
radio A 先、radio B 后。

`init_rf_reg` **并未整体完成**。`halrf_config_rf_parameter()` 在 `halrf_config_radio()`
之后还有 power-by-rate、power limit、power limit RU、power track、xtal track 五张表，
本节都还没有实现；NCTL/RFK 同样不在范围内。这几张表决定 TX 功率与温补，对被动扫描的
纯接收路径不是前置条件，但在任何发射（Probe Request、关联、数据）之前必须补上。
因此本节的结论只能是"radio A/B 参数镜像已下发"，不能读作 `init_rf_reg` 或 RF 校准完成。

原厂 RF 表 `array_mp_8852b_radio{a,b}[]` 是**带条件的包**：开头是一组以
`{RFE type, chip CV}` 为键的 headline，之后是 IF / ELSE IF / CHK / ELSE / END
指令与寄存器对交错的表体。opcode 为 `word >> 28`（`0xf` headline、`0x8` IF、
`0x9` ELSE IF、`0xa` ELSE、`0xb` END、`0x4` CHK），条件为
`word & 0x0fffffff`，编码 `(rfe << 16) | cv`，`0xff` 表示 don't care。原厂只把被选中的
分支写进芯片，因此整张条件表不能原样进入固件。

#### 一、板级 RF 上下文：`wireless_fw_runtime_rf_context_diag`

`CONFIG_K1_RTL8852BS2_RF_CONTEXT_DIAGNOSTIC` 在**固件下载之前**读取并打印板子的真实
RF 上下文，不再假定 RFE 或 CV：

- chip CV 取自 `R_AX_SYS_CFG1 (0x00f0)` 的 bit 15:12，通过宿主间接窗口读 `0x00f1`
  一个字节后右移 4 位，对应 `enum rtw_cv`（`CAV=0`、`CBV=1`、…）。
- RFE type 取自逻辑 eFuse `0x2ca`。与原厂 `hal_rfe_type_chk()` 一致：读到 `0xff`
  表示该字节未烧写，此时使用 halrf 默认值 `0x01` 并在日志里标记
  `rfe_source=halrf-default`。
- 同时打印 BOARD_OPTION `0x2c1`、CHAN_PLAN `0x2b8`、XTAL `0x2b9`、
  THERMAL_A/B `0x2d0`/`0x2d1`，以及 TSSI-DE `0x210..0x259` 和 RX gain-K
  `0x2d4..0x2dd` 里已烧写（非 `0xff`）的字节数，用
  `calibration=absent(table defaults, uncalibrated TSSI and RX gain)` 或
  `present` 说明该板是否写过 RF 校准数据。

它必须跑在固件下载前：eFuse 读取会驱动 power-cut/isolation 寄存器，而 WCPU 接管芯片后
宿主间接窗口不再返回这些实时值。结果缓存在一个静态结构里，供后面的 RF 阶段复用。
`k1_rtl8852bs_read_wlan_efuse()` 自己完成 eFuse 上电与断电，因此它与既有的 eFuse MAC
读取各调用一次是安全的。

#### 二、GPL-only 主机端解析器：`tools/k1_rtl8852bs_rf_table_gen.py`

该工具逐行复现原厂选择逻辑，而不是盲写原始表：`halrf_sel_headline_8852b()`
的五个有序 case、以及 `halrf_config_8852b_radio_{a,b}_reg()` 的
IF/ELSE IF/CHK/ELSE/END 遍历。原厂头文件不随本项目分发，用 `--source` 指向本地副本：

```bash
python3 tools/k1_rtl8852bs_rf_table_gen.py \
  --source /path/to/halrf_hwimg_raw_data_8852b.h \
  --rfe 0x01 --cv 1 --emit-dir chip/k1
```

`--survey` 可列出每条 headline 各自选中的镜像与其 sha256，用于确认哪些
`{RFE, CV}` 组合共享同一份分支。`--emit-dir` 生成三个 GPL-2.0-only include：

原厂头文件用单文件下载取得，用完即删，不要再克隆整棵 Linux 树：

```bash
curl -fsSLo /tmp/halrf_hwimg_raw_data_8852b.h \
  https://raw.githubusercontent.com/spacemit-com/linux-6.6/\
31c449aeaad8c7759bc983ca0e26946e5b6746dc/drivers/net/wireless/realtek/\
rtl8852bs/phl/hal_g6/phy/rf/halrf_8852b/halrf_hwimg_raw_data_8852b.h
```

| 文件 | 内容 |
| --- | --- |
| `chip/k1/k1_rtl8852bs_rf_radio_a_8852b.inc` | RFE 0x01/CV 1 选中的 radio A 镜像，947 项，sha256 `5b7d975c8b432791de3f86e8a63ce535b1a9bb6084263c41739d77018dfe5a3e` |
| `chip/k1/k1_rtl8852bs_rf_radio_b_8852b.inc` | 同上 radio B，932 项，sha256 `bfa62d034e3b62c468e7a8e8e01013de668c43cd7b651445396ebb2091f788d3` |
| `chip/k1/k1_rtl8852bs_rf_headline_8852b.inc` | 14 行 `{RFE, CV, images}` 运行时守卫表，`images` bit0=radio A、bit1=radio B |

三个文件由工具生成，不得手工编辑，也不要把带条件的原始表加进来。

#### 三、运行时 headline 守卫

`k1_rtl8852bs_rf_select_headline()` 在板上重放 `halrf_sel_headline_8852b()` 的五个
case：`{RFE 命中, CV 命中}`、`{RFE 命中, CV don't care}`、`{RFE 命中, CV 取表内最大}`、
`{RFE don't care, CV 取表内最大}`，全不命中则与原厂一样中止（`-ENOENT`）。选中的 headline
的 `images` 必须为 `0x3`（radio A 与 radio B 都与编译进来的镜像逐字节相同），否则打印
`RF CR guard refused` 并返回 `-ENOTSUP`，不下发任何 RF 写入。

该守卫已做过穷举等价测试：把 C 函数原样抽出，在主机上针对真实的
`k1_rtl8852bs_rf_headline_8852b.inc` 编译，枚举 rfe `0..255` × cv `0..15` 共 4096 组，
与生成器对真实原厂头文件的忠实重放逐组比较，并把 `images` 判定与"实际遍历原厂表再对两条
path 取摘要"的基准真值比较。结果为**4096 组全部一致、0 处不符、158 组会放行**。
当前 14 条 headline 中，`RFE 0x01/CV 0`、`RFE 0x02/CV 0`、`RFE 0x0b/CV 1`、
`RFE 0x0c/CV 1` 的 `images` 为 `0x0`，即这些板必须重新生成镜像，而不是沿用当前编译结果。

#### 四、RF 写入复用 CMD_OFLD：`wireless_fw_runtime_rf_cr_diag`

`CONFIG_K1_RTL8852BS2_RUNTIME_RF_CR_DIAGNOSTIC` 复用 PHY CR 已验证的
`fwcmd_cmd_ofld` 批处理与 C2H 等待机制，但按 `halrf_wrf()` 的原厂语义编码：

- 普通 RF serial-interface 写：`source=RF`、`path` 取 A=0/B=1、`offset` 为 RF 地址。
- RF D-die 写（地址带 `BIT(16)`）：原厂把它折算成 BB 口径，
  `direct_addr = offset_write_rf[path] + ((addr & 0xff) << 2)`，
  `offset_write_rf[2] = {0xe000, 0xf000}`，并使用 `source=BB`（**不是** `RF_DDIE`）。
- 所有值都按原厂 `MASKRF = 0x000fffff` 下发（`halrf_cfg_rf_radio_{a,b}_8852b()`
  对每一项都用这个固定掩码，表里没有 per-entry mask）；超出该掩码的值会被拒绝而不是
  截断。另外，`0xf9..0xfe` 只在 `halrf_cfg_rf_nctl_8852b()` 里表示延时，radio A/B
  表没有这层含义；已确认两张原厂 radio 数组的**所有分支**都没有落在该区间的地址，
  运行时仍对这种地址硬停，以防喂进这个遍历读不懂的表。
- radio A 用 sequence `0x80` 起、radio B 用 `0xc0` 起，与 PHY CR 的 `0x40..0x7f`
  不重叠；A 为 60 批、B 为 59 批，均在 `0xff` 上限内。
- 每批提交后等待匹配的 CMD_OFLD C2H；**第一批失败即返回**，不对固件可能已经接收的
  事务做任何重放。

原厂 OUTSRC class 8 / class 9 的 radio-to-FW 分页上传**故意不发送**：
`rtw_hal_rf_config_radio_to_fw()` 位于 `USE_TRUE_PHY` 之后，`halrf_config_rf_parameter()`
不会走到它，所以 `init_rf_reg` 阶段本身从不上传该分页；它也没有 C2H 完成合同，发送它
等于一次无法验证的盲 H2C，与"每批失败立即停"的约束冲突。

#### 五、构建与实体板验收命令

构建与静态检查已通过：`tools/check_k1_sources.sh` 全部通过；
`wireless_fw_runtime_rf_cr_diag` 的 ELF 为 ELF64 RISC-V、entry `0x11000000`、LOAD 段不重叠
且无 RWX，sha256 `6c4a40f98b38b650da25a48413cec53b7d6c09657259b370e40c88f65c3086bb`
（text 635374 / data 8848 / bss 19952）。`wireless_fw_runtime_rf_context_diag`、
`wireless_fw_runtime_scanofld_phy_cr_fwofld_diag`、`wireless_fw_runtime_scanofld_rx_diag`
的回归构建也都通过，确认新增的 `#ifdef` 拆分没有破坏关闭这两个选项的 profile。

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless_fw_runtime_rf_cr_diag \
  --build-dir cmake_out/k1-wireless-rf-cr \
  --package --package-dir out/k1-wireless-rf-cr --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-rf-cr/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-rf-cr/k1-go-wrapper.bin \
  --manual-reset \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-mac-core --require-runtime-bb-rf \
  --require-rf-context --require-runtime-rf-cr \
  --require-runtime-control-plane --require-runtime-address-cam \
  --require-runtime-role-cam-done-ack \
  --require-runtime-scanofld-channel-done-ack \
  --require-runtime-scanofld-passive \
  --require-bringup-success
```

`--require-rf-context` 要求出现 `K1 Wi-Fi GPL: RF context read complete`；
`--require-runtime-rf-cr` 要求出现 `RTL8852BS2 RF radio image complete` 与两条
`RF radio A|B offload entries=` 汇总，并在日志中出现 `RF radio [AB] offload response`
（仅在失败时打印）或 `RF CR guard refused` 时判定失败。

`--require-h5` 不要加在这个 profile 上：`CONFIG_K1_BT_H5_LOCAL_VERSION_PROBE` 只在
`wireless_bt_hci_diag` 与 `wireless_bt_vendor_firmware_diag` 打开，本 profile 编译时就不会
发 HCI Read Local Version，那条门控只会稳定判负。

#### 六、实体板结果（2026-08-29）

`wireless_fw_runtime_rf_cr_diag` 已在 MUSE Pi Pro 跑通，串口日志
`out/k1-serial/k1-wireless-rf-cr-20260828T231742Z.log`。

固件下载之前读到的板级 RF 上下文：

```
RF context SYS_CFG1[0x00f1]=0x1d chip CV=0x1
RF context rfe[0x2ca]=0x1 board_option[0x2c1]=0x21 chan_plan[0x2b8]=0x7f
RF context xtal[0x2b9]=0x45 thermal_a[0x2d0]=0x20 thermal_b[0x2d1]=0x1e
RF context tssi_de[0x210..0x259] programmed=0x2d/0x4a rx_gain_k[0x2d4..0x2dd] programmed=0x5/0xa
RF context calibration=present rfe_source=efuse
```

这块板是 **RFE 0x01 / CV 1（CBV）**，且 `rfe_source=efuse` 说明 RFE 是真的从 eFuse 读到的，
不是 `hal_rfe_type_chk()` 失败后回退到 halrf 默认值 0x1，与生成镜像时用的
`--rfe 0x01 --cv 1` 完全一致，无需重新生成。board_option、xtal、thermal A/B 以及部分
TSSI-DE / RX gain-K 都已烧写，这块板带有真实 RF 校准数据。

运行时 headline 守卫的判定与主机端 4096 组穷举预测逐字段一致：

```
RF CR guard rfe=0x1 cv=0x1 case=0x1 headline=0x2 images=0x3 compiled_for rfe=0x1 cv=0x1
```

两条 path 的镜像都被运行中的固件接受：radio A `entries=0x3b3 batches=0x3c`（947 项 / 60 批，
末批 `index=0x3b0 entries=0x3 sequence=0xbb`）、radio B `entries=0x3a4 batches=0x3b`
（932 项 / 59 批，末批 `index=0x3a0 entries=0x4 sequence=0xfa`），随后
`RTL8852BS2 RF radio image complete`。只在失败时才打印的
`RF radio [AB] offload response` 与 `RF CR guard refused` 在日志中**出现 0 次**，
即 119 批全部拿到 C2H `result=0`。

同一次运行其余阶段也都满足：`firmware runtime diagnostic complete`、runtime transport、
H2C loopback、MAC core、`BB/RF release complete`、`PHY CR image complete`、
`runtime control-plane serialization complete`、`runtime address CAM serialization complete`、
`runtime role/CAM done-ack complete`（`sequences=1,2,3,4`）、
`passive scan channel-list done-ack complete`、`passive scan-offload complete`，
无 `bring-up failed`，最后进入 NSH。

1/6/11 passive scan-offload：三个信道各收到 `reason=0x3`（换信道）C2H，信道 11 上收到
`reason=0x5`（扫描结束），扫描报告为
`scan-report=0x1 report-rx=0 report-ch=0 bss=0 beacon=0 probe-rsp=0 ssid-len=0 bssid=0`，
即**本次没有解析出任何 Beacon/Probe Response**。该 profile 没有打开
`CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_RX_DIAGNOSTIC`，本来就不解析 RX 帧，所以这条结果
既不能说明空口 RX 已经工作，也不能说明 RF 镜像有问题；要判断必须用同时打开 RF CR 与 RX
门控的 profile 复跑。

工具最终判定为 FAIL，唯一缺失的标记是 `K1 Bluetooth: H5 local version`——命令行里带了
`--require-h5`，而本 profile 编译时不含该探测（见上）。BT 侧本身正常：
`H5 raw HCI registered /dev/ttyHCI0`、`H5 bring-up complete`。这次 FAIL 是验收命令的门控
写错，不是驱动回归；Wi-Fi/RF 侧要求的每一条标记都已在日志中逐条核对存在。

首次尝试（`out/k1-serial/k1-wireless-rf-cr-20260828T231402Z.log`）与 RF 代码无关：
`loadx 0x11008000` 传到约 24 个 ACK 时 SoC 复位（XMODEM 重发 `C` 后出现 `sys: 0x200` /
`U-Boot SPL`），板子转去启动 flash 里的 Bianbu Linux，工具在等 `Ready for binary` 时超时；
日志里还有 `1 KiB packets exhausted their retries; falling back to 128-byte packets`。
改用 `--gzip-payload`（647824 → 315517 字节，约 10 个分块而不是 20 个，`loadx 0x13000000`
之后 `unzip`）后一次传完，这也是之前 phy-cr-fwofld 成功运行用的路径。

仍未完成，禁止误报：本节只证明"radio A/B 参数镜像已按原厂 headline 选择下发并被固件逐批
确认"。`init_rf_reg` 的 power-by-rate、power limit、power limit RU、power track、xtal track
五张表仍未实现，NCTL/RFK 仍在范围外，空口 RX、`wlan0`、关联与联网都还没有任何证据。

#### 七、`wireless_fw_runtime_rf_cr_rx_diag`：RF 镜像 + Beacon/BSS RX 门控

上一节的 `bss=0` 无法区分"射频真的收不到"和"这个 profile 根本不解析 RX 帧"，因为
`CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_RX_DIAGNOSTIC` 在 `wireless_fw_runtime_rf_cr_diag`
里是关的。新增 profile 只做一件事：在 RF CR 之上打开已有的 RX 门控。

```
# board/k1/muse_pi_pro/configs/wireless_fw_runtime_rf_cr_rx_diag/defconfig
#include "../wireless_fw_runtime_rf_cr_diag/defconfig"
CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_OFLD_RX_DIAGNOSTIC=y
```

该符号 `depends on K1_RTL8852BS2_RUNTIME_SCAN_OFLD_PASSIVE_DIAGNOSTIC`，而 RF CR 的
include 链里已经有 passive scan-offload，所以只需这两行。它不发 probe、不注册网络设备、
不关联、不认证，也不写 eMMC/SPI flash/eFuse/U-Boot 环境。

构建通过：`tools/check_k1_sources.sh` 全绿，ELF 为 ELF64 RISC-V、entry `0x11000000`、
LOAD 段不重叠且无 RWX，sha256
`1600d8740a9da95edef080a95354904ea6941f0f03089bb25a56ffc3966178a5`
（text 635392 / data 8848 / bss 19952）。

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless_fw_runtime_rf_cr_rx_diag \
  --build-dir cmake_out/k1-wireless-rf-cr-rx \
  --package --package-dir out/k1-wireless-rf-cr-rx --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-rf-cr-rx/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-rf-cr-rx/k1-go-wrapper.bin \
  --manual-reset --gzip-payload --boot-timeout 300 \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-mac-core --require-runtime-bb-rf \
  --require-rf-context --require-runtime-rf-cr \
  --require-runtime-control-plane --require-runtime-address-cam \
  --require-runtime-role-cam-done-ack \
  --require-runtime-scanofld-channel-done-ack \
  --require-runtime-scanofld-passive \
  --require-runtime-scanofld-rx \
  --require-bringup-success
```

`--require-runtime-scanofld-rx` 要求 `passive scan RX ... bss=0x1`，即至少解析出一个合法
BSS。这是"是否继续 `wlan0`/关联/认证"的唯一前置判据：只有它通过，且 SDIO RX 出现
packet type 0 且带 Beacon/Probe Response BSSID，才允许往网络层推进；绝不能靠注册 `wlan0`
或伪造扫描结果宣称完成。

如果 `bss=0`，排查方向**不是**补 `init_rf_reg` 剩下的五张表。已核对原厂实现：
`halrf_config_power_by_rate/power_limit/power_limit_ru/power_track/xtal_track` 五个函数
全部只调用对应的 `halrf_config_8852b_store_*()`，把表存进 `rf->pwr_info`、
`rf->pwr_track`、`rf->xtal_track` 等驱动内部结构（power-by-rate 的表是每项 4 个 u32
`{band, tx_num, rate_id, data}`，落到 `pwr->tx_pwr_by_rate[band][rate]`），
**init 阶段一个寄存器都不写**。芯片只在后续算发射功率、做热敏/晶振跟踪时才拿这些值。
也就是说 radio A/B 镜像就是 `init_rf_reg` 面向芯片的全部工作，这五张表只影响**发射**
正确性，与能否收到 Beacon 无关。`bss=0` 要往 `bb_dm_init`（BB DM/AGC）、RFK
（DACK/IQK/DPK/TSSI）、scan 期间的 RX filter/队列处理，以及我们自己的 RX FIFO 解析上查。

### 2026-08-29：RX 门控实板结果——空口 RX 仍为零，缺口定位到 `bb_dm_init` / `rf_dm_init`

`wireless_fw_runtime_rf_cr_rx_diag` 第四次尝试完整跑通（前三次分别是传输中复位、断电
约 15 分钟、运行中复位）。RAM-only，无 `saveenv`、无 eMMC/SPI/eFuse 写入。

除 RX 之外的门控全部满足，与 08-28 的 RF CR 跑法一致：`rfe[0x2ca]=0x1`、
`SYS_CFG1[0x00f1]=0x1d` → `CV=0x1`、`RF CR guard rfe=0x1 cv=0x1 case=0x1 headline=0x2
images=0x3`、radio A `entries=0x3b3 batches=0x3c`、radio B `entries=0x3a4 batches=0x3b`、
`RTL8852BS2 RF radio image complete`，无一条 offload response 失败。

扫描本身也成功：`passive scan-offload wait error=0x3d C2H=0x5 done-ack=0x1
scan-events=0x4 enter-mask=0x7 next=0x7 end=0x1 last=ch0xb reason=0x5 status=0x1`，
`firmware-return=0x0`。1/6/11 三个信道都进入并推进，固件报告扫描结束、返回码 0。
`error=0x3d` 就是 61 = `ENODATA`，正是 RX 判据本身：
`match.done_ack.firmware_return == 0 && require_bss && !match.first_bss_valid`。

空口侧全零：整个三信道 × 250 ms 的扫描窗口里 RX FIFO 只出现 **5 个包，全部
`type=0xa`（C2H）**，`type=0` 的空口帧一个都没有，`crc=0 icv=0`（没有被 CRC/ICV
过滤掉的帧）。`passive scan RX data=0 mgmt=0 beacon=0 probe-rsp=0 bss=0`。

RX filter 快照已按 `mac_reg_ax.h` 逐位核对，**不是过滤器问题**：

- `R_AX_RCR (0xce00)` 低 16 位 `0x000f` → `CH_EN=0xf`、`STOP_RX_IN=0`（RX 未停）；
  高 16 位 `0x20f3` 属于 `R_AX_DLK_PROTECT_CTL (0xce02)`。
- `R_AX_PLCP_HDR_FLTR (0xce04)=0x006f`：CCK CRC/SIG、L-SIG parity、SIG-A CRC 检查开。
- `R_AX_RX_FLTR_OPT (0xce20)`：扫描前 `0xf0170001` → 扫描中 `0xf017000f` → 扫描后
  还原。低 4 位 `0xf` = `B_AX_A_MC|B_AX_A_BC|B_AX_A_A1_MATCH|B_AX_SNIFFER_MODE`，
  Beacon 是广播，`B_AX_A_BC` 已放开。
- `R_AX_MGNT_FLTR (0xce28)=0x55555555`：16 个管理帧子类型每个 2 bit，全部 `0b01`
  = forward to host。顺带解释了固件自己的 `report-rx=0`——管理帧只转给 host，没有转给
  WLAN CPU（那需要 `0xaaaaaaaa`），所以固件的扫描报告本来就数不到帧，这不是新故障。

`phy_reg_gain` 也可以排除了，和五张功率表是同一类：`halbb_cfg_bb_gain_ax_8852b()` 走
和 radio 表相同的 headline/IF/CHK 走表，但每个表项最终只调
`halbb_cfg_bb_gain_8852b()`，而后者只写 `bb->bb_gain_i.lna_gain[][][]`、
`tia_gain[][][]` 和 rpl offset，**一个寄存器都不写**，只影响 RSSI/RPL 报告精度。

同样排除 RFE GPIO：`halrf_rfe_type_gpio_setting()` 只调 `halrf_set_gpio()` →
`ops->set_gpio_by_ch`，而 `halrf_ops_rtl8852b.c` 没有注册这个 op，对 8852B 是空操作。

真正缺的三块，按 `hal_start_8852b()` 的顺序：

1. **`halbb_reset_bb()` / `halbb_bb_reset_8852b()`**——`rtw_hal_init_bb_reg()`
   （hal_api_bb.c:301）= `halbb_init_reg()` + `halbb_reset_bb()`，我们只做了前半段的
   phy_reg。后半段是十几个带掩码的 BB 写：TSSI protect on（`0x58dc` BIT30|31、
   `0x5818` BIT30、`0x78dc`、`0x7818`）、PD disable（`0x2344` BIT31=1、`0xc3c` BIT9=1）、
   停 phy-sts（read8 `0xce40` 清 bit0，延时 2 us）、**BB reset 脉冲 `0x704` BIT(1)
   1→0→1**、重开 phy-sts（`0xce40` bit0=1）、PD enable（`0x2344` BIT31=0 仅 2.4 G、
   `0xc3c` BIT9=0）、TSSI protect off。注意这不是“静态值不对”：
   `k1_rtl8852bs_phy_reg_8852b.inc` 里最后一次写 `{0x0704u, 0x601c05ffu}`（bit1=1，
   RSTB_ASYNC 已释放）、`{0x2344u, 0x0006318au}`（BIT31=0，PD 已开）、
   `{0x0c3cu, 0x2840e1bfu}`（BIT9=0）、`{0x20fcu, 0x00000000u}`（ADC 已开）都已经是
   工作值，缺的是**复位脉冲本身**和 `0xce40`（phy-sts / PPDU stat 使能，phy_reg 表里
   根本没有这个地址）。
2. **`rf_dm_init` = `halrf_dm_init()`**（halrf_init.c:695），我们完全没实现。其中会写
   芯片的有：`halrf_config_nctl_reg`（NCTL 表）、`halrf_si_reset`、
   `halrf_aack_trigger`、`halrf_lck_trigger`（LO/VCO 校准）、`halrf_rck_trigger`
   （RC 滤波器校准）、`halrf_dack_trigger`，以及 efuse thermal/PA-bias/TSSI trim。
   RX DCK 不在这里，它推迟到 `halrf_chl_rfk_trigger(RFK_TYPE_PLATFORM_INIT)`。
   函数最后一行是一条 H2C：
   `halrf_fill_h2c_cmd(rf, 4, FWCMD_H2C_RF_INIT_CFG /*0xe*/, 0xa /*class*/,
   H2CB_TYPE_DATA, &rfe_type)`——**把板子的 RFE type 交给固件**。scan offload 期间是
   固件自己切信道、自己配 RF，而我们从未发过这条 H2C，固件不知道 RFE type = 1。
3. **`bb_dm_init` = `halbb_dm_init()`**：DIG/AGC 初值、EDCCA、CFO tracking。没有 DIG
   初始化时 AGC 门限停在上电默认值，也足以让 PPDU 一个都解不出来。

因此下一步的实现顺序（由省到贵）：(1) 补发 `FWCMD_H2C_RF_INIT_CFG`（class 0xa,
func 0xe, 4 字节 payload = rfe_type），走已有的 H2C 通路，一条命令；
(2) 实现 `halbb_bb_reset_8852b()`；(3) `rf_dm_init` 的 RF 校准（NCTL 表 + si_reset /
AACK / LCK / RCK / DACK）；(4) `halbb_dm_init()`。每一步都要重新构建、跑
`tools/check_k1_sources.sh` 与 ELF 检查，并且需要用户再按一次 RST 才能实板验证。

Wi-Fi **尚未完成**：在某次运行出现 `passive scan RX ... bss=0x1` 且带有效
Beacon/Probe-Response BSSID 之前，不注册 `wlan0`、不做 assoc/auth。

工具侧一处修复：`tools/run_k1_wireless_smoke.py` 的 `boot_wireless()` 过去在 `go`
之后一直等 `nsh>`，运行中复位会白等满整个启动超时（第三次尝试因此浪费 25 分钟）。
新增 `wait_for_nsh_or_reset()`：一旦 `go` 之后重新出现 `U-Boot SPL 2022` banner 就
立即报错退出，说明这次运行什么都没测到。第三次尝试的复位发生在
`RTL8852BS2 bootstrap begin` 之后的 SDIO 命令启动阶段，同时 USB 串口适配器重新枚举
（`ttyUSB1` → `ttyUSB0`），是第三次复位类事件，归因板级供电/线缆而非 RF 代码。

### 2026-08-29（续）：补 `FWCMD_H2C_RF_INIT_CFG` 与 BB reset，新增 `wireless_fw_runtime_bb_reset_rx_diag`

按上一节记录的顺序实现了前两个候选，两者都在同一个新 profile 里，等待实板复位验证。

#### 候选 1：`halrf_dm_init()` 末尾的 RF init config H2C

`halrf_dm_init()`（halrf_init.c:695）最后一行把板子的 RFE type 交给固件：
`data_to_fw[0] = rf->phl_com->dev_cap.rfe_type;
halrf_fill_h2c_cmd(rf, 4, FWCMD_H2C_RF_INIT_CFG /*0xe*/, 0xa, H2CB_TYPE_DATA, ...)`。
它经 `rtw_hal_mac_send_h2c()`（hal_api_mac.c:2786）→ `mac_outsrc_h2c_common()`
（fwcmd.c:3382）下发，所以 FWCMD 头是 category `FWCMD_H2C_CAT_OUTSRC` = 2、class 0xa、
function 0xe，且 halrf 把 hdr 清零，即 `rec_ack = done_ack = 0`：**原厂不要任何应答**。

因此 `k1_rtl8852bs_runtime_rf_init_cfg_h2c()` 不能等 C2H，改用可观测的替代判据：提交前
读一次 SDIO TX 空闲页计数（`k1_rtl8852bs_h2c_resource_read()`），提交后轮询它是否回到原
值。固件把命令从 FIFO 取走才会归还这一页，页不回来就返回 `-ETIMEDOUT` 并停止，不重发
——与本组件“不盲发无法验证的 H2C”“每批失败立即停”的既有约定一致。它挂在
`k1_rtl8852bs_runtime_rf_cr_offload_init()` 的末尾（radio B 之后），位置与
`hal_start_8852b()` 里 `init_rf_reg` → `rf_dm_init` 的顺序一致。日志行：
`RF init cfg H2C rfe=… category=… class=… function=… pages=X->Y polls=N`。

#### 候选 2：BB reset——原厂在 io_ofld 打开时用的是**精简版**

核对原厂发现一处关键事实，改变了实现：`halbb_reset_bb_phy()`（halbb_api.c:928）并不总是
调 `halbb_bb_reset_8852b()`，当 `dev_cap.io_ofld` 为真（`halbb_check_fw_ofld()`，
halbb_fwofld.c:62）时它走 `halbb_fwofld_bb_reset_8852b()`
（halbb_8852b_fwofld_api.c:79）。后者只有 11 条写：

```
TSSI protect on : 0x58dc[31:30]=1, 0x5818[30]=1, 0x78dc[31:30]=1, 0x7818[30]=1
BB reset        : 0x704[1]=1, 0x704[1]=0, 0x704[1]=1
TSSI protect off: 0x58dc[31:30]=3, 0x5818[30]=0, 0x78dc[31:30]=3, 0x7818[30]=0
```

也就是说：**PD disable/enable（`0x2344` BIT31、`0xc3c` BIT9）和 `0xce40` phy-sts 的
停/开加 2 us 延时，在固件 I/O offload 路径里原厂自己就不做**。我们这个移植 WCPU 启动后
根本没有可用的主机 BB 写通路，全部 BB 写都走 cmd_ofld，正对应原厂的这一支；而且
phy_reg 镜像已经把 `0x2344` BIT31 和 `0xc3c` BIT9 都留在“PD 已开”的状态，精简版不碰它们
也没有需要恢复的东西。上一节里“缺的是复位脉冲本身和 `0xce40`”这句话要修正一半：
`0xce40` 在原厂 offload 路径上并不参与 BB reset，本阶段只**读一次**记录它的现值
（日志 `ppdu-stat=` / `phy-sts=`），不写。

实现是 `g_k1_rtl8852bs_bb_reset_fields[]`（复用既有的 `{address, mask, value}`
`k1_rtl8852bs_register_field_s`）加一个 `k1_rtl8852bs_runtime_bb_reset_offload()`：11 条
一次装进一个 batch（上限 16 条），最后一条带 LAST_COMMAND，然后用现成的
`k1_rtl8852bs_runtime_cmd_ofld_wait()` 等成功的 CMD_OFLD_RSP C2H；失败即结束该阶段，不
重放脉冲。sequence 用 0x20，与 PHY CR（0x40..0x7f）、radio A（0x80）、radio B（0xc0）
以及各条单发 H2C（0..8）全不重叠。

value 列的语义与本文件其它表**不同**，值得单独记一笔：`halbb_set_reg()` /
`halbb_set_reg_cmn()`（halbb_interface.c:319/374）在做主机侧移位**之前**就转到
`halbb_fw_set_reg()`（halbb_fwofld.c:194），后者 `cmd.value = val`、`cmd.mask = mask`
原样填进 cmd_ofld dword2/dword3，移位由固件完成。所以表里写的是**未移位的字段值**
（`0x58dc` 掩码 `0xc0000000` 值 `0x1`），不是整寄存器值。我们已有的 PHY CR（掩码
0xffffffff）和 RF（掩码 0xfffff、shift 0）镜像不受影响。另外
`halbb_fwofld_bitmap_en(false, ...)` 收尾时补的那条 `0x1a24[7:0]=0` 只是为了在 API 没置
last-command 时补一个 flush 标志，我们每个 batch 的最后一条本来就带该标志，不需要复制。

`0x704` 来自 `halbb_set_reg_cmn(..., phy_idx)`，`halbb_phy0_to_phy1_ofst()` 对
`HW_PHY_0` 返回 0，且 8852B 无 DBCC，因此不加相位偏移。

#### profile、门控与构建

```
# board/k1/muse_pi_pro/configs/wireless_fw_runtime_bb_reset_rx_diag/defconfig
#include "../wireless_fw_runtime_rf_cr_rx_diag/defconfig"
CONFIG_K1_RTL8852BS2_RUNTIME_BB_RESET_DIAGNOSTIC=y
```

新 Kconfig 符号 `depends on K1_RTL8852BS2_RUNTIME_PHY_CR_FWOFLD_DIAGNOSTIC`；阶段在
`board/k1/muse_pi_pro/src/k1_wireless.c` 里插在 PHY CR 与 RF CR 之间，对应
`rtw_hal_init_bb_reg()` = `halbb_init_reg()` + `halbb_reset_bb()` 的先后。

`tools/run_k1_wireless_smoke.py` 加了 `--require-runtime-bb-reset`：要求
`RTL8852BS2 BB reset complete` 与 `BB reset offload entries=`，且一旦出现
`BB reset offload response`（即某批没拿到成功的 CMD_OFLD_RSP）就判失败。同时
`--require-runtime-rf-cr` 里加了一条：必须出现 `RF init cfg H2C rfe=`。

构建通过：`tools/check_k1_sources.sh` 全绿，ELF 为 ELF64 RISC-V、entry `0x11000000`、
LOAD 段不重叠且无 RWX，sha256
`8c8b8ca9f3f1f589591b7d9248ce2a9115b97ab951a70132aca1e9fa170303db`
（text 637404 / data 8848 / bss 19952，比 `rf_cr_rx` 多 2012 字节 text）。

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/wireless_fw_runtime_bb_reset_rx_diag \
  --build-dir cmake_out/k1-wireless-bb-reset-rx \
  --package --package-dir out/k1-wireless-bb-reset-rx --jobs 8

tools/run_k1_wireless_smoke.py \
  --payload out/k1-wireless-bb-reset-rx/contest-nuttx-flat.bin \
  --wrapper out/k1-wireless-bb-reset-rx/k1-go-wrapper.bin \
  --manual-reset --gzip-payload --boot-timeout 300 \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-mac-core --require-runtime-bb-rf \
  --require-runtime-bb-reset \
  --require-rf-context --require-runtime-rf-cr \
  --require-runtime-control-plane --require-runtime-address-cam \
  --require-runtime-role-cam-done-ack \
  --require-runtime-scanofld-channel-done-ack \
  --require-runtime-scanofld-passive \
  --require-runtime-scanofld-rx \
  --require-bringup-success
```

仍是 U-Boot `loadx + go` 的 RAM-only 运行，无 `saveenv`、无 eMMC/SPI/eFuse 写入；需要用户
在监听器打开后按 RST。判据不变：只有 `passive scan RX ... bss=0x1` 且带有效
Beacon/Probe-Response BSSID 才允许往 `wlan0`/关联/认证推进。

（实板结果见下一节，本节的"尚未测得"已被取代。）

### 2026-08-29（续二）：候选 1/2 实板结果为负，RMAC 接收计数器把缺口锁定在基带之前

#### 实板运行方式：不再需要人工按 RST

`tools/run_k1_wireless_smoke.py` 的三条入板路径必须按板上当前运行的系统选，选错会白等一
个完整超时窗口：

| 板上现状 | 正确参数 | 机制 |
| --- | --- | --- |
| 原厂 eMMC Linux 在跑 | 默认（不加 `--manual-reset`） | `wait_for_adb` + `adb reboot`，再在串口截断 U-Boot autoboot |
| NuttX 在 `nsh>` | `--nsh-reboot` | 在 `nsh>` 发 `reboot`，停在 U-Boot，再补一次 U-Boot `reset` 以恢复干净的 RTL8852BS2 SDIO 上电状态 |
| 已经停在 `=>` | `--uboot-ready` | 直接 `loadx` |
| 只能人工复位 | `--manual-reset` | 等一次物理 RST，**不会**走 ADB 重启 |

先探一次串口再决定：向串口写 `\n`，回 `nsh> \x1b[K` 是 NuttX，回 `k1 login:` 是原厂
Linux。本次因为误用 `--manual-reset` 连续浪费两个窗口（`/tmp/k1-bb-reset-rx-run.log`
与 `run2.log`，串口日志 0 字节、`FAIL: did not acquire U-Boot prompt`），换成默认 ADB
路径后一次通过。串口日志约 1 MB 且含控制字符，`grep` 必须加 `-a`，否则静默无输出。

#### 候选 1（BB reset）与候选 2（`FWCMD_H2C_RF_INIT_CFG`）：固件接受，RX 无变化

`wireless_fw_runtime_bb_reset_rx_diag` 实板运行（默认 ADB 路径）两个新阶段都成功：

```
K1 Wi-Fi GPL: BB reset offload entries=0x0b sequence=0x20 ppdu-stat=0x0c00 phy-sts=0x0
K1 Wi-Fi GPL: RF init cfg H2C rfe=0x1 category=0x2 class=0xa function=0xe pages=0x20->0x20 polls=0x0
```

但 RX 判据仍然为零，且与上一轮 `k1-wireless-rf-cr-rx-20260829T002348Z.log` 的对应四行
**逐字节相同**：

```
K1 Wi-Fi GPL: scan report dword3=0x0 bytes=0x1c
K1 Wi-Fi GPL: passive scan RX reason=0x5 status=0x1 bss=0x0
K1 Wi-Fi GPL: scanofld passive wait error=0x3d ... end=0x1
K1 Wi-Fi GPL: bring-up failed: -61
```

每个 RXD 仍是 `type=0xa`（C2H），没有一个 type 0 的空口帧。结论：候选 1 与 候选 2 都被
固件正确接受，但对 RX 没有任何可测量的影响。

#### 新增判别性测量：RMAC 逐 PPDU 类型接收计数器

`bss=0x0` 本身无法区分"基带什么都没解调出来"和"解调出来但在送到 host 的路上丢了"。原厂
`mac_rx_cnt()`（`mac_ax/dbgpkg.c:3448`）暴露 48 个 RMAC 接收计数器，正好回答这个问题：

- `R_AX_RX_DBG_CNT_SEL` = `0xCEE0`：写 bit[5:0] 选一个计数器，读 bit[31:16] 取它的 16 位
  值（`B_AX_RX_DBG_CNT_SH` 16 / `_MSK` 0xffff），`B_AX_RXERR_RPT_RST` = BIT(8) 是复位触发。
- 索引表（`dbgpkg.h:196`，顺序 CCK, OFDM, HT, VHT-SU, VHT-MU, HE-SU, HE-MU, HE-TB）：
  CRC-OK `{3,0,6,10,14,18,22,26}`，CRC-FAIL `{4,1,7,11,15,19,23,27}`，
  FA `{5,2,9,13,17,21,25,29}`，PPDU `{48,48,8,12,16,20,24,28}`（48 = 该类型无此计数器）。
- 非 PPDU 类型的计数器名字来自 `dbgpkg.c:42` 的 `rx_cnt_type[]`：30 `INVD`、31 `RECCA`、
  32 `FULLDRP`、33 `FULLDRP_PKT`、34 `RXDMA`、35 `PKTFLTR_DRP`、36/37 CSI、38 `NDP_PPDU`、
  39 `CONT_FCS`、40..47 `USER0..7`。
- 只有复位路径会去动 `R_AX_RXGCK_CTRL`（`0xCE06`）的 `B_AX_DISGCLK` BIT(0)；**读路径不需
  要**，原厂读就是 `W8(reg, idx)` + `R16(reg+2)`，中间 1 µs 延时（SDIO 单次寄存器访问远
  超 1 µs，天然满足）。本组件只有 32 位间接访问，所以选择写保留 bit[15:8]、显式清掉
  BIT(8)，绝不会顺手复位计数器。

实现：`CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_PHY_COUNTER_DIAGNOSTIC`（`chip/k1/Kconfig:626`）
与 profile `board/k1/muse_pi_pro/configs/wireless_fw_runtime_scan_phy_counter_diag`
（`#include` BB reset profile 后只加这一个开关）。采样点在
`k1_rtl8852bs_fwdl_runtime_scanofld_passive_diagnostic_common()` 内、**扫描接收过滤器已
经生效之后**取基线，扫描结束后、过滤器恢复之前再取一次，因此差值只属于 1/6/11 三次
dwell。采样失败只打印 `scan PHY counters error=` 并被忽略，不改变扫描本身的结果。
`tools/run_k1_wireless_smoke.py --require-scan-phy-counters` 要求两次采样都出现且没有读
错误。

#### 第一次计数器结果：全零

`k1-wireless-scan-phy-cnt-20260829T020308Z.log`（`--nsh-reboot`，ELF sha256
`62df48f0337f5f788caa2ac15d6d9168a4ba8c84b4936e78345ecc589a7d8fc3`，
text 638794 / data 8872 / bss 19984）：

```
K1 Wi-Fi GPL: scan PHY counters before crc-ok=0x0 crc-fail=0x0 fa=0x0
K1 Wi-Fi GPL: scan PHY counters after  crc-ok=0x0 crc-fail=0x0 fa=0x0 delta-ok=0x0 delta-fail=0x0 delta-fa=0x0
```

cck / ofdm / ht 三条逐类型行也全是 `ok=0x0 fail=0x0 fa=0x0`。**连一次误检（FA）都没有。**
在 2.4 GHz 这种拥挤频段上，一个"能工作但解不出包"的接收机也会持续产生 FA；FA 恒为零意味
着基带的包检测器从未触发，故障点在接收 MAC **之前**——不是本组件的 RX 过滤器、DLE 或
FIFO 解析。这把嫌疑指向候选 3（`rf_dm_init` 的 RF 校准），而不是候选 4。

该运行的判定是 `FAIL: NuttX started without: RTL8852BS2 one-shot passive scan-offload`，
属预期：板上 profile 打开了 `..._SCAN_OFLD_RX_DIAGNOSTIC=y`，被动扫描阶段本身就会以 `-61`
失败。计数器行是这次运行的目的，已按设计产出。

#### 扩展计数器：证明计数器窗口是活的，且连 CCA 都没有

全零有两种读法："接收机确实什么都没收到"和"这个计数器窗口根本没响应"。所以把采样扩展成：

1. 顺带读 30 `INVD` / 31 `RECCA` / 32 `FULLDRP` / 33 `FULLDRP_PKT` / 34 `RXDMA` /
   35 `PKTFLTR_DRP`，即"帧到了但丢在哪一级"的分级证据；
2. 保留选择字整字，其低 6 位必须读回刚写进去的索引（选 RECCA 时应为 `0x1f`）——这是本组件
   唯一能证明窗口有响应的手段。`tools/run_k1_wireless_smoke.py --require-scan-phy-counters`
   现在会检查 `raw=` 的低 6 位是否等于 `0x1f`，不等即判失败。

`k1-wireless-scan-phy-cnt2-20260829T021741Z.log`（ELF sha256
`ba5340f8d49269631d09db8d959bd0e6402176c3e4517fb6eb812fc6fcc0dbd9`，
text 639604 / data 8872 / bss 19984）：

```
K1 Wi-Fi GPL: scan PHY counters before crc-ok=0x0 crc-fail=0x0 fa=0x0
K1 Wi-Fi GPL: scan PHY stage before recca=0x0 invd=0x0 fulldrp=0x0 fulldrp-pkt=0x0 rxdma=0x0 pktfltr-drp=0x0 raw=0x1f
K1 Wi-Fi GPL: scan PHY counters after  crc-ok=0x0 crc-fail=0x0 fa=0x0 delta-ok=0x0 delta-fail=0x0 delta-fa=0x0
K1 Wi-Fi GPL: scan PHY stage after  recca=0x0 ... raw=0x1f delta-recca=0x0 delta-rxdma=0x0 delta-pktfltr-drp=0x0
```

`raw=0x1f` 说明选择寄存器确实把索引锁存了，窗口是活的；同一块 RMAC 的 `0xCE40` 也读回
非零的 `0x0c00`。因此结论可以确定下来：

- `RECCA=0`——基带在三次 dwell 里**一次 CCA 都没报**；
- `PKTFLTR_DRP=0`——没有任何帧被本组件的接收过滤器丢掉；
- `FULLDRP / FULLDRP_PKT / RXDMA / INVD = 0`——没有任何帧丢在 RMAC 内部或送往总线的路上。

即：故障完全在接收 MAC 之前。本组件的 RX 过滤器、DLE、FIFO 解析、host 交付路径全部无罪，
接收机是"聋"的。

#### 下一步测量：host 侧读回射频状态

`RECCA=0` 之后第一个要问的是"射频到底调到哪个信道了"。原厂 `halbb_read_rf_reg_8852b_a()`
给出了 host 直接读 A-die RF 寄存器的方法（这些是基带寄存器，与 MAC 寄存器共用同一个间接
地址空间，现有 32 位访问器可直接到达）：

1. 轮询 `0x174c` 的 BIT(24) w_busy 与 BIT(25) r_busy，等串行接口两个方向都空闲；
2. 把 `(path << 8) | (addr & 0xff)` 写进 `0x378` 的 bit[10:0]，**循环直到读回相同值**；
3. 等 `0x174c` BIT(26) r_done，再从 `0x174c` 低 20 位取值（RF 寄存器只有 20 位）。
4. `halbb_read_rf_reg_8852b()` 用 `addr & 0x10000` 选 A-die/D-die（`DAV`=0），普通地址走
   A-die 这条路。

实现为 `CONFIG_K1_RTL8852BS2_RUNTIME_SCAN_RF_READBACK_DIAGNOSTIC` 与 profile
`wireless_fw_runtime_scan_rf_readback_diag`（`#include` 计数器 profile 后只加一个开关），
在扫描前后各采一次两条通道的 RF `0x00`（模式）、`0x05`（校准保存/恢复用）、`0x18`（信道，
低字节即当前调谐信道）、`0x1b`/`0x1c`（RCK 触发与 done）。**这是只读路径**：只写串行接口的
读选择寄存器，从不写 RF 寄存器，本组件所有 RF 写仍然只经固件 CMD_OFLD。
`--require-scan-rf-readback` 要求两次采样都出现且没有读错误。

这条路同时是候选 3 的前置条件：原厂 RCK/DACK 即使在 `io_ofld` 下也要 host 读回 RF 值再写
回（`halrf_rck_8852b()` 里 `rck_val = halrf_rrf(rf, path, 0x1b, 0x07C00)` 就在 fwofld 窗口
之外），所以 host 侧 RF 读是绕不开的基础设施，而不是一次性诊断。

ELF sha256 `9e21d6981435f20e00b3ab1cedbc3adfda0ae5bf021d61ff4efa98428203a583`
（text 640974 / data 8872 / bss 19984）。

### 2026-08-29（续三）：host 侧 RF 读回全零 → 根因是电源序列漏了一条 A-die pad 使能

#### 运行 5：三种访问者同时证明 A-die 是黑的

`out/k1-serial/k1-run5-rf-access-pkg-20260829T041039Z.log`，ELF sha256
`2c22936def24f1909fb24467bfdb8d1cd1b090e1cfde994e951d64609dbdc5f8`
（text 647504 / data 8872 / bss 19984）。这一轮只加诊断，不改任何硬件配置：

1. `pre-status`：每次 RF 读之前先采一次 `0x174c`，用来区分"本次读产生了 done"和"done 位
   本来就是 1"；
2. `k1_rtl8852bs_rf_write()`（原厂 `halbb_write_rf_reg_8852b_a()` 的忠实移植，只在诊断里
   调用）+ 回读命令寄存器 `0x370`；
3. `source=RF` 的固件 CMD_OFLD WRITE/COMPARE，以及 `source=RF_DDIE`（新增源 3）的 COMPARE
   作为对照。

结果（`0x` 前缀已缩短）：

```
scan RF readback before path=0x0 mode=0x0 r05=0x0 ch-reg=0x0 ch=0x0 rck=0x0 r5a=0x0 r5a-expect=0x7ffff
scan RF readback trace before pre-status=0x04000000 select=0x18 status=0x04000000 select-polls=0x0 done-polls=0x0 ddie-ch=0x1
scan RF access write path=0x0 value=0x7ffff command=0x05a7ffff readback=0x05a7ffff polls=0x0 error=0x0
scan RF access read  path=0x0 r5a=0x0 expect=0x7ffff status=0x04000000 error=0x0
scan RF access fw-cmp  path=0x0 expect=0x7ffff agree=0x0 error=0x5
scan RF access fw-rw   path=0x0 value=0x7ffff agree=0x0 error=0x5
scan RF access fw-ddie path=0x0 expect=0x0     agree=0x1 error=0x0
```

逐条读法：

- 串行接口的**命令**寄存器写得进去（`readback == command`，`polls=0`），所以 BB 窗口本身没问题；
- 每次 A-die 读都得到 0，且 `pre-status=0x04000000` 说明 BIT(26) done 在本次读**之前**就是 1
  ——这是一个陈旧的 done，`done-polls=0` 不是"很快就绪"，而是"从来没被清过"；
- **固件**自己的 `source=RF` COMPARE 也不同意（`agree=0`，`-EIO`/`error=0x5`），而同一批
  `source=RF_DDIE` 的 COMPARE 同意（`agree=1`）——负对照有效，排除了"我们把 CMD_OFLD 用错了"；
- 三种访问者（host SWSI、固件 RF、host D-die）里只有走 A-die 的两种失败。

结论：**A-die 对所有访问者都是黑的**，与我们的读实现无关；配合 RMAC 计数器全零（`RECCA=0`），
故障点固定在"A-die 未上电/未出时钟"。

#### 根因：`mac_pwron_8852b[]` 少抄了一条 `0x0018 BIT(5)`

用 GitHub blobs API 单文件取回原厂
`phl/hal_g6/mac/mac_ax/mac_8852b/pwr_seq_8852b.c`（不再克隆整棵树），把
`mac_pwron_8852b[]` 与本组件的 `g_k1_rtl8852bs_power_on[]` 做机械对比（脚本解析两张表、
剔除 PCIe/USB 专用项与结束哨兵）。差异恰好一条：

```c
/* mac_reg_ax.h */
#define R_AX_SYS_ADIE_PAD_PWR_CTRL   0x0018
#define B_AX_SYM_PADPDN_WL_RFC_1P3   BIT(5)   /* A-die WL 射频时钟 pad */
#define B_AX_SYM_PADPDN_WL_PTA_1P3   BIT(6)
```

原厂在**第一次和第二次 XTAL_SI（`0x90`）事务之间**写 `0x0018 BIT(5)`；我们抄到了相邻的
`BIT(6)`，漏了 `BIT(5)`。两条几乎一样的 `0x0018` 写被夹在两段完全相同的 XSI 块中间，是很容易
漏抄的一处。**没有这条，A-die 射频时钟 pad 一直处于 power-down，串行接口对任何事务都回 0。**

补上的表项（`chip/k1/k1_rtl8852bs_gpl.c`，`g_k1_rtl8852bs_power_on[]`）：

```c
  {0x0273, 0x80, 0x00, K1_RTL8852BS_POWER_POLL},

  /* SYS_ADIE_PAD_PWR_CTRL BIT(5) is SYM_PADPDN_WL_RFC_1P3, the A-die WL RF
   * clock pad.  mac_pwron_8852b sets it between the first and the second
   * XTAL_SI transaction; without it the A-die serial interface answers every
   * transaction with zero.
   */

  {0x0018, 0x20, 0x20, K1_RTL8852BS_POWER_WRITE},
  {0x0270, 0xff, 0x90, K1_RTL8852BS_POWER_WRITE},
  {0x0271, 0xff, 0x20, K1_RTL8852BS_POWER_WRITE},
```

补完之后同一个对比脚本给出 `vendor 55 ours 55 identical`——上电序列与原厂逐项一致。

#### 运行 6：A-die 活了，而且射频镜像其实一直都写进去了

`out/k1-serial/k1-run6-adie-pad-pkg-20260829T042729Z.log`，ELF sha256
`6bb52f691c7b5cf3348c533c911f737b9e186431d864f29b9f226f3a1d6de24e`
（text 647512 / data 8872 / bss 19984）。**只动了上面那一条表项**，profile 与诊断开关与
运行 5 完全相同：

```
scan RF readback before path=0x0 mode=0x337e1 r05=0x11 ch-reg=0x1001 ch=0x1 rck=0x3a00 rck-sts=0x73800 r5a=0x7ffff r5a-expect=0x7ffff
scan RF readback before path=0x1 mode=0x337e1 r05=0x11 ch-reg=0x1001 ch=0x1 rck=0x3a00 rck-sts=0x73800 r5a=0x7f000 r5a-expect=0x7f000
scan RF readback after  path=0x0 ... ch-reg=0x1c0b ch=0xb ...
scan RF access read r5a=0x7ffff（与 expect 相同）  fw-cmp agree=0x1 error=0x0
scan RF access fw-rw agree=0x1 error=0x0            fw-ddie agree=0x1
```

三点结论：

1. 两条通道的 `r5a` **正好等于 `r5a-expect`**，即射频镜像本该写进 `0x5a` 的值。也就是说
   947 条 radio-A / 932 条 radio-B 的固件 CMD_OFLD 写**一直是成功的**，此前只是读回是瞎的
   ——**假设 H14（"固件 RF 写没落地"）被否证**；
2. `ch-reg` 从 `0x1001`（ch 1）变成 `0x1c0b`（ch 11），说明扫描期间射频合成器确实跟着固件换
   信道；`rck=0x3a00`、`rck-sts=0x73800` 说明 RCK 也跑过了；
3. host SWSI 读、固件 `source=RF` COMPARE、固件 WRITE+COMPARE、D-die 对照四种访问者全部一致
   （`agree=1`, `error=0`）。

#### 运行 6 的另一半：**第一次从 SDIO 收到真正的 802.11 帧**

同一份日志里，被动扫描阶段第一次出现了 `rpkt_type=0`（WiFi 包）的接收描述符——运行 5 一条
都没有：

```
passive scan RXD0=0x80180196 RXD3=0x00200000 type=0x0 len=0x196 crc=0x0 icv=0x0   (ch 1)
passive scan RXD0=0x01000018 RXD3=0x00200001 type=0x1 len=0x18  crc=0x0 icv=0x0   (PPDU status)
passive scan RXD0=0x80180196 RXD3=0x00200000 type=0x0 len=0x196 crc=0x0 icv=0x0   (ch 6)
passive scan RXD0=0x80180154 RXD3=0x00200000 type=0x0 len=0x154 crc=0x0 icv=0x0   (ch 11)
passive scan RX data=0x3 mgmt=0x0 beacon=0x0 probe-rsp=0x0 bss=0x0
```

按原厂 `phl/hal_g6/mac/rxdesc.h` 解码（本组件的字段定义与之逐位一致，
`RPKT_TYPE` 在 dword0[27:24]，`WIFI=0 / PPDU=1 / C2H=10`；`DRV_INFO_SIZE` 在 [30:28]，
`LONG_RXD` 是 BIT(31)；CRC32/ICV 错误在 dword3 BIT(9)/BIT(10)）：

- 三帧长度 406 / 406 / 340 字节，`crc=0 icv=0`——**CRC 校验通过的真实帧**；
- `RXD0[21:16]` 即 `WL_HD_IV_LEN` = 0x18 = 24，正好是"无 QoS、无 IV"的 802.11 头长度，
  与 Beacon/Probe-Response 的头部完全吻合；
- 载荷偏移按原厂 `hal_api_mac.c:2231` 的 `offset = rxdlen + drvsize*8 + shift*2` 计算
  （本轮为 32 + 0 + 4 = 36），与原厂同式；
- 但本组件的管理帧解析对三帧都判为"非管理帧"，于是 `mgmt=0 beacon=0 bss=0`。

也就是说：**接收链已经能解出帧，缺口从"射频/基带聋"变成了"这三帧到底是什么"**。两种可能——
载荷偏移仍差几个字节（那么 frame control 取错了位置），或者这三帧确实是邻居网络的数据帧
而 Beacon 还没被收到（三次 dwell 只收到三帧，对 2.4 GHz 而言明显偏少）。

同一轮里 RMAC 计数器仍然是 `crc-ok=0 crc-fail=0 fa=0`（`before` 采到 `crc-fail=0x3`，
`after` 归零，`delta-fail=0xfffffffd` 即 −3，是被清零而不是递增）。计数器与实际收到的三帧
互相矛盾，说明**计数器采样窗口本身还需要复核**（选择字 `raw=0x1f` 正确，但 CRC-OK 索引可能
选错了），不能再用它单独判定"接收机聋不聋"。

被动扫描的最终判定仍是负：`passive scan-offload error=0x3d`、`bring-up failed: -61`、
`bss=0x0`。判据不变：只有 `passive scan RX ... bss=0x1` 且带有效 Beacon/Probe-Response
BSSID 才允许往 `wlan0`/关联/认证推进。

### 2026-08-29（续四）：把帧打印出来 → 缺口在本组件自己的 IE 解析

#### 运行 7：原样 dump 收到的帧

只加观测、不改行为：`k1_rtl8852bs_scanofld_log_frame()` 现在在 `rpkt_type=0`（WiFi）时追加打印
`RXD meta hdr-iv/bb-sel/mac-info/shift/drv/rxd-len/payload-offset`、长描述符的 `RXD1/4/5/6/7`，
以及从 `offset + rxd-len` 起的前 64 字节十六进制。

- ELF `7c04e5fb21f5eda5d0d166e81bb4bf6eedcedd6f9de99c1290086696b3b0002a`，text 648502 / data 8872 / bss 19984；
- 串口日志 `out/k1-serial/k1-run7-rxdump-pkg-20260829T044936Z.log`；
- 元数据：`shift=0 drv=0 rxd-len=0x20 payload-offset=0x20`，与原厂
  `offset = rxdlen + drvsize*8 + shift*2` 一致。

第二帧的 dump 直接给出了答案：

```
passive scan frame=80000000ffffffffffff564f3be2e6d2564f3be2e6d27020<tsf>6400 2104 0000 0108...0301 01...
```

按 802.11 拆开：FC=`0x0080`（管理帧 / 子类型 8 = Beacon）、addr1=广播、addr2=addr3=BSSID
`56:4f:3b:e2:e6:d2`、beacon interval 100 TU、SSID IE 长度 0（隐藏 SSID）、DS Param 信道 1。
硬件自己的解码 `RXD4=0x02070008`（TYPE=管理帧、BC=1、SEQ=0x207）与软件解码互相印证；第一帧
是受保护的数据帧（FC=`0x4208`，SEQ 0x898 == `RXD4=0x0898000a`）。

**结论：载荷偏移正确，接收链端到端正确。** `mgmt=0` 不是硬件没收到，而是本组件把已经解出来
的 Beacon 自己丢掉了。

#### 根因：IE 遍历对帧尾不容错

`k1_rtl8852bs_runtime_mgmt_parse()` 的元素遍历把"剩余长度不足一个元素"当成协议错误返回
`-EPROTO`，而 `k1_rtl8852bs_scanofld_observe_wifi()` 对 `ret < 0` 整帧丢弃。上报长度会覆盖尾部
FCS / 硬件补齐，dwell 边缘的 Beacon 也可能截断——两者都不影响前面已经解出的 BSSID/信道/SSID。
修法：走到不完整元素就 `break`，保留已解字段；并新增
`passive scan frame skipped ret= fc= len=` 诊断，下次再丢帧时能直接看到原因。

这也解释了运行 7 的 `passive scan-offload wait error=0x3d`：`0x3d = 61 = -ENODATA`，即
扫描卸载链上唯一未满足的条件就是 `require_bss && !first_bss_valid`；done-ack、enter mask
`0x7`、next `0x7`、scan-end 早就全过了。

#### 顺带修掉一个会把成功判成失败的工具侧 bug

`tools/run_k1_wireless_smoke.py` 的验收正则原本是
`rb"K1 Wi-Fi GPL: passive scan RX .*" rb" bss=(?:0x)?0*1\r?\n"`，要求 `bss=…1` 后**紧接换行**；
可这一行后面还有 ` channel= ssid-len= bssid=`，所以**即使板上真的收到 BSS 也永远不会匹配**。
已改为行内匹配（`[^\r\n]*`）、`bss=` 必须正好是 1（`(?![0-9a-fA-F])` 防止匹配 `0x1a` 之类），
并在同一行追加两条断言，使工具断言与判据字面一致：`beacon=`/`probe-rsp=` 至少一个非零、
`bssid=` 不是全零。核对过历史各轮日志：运行 5 没有 WiFi 帧、运行 6/7 的 `bss` 实际都是 `0x0`，
所以这个 bug 没有掩盖过任何一次真实的成功。

#### 运行 8：被动扫描第一次判定为正

- 构建：`--config board/k1/muse_pi_pro/configs/wireless_fw_runtime_scan_rf_readback_diag`，
  ELF `d3eb6f9435bf398c1e064f7171312c02f22f7018eb203f4b2e60dea597b1e3b5`，
  text 648624 / data 8872 / bss 19984（相对运行 7 只多 122 字节，就是这次的容错与诊断）；
- 串口日志 `out/k1-serial/k1-run8-mgmt-parse-pkg-20260829T050311Z.log`（1.75 MB，`grep -a`）。

```
K1 Wi-Fi GPL: passive scan RX data=0x3 mgmt=0x3 beacon=0x3 probe-rsp=0x0
              scan-report=0x1 report-rx=0x0 report-ch=0x0
              bss=0x1 channel=0x1 ssid-len=0x0 bssid=56:4f:3b:e2:e6:d2
K1 Wi-Fi GPL: passive scan-offload done-ack return=0x0
K1 Wi-Fi GPL: RTL8852BS2 passive scan-offload complete
```

三帧全部是 Beacon，覆盖两个 BSSID：`56:4f:3b:e2:e6:d2`（隐藏 SSID，capability `0x0421`，两帧）
和 `50:4f:3b:e2:e6:d2`（SSID IE 长度 2 = `"SB"`，capability `0x0431`，带 privacy 位），
三帧都带 `03 01 01`（DS Param 信道 1）、`05 04 …`（TIM）、`2a 01 …`（ERP），
`crc=0 icv=0`。没有出现任何 `passive scan frame skipped`。

工具判定：`PASS: K1 wireless RAM image reached NSH`，**无 FAIL 行**；本轮请求的 18 项要求
（含 `--require-runtime-scanofld-passive`、`--require-runtime-scanofld-rx`、
`--require-scan-rf-readback`、`--require-scan-phy-counters`、`--require-bringup-success`）全部满足，
日志中 `bring-up failed` 出现 0 次。RF 读回四访问者仍然全部一致
（`r5a == r5a-expect`，扫描后 `ch-reg=0x1c0b`，`rck=0x3a00`）。

**判据达成**（`passive scan RX … bss=0x1` + 有效 Beacon BSSID），可以开始往 `wlan0`/关联/认证推进。

仍然遗留、不要当成已完成：

1. 固件扫描报告依然是空的——`passive scan report dword3=0x0 bytes=0x1c`，`report-rx=0
   report-ch=0`。本轮的 BSS 是**本组件自己从 SDIO RX 帧解出来的**。这未必是缺陷：Linux 侧的扫描
   结果同样来自收到的 Beacon/Probe-Response，固件报告更接近辅助/调试通道。但 `dword3=0`
   （channel_count=0、report_size=0）与 `bytes=0x1c` 说明我们请求报告的方式或解析口径与固件不一致，
   在把扫描结果接到 `wlan0` 之前应当先弄清它到底该不该有内容，不要默认"空即正常"。
2. RMAC PPDU 计数器仍然自相矛盾（`before crc-fail=0x2`、`after` 全零、`delta-fail=0xfffffffe`
   即 −2），与实际收到的三帧对不上，计数器选择索引待复核；不能用它判定接收机健康。
3. TX 正确性与 RSSI 精度相关的初始化仍缺：`set_enable_bb_rf(hal, 0)` 的 disable 半边、
   `halbb_dm_init()`、`halrf_dm_init()` 正文（`halrf_config_nctl_reg` / `rck_trigger` /
   `dack_trigger`）、五张 `init_rf_reg` store 表、halbb `phy_reg_gain`。

### 2026-08-29（续五）：dwell 改成 RX 循环里的截止时间 → Beacon 从 3 帧涨到 8 帧

> 本节引用的串口行按 `0x0000000000000001` → `0x1` 压缩前导零（`k1_early_puthex` 固定输出 16 位
> 宽），其余字面不变。

#### 运行 9：连续排空 RX FIFO + 8 KB 聚合缓冲

运行 8 的三帧并不是空口只有三帧，而是**主机在每个 dwell 里只读了一次 FIFO**：dwell 原本是在
C2H 处理函数里 `up_mdelay(250)` 等出来的，而这个处理函数本身是从 RX 读循环里调用的，于是整个
250 ms 内没有任何人排空 RX FIFO，固件收到的帧除第一帧外全部丢在 FIFO 里。改法：

- C2H 处理函数只**布置**截止时间（`dwell_pending` / `dwell_deadline` / `dwell_channel`），由 RX
  读循环每轮开头调用的 `k1_rtl8852bs_scanofld_dwell_poll()` 到点后再发 NEXT_CH；
- RX 缓冲从单 C2H 的 512 B 提到 `K1_RTL8852BS_SCAN_OFLD_RX_MAX = 8192`，一次读走整个聚合；
  `-ENOSPC` 只计数不再中断（`oversize` / `oversize-max`）；
- 收到 scan-end 之后**继续排空**（`DRAIN_POLL_COUNT=256` 次成功读或 `DRAIN_IDLE_POLLS=32` 次空
  轮），`require_bss` 的判定挪到循环结束之后——最后一个 dwell 的 Beacon 往往还在 FIFO 里；
- 新增按 BSSID 去重的 BSS 表（`K1_RTL8852BS_SCAN_OFLD_BSS_MAX`），match 结构因此改为
  `kmm_zalloc`（BSS 表放不进 bring-up 栈）。

构建：`--config board/k1/muse_pi_pro/configs/wireless_fw_runtime_scan_rf_readback_diag`，
`nuttx.sha256 = 34651ed4e99b09dbb8c5d6b186d2ea00b916447bc56e474a34feafedf3c80ce8`，
text 650524 / data 8872 / bss 19984；串口日志
`out/k1-serial/k1-run9-scan-drain-pkg-20260829T055219Z.log`。

```
K1 Wi-Fi GPL: passive scan RX data=0x34 mgmt=0x1c beacon=0x8 probe-rsp=0x0
              scan-report=0x1 report-rx=0x0 report-ch=0x0
              bss=0x1 channel=0x1 ssid-len=0x2 bssid=50:4f:3b:e2:e6:d2
K1 Wi-Fi GPL: passive scan BSS entries=0x2 dropped=0x0 reads=0x19b oversize=0x0
              oversize-max=0x0 logged=0xc suppressed=0x18f
K1 Wi-Fi GPL: passive scan BSS[0x0] bssid=504f3be2e6d2 ds-ch=0x1 dwell-ch=0x1
              dwell-mask=0x3 beacons=0x4 probe-rsp=0x0 cap=0x431 interval=0x64
              ssid-len=0x2 ssid=5342
K1 Wi-Fi GPL: passive scan BSS[0x1] bssid=564f3be2e6d2 ds-ch=0x1 dwell-ch=0x1
              dwell-mask=0x3 beacons=0x4 probe-rsp=0x0 cap=0x421 interval=0x64
              ssid-len=0x0 ssid=0x0
K1 Wi-Fi GPL: passive scan-offload done-ack return=0x0
K1 Wi-Fi GPL: RTL8852BS2 passive scan-offload complete
```

空口帧 3 → 52（`data=0x34` 是"WiFi 类型帧总数"，命名待改），Beacon 3 → 8，即每个 BSSID 每
250 ms dwell 收到 2 帧，与 100 TU 的 beacon interval 完全吻合；`oversize=0x0` 说明 8 KB 缓冲
够用；两个 BSSID 是同一台 AP 的两个 BSS（`50:4f:…` SSID `"SB"`、`56:4f:…` 隐藏），都在信道 1。
判据仍然满足：`bss=0x1` + 有效 Beacon BSSID + `done-ack return=0x0` + `PASS`、无 `bring-up failed`。

两处当时无法解释、由运行 10 补上的现象：

1. `dwell-mask=0x3` 意味着同一个 BSS 在信道 1 和信道 6 的 dwell 里都出现过；
2. 24 行 `passive scan frame skipped ret=0x0` —— `ret=0x0` 表示解析根本没失败，这个诊断名字是
   错的，它们是普通数据帧/Null 帧（`fc=0x4188/0x4988/0x0148/0x1148`）。

#### 运行 10：逐 dwell 归属 + 帧分类，`dwell-mask=0x3` 是排空滞后造成的错归属

- 每个 dwell 独立的帧打印预算（进入信道的 C2H 里复位 `rx_frames_logged`）——运行 9 的全局 12 帧
  预算正好在信道 6 的 enter-C2H 处用尽，所以信道 6 的 dwell 只有计数没有输出；
- `passive scan frame skipped` 换成只在真正 `mgmt_parse` 失败时打印的
  `passive scan frame error ret= fc= len=`，并把数据帧/控制帧改为计数（`data=` / `ctrl=`）；
- 新增 `k1_rtl8852bs_scanofld_traffic_record()`：非 Beacon 帧也按 BSSID 归档（管理帧取 addr3，
  to-DS 数据帧取 addr1，from-DS 取 addr2），并输出逐 dwell 与管理帧子类型直方图。

构建 text 651994；串口日志 `out/k1-serial/k1-run10-scan-attrib-pkg-20260829T060754Z.log`。

```
K1 Wi-Fi GPL: passive scan RX data=0x6 mgmt=0x5 beacon=0x5 probe-rsp=0x0 ... bss=0x1
              channel=0x1 ssid-len=0x2 bssid=50:4f:3b:e2:e6:d2
K1 Wi-Fi GPL: passive scan BSS entries=0x3 dropped=0x0 reads=0x26b oversize=0x0
              oversize-max=0x0 logged=0x2 suppressed=0x264 parse-err=0x0 data=0x1 ctrl=0x0
K1 Wi-Fi GPL: passive scan dwell[0x0] ch=0x1 frames=0x5 mgmt=0x5 beacon=0x5
K1 Wi-Fi GPL: passive scan dwell[0x1] ch=0x6 frames=0x1 mgmt=0x0 beacon=0x0
K1 Wi-Fi GPL: passive scan dwell[0x2] ch=0xb frames=0x0 mgmt=0x0 beacon=0x0
K1 Wi-Fi GPL: passive scan mgmt subtypes=0,0,0,0,0,0,0,0,0x5,0,0,0,0,0,0,0
K1 Wi-Fi GPL: passive scan BSS[0x0] bssid=504f3be2e6d2 ... dwell-mask=0x1 beacons=0x2 ...
K1 Wi-Fi GPL: passive scan BSS[0x1] bssid=564f3be2e6d2 ... dwell-mask=0x1 beacons=0x3 ...
K1 Wi-Fi GPL: passive scan BSS[0x2] bssid=3e49ffc2ea86 ds-ch=0x0 dwell-ch=0x6
              dwell-mask=0x2 beacons=0x0 probe-rsp=0x0 mgmt-other=0x0 data=0x1
```

- 管理帧只有子类型 8（Beacon），`parse-err=0x0`，`crc/icv` 全 0；
- 两个 BSS 的 `dwell-mask` 都变成 `0x1`：**运行 9 的 `0x3` 是排空滞后造成的错归属**——超过 dwell
  截止时间之后才被读出来的帧，会被记到下一个信道名下，不是相邻信道串扰；
- 信道 6 只有 1 帧，且是一个本地管理地址（`3e:49:ff:c2:ea:86`，第二字节 bit1 置位）发出的数据
  帧；信道 11 一帧都没有。接收机在信道 1 之外**不是聋的**，但这一轮不足以判断"附近只有一台 AP"
  还是"灵敏度不够"，两种解释都没有排除。

#### 运行 9 与运行 10 之间还有一个未解释的量级差

同样的 3 × 250 ms dwell，运行 9 读到 52 个 WiFi 帧（其中 28 个管理帧），运行 10 只有 6 个
（5 个管理帧）；而**解析出来的描述符总数反而是运行 10 更多**（`logged+suppressed`：614 对 411，
注意运行 10 的 `logged` 每个 dwell 复位，所以只反映最后一个 dwell）。也就是说运行 10 里绝大多数
描述符不是 WiFi 帧（`rpkt_type != 0`，多半是 PPDU status），或者是 WiFi 帧但带 CRC/ICV 错被
`if (!crc_error && !icv_error && packet_type == 0)` 静默跳过了——当前没有任何计数器能区分这两种
情况。这正是运行 11 要补的观测：按 `rpkt_type` 的 16 项直方图 + `crc-err` / `icv-err` 计数。

#### 顺带定位到一个会挡住更宽扫描的尺寸上限

13 个 2.4 GHz 信道的 channel-list 内容需要 `4 + 13 * 28 = 368` 字节，而
`K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX` 当时等于 `K1_RTL8852BS_MACID_PAUSE_SLEEP_SIZE = 256`，
`k1_rtl8852bs_runtime_control_h2c_submit()` 对超限直接返回 `-EINVAL`。原厂
`mac_add_scanofld_ch()`（`fwofld.c:1486`）本身没有分包上限——它把整张链表一次序列化成
`sizeof(struct fwcmd_add_scanofld_ch) + list_size * 28` 再交给 `mac_h2c_common()`，`content_len`
是 `u16`。所以正确的修法是把本组件的上限抬到 384 并保留原有的命令卸载批大小，而不是分包。

### 2026-08-29（续六）：被动扫描扩到 2.4 GHz 全部 13 个信道

#### 改动

1. **H2C 内容上限**：`K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX` 从
   `K1_RTL8852BS_MACID_PAUSE_SLEEP_SIZE`（256）改为 `384u`，容纳 `4 + 13 * 28 = 368` 字节的
   channel-list；同时新增 `K1_RTL8852BS_CMD_OFLD_BATCH_MAX 256u`，让命令卸载的每批 16 条保持原
   来验证过的大小，不随上限漂移；并在 scan-offload 尺寸旁加了
   `#if K1_RTL8852BS_SCAN_OFLD_CONTENT_SIZE > K1_RTL8852BS_RUNTIME_H2C_CONTENT_MAX` 的编译期检查。
2. **信道表**：`PASSIVE_CHANNEL_COUNT` 3 → 13，`channels[] = {1..13}`；
   `k1_rtl8852bs_scanofld_passive_channel_index()` / `..._passive_channel()` 从三分支 switch 改成
   `channel - 1` / `index + 1`；`entered_channels` / `advanced_channels` / `dwell_mask` 从 8 位加宽
   到 16 位；`BSS_MAX` 12 → 24；`PASSIVE_POLL_COUNT` 4000 → 16000（13 × 250 ms 的 dwell 加排空）。
3. **帧分类计数**（回答续五留下的量级差）：新增按 `rpkt_type` 的 16 项直方图、`rx-total`、
   `crc-err`、`icv-err`；每个 dwell 的帧打印预算改为**只用于空口帧**（`packet_type == 0`）且降到 1
   帧——轮询串口打印一帧要 ~50 ms，而 C2H/PPDU-status 描述符已经有专门的计数与 C2H 轨迹。

#### 运行 11：13 信道链路全通，但 `entered_channels` 的位掩码被截断

`nuttx.sha256 = 928bff4af78e11b62b405521cab0352a223f71186570485436902d7115b87e97`，text 652370；
串口日志 `out/k1-serial/k1-run11-scan-band-pkg-20260829T062634Z.log`。

368 字节的 channel-list **被固件接受**（`channel-list done-ack return=0x0`，`pages=0x20`），13 个
enter-channel C2H 按 1→13 顺序到齐，13 条 NEXT_CH 全部发出，`scan-end` 落在信道 13。但工具判
FAIL：

```
K1 Wi-Fi GPL: passive scan-offload wait error=0x6e C2H=0xf done-ack=0x1 scan-events=0xe
              enter-mask=0xff next=0x1fff end=0x1 last=ch0xd reason=0x5 status=0x1 band=0x0
```

`0x6e = 110 = -ETIMEDOUT`：`next=0x1fff`（13 位全 1）而 `enter-mask=0xff` 只有低 8 位。原因是
`k1_rtl8852bs_runtime_scanofld_passive_match()` 里的局部变量 `channel_mask` 还是 `uint8_t`，
`1u << 8` 存进去就变成 0，信道 9–13 的 entered 位全部丢失，完成条件
`entered_channels == (1u << 13) - 1` 永远不成立。（`advanced_channels` 用的是
`1u << channel_index` 直接赋值，所以没被截断；而 `(advanced & 0) == 0` 恒真，dwell 反而照常推进
——这也是为什么控制链看起来完全正常。）修法就是把 `channel_mask` 加宽到 `uint16_t`。

这一轮的观测同样有用，先记下来：

```
K1 Wi-Fi GPL: passive scan rpkt types=0x3f,0x34e,0,0,0,0,0,0,0,0,0xf,0,0,0,0,0
K1 Wi-Fi GPL: passive scan BSS entries=0x5 ... rx-total=0x39c crc-err=0x0 icv-err=0x0
K1 Wi-Fi GPL: passive scan mgmt subtypes=0,0,0,0,0x23,0,0,0,0x5,0,0,0,0,0,0,0
```

**续五的量级差就此解释清楚**：924 个描述符里 63 个是空口帧（`rpkt_type=0`）、846 个是 PPDU
status（`rpkt_type=1`）、15 个是 C2H（`rpkt_type=10`），`crc-err` / `icv-err` 都是 0。也就是说
运行 10 的"614 个描述符只有 6 个 WiFi 帧"不是我们把带错误的帧静默丢了，而是 PHY 报的 PPDU
状态远多于 MAC 交付的 MPDU——这与扫描期 RX filter 只放行管理帧的配置一致，不是接收缺陷。

#### 运行 12：13 信道被动扫描实板通过

`nuttx.sha256 = 75cb8a436536e40e5ac711438fdf11cc20c1edf740a4c8bafbbde69e2ba32c0f`，text 652362；
串口日志 `out/k1-serial/k1-run12-scan-band-pkg-20260829T063205Z.log`。

```
K1 Wi-Fi GPL: passive scan channel-list done-ack return=0x0
K1 Wi-Fi GPL: passive scan RX data=0x52 mgmt=0x4f beacon=0x8 probe-rsp=0x0 scan-report=0x1
              report-rx=0x0 report-ch=0x0 bss=0x1 channel=0x1 ssid-len=0x2
              bssid=50:4f:3b:e2:e6:d2
K1 Wi-Fi GPL: passive scan BSS entries=0x4 dropped=0x0 reads=0x1a9 oversize=0x0
              logged=0x1 suppressed=0x19c parse-err=0x0 data=0x3 ctrl=0x0
              rx-total=0x1a9 crc-err=0x0 icv-err=0x0
K1 Wi-Fi GPL: passive scan rpkt types=0x52,0x148,0,0,0,0,0,0,0,0,0xf,0,0,0,0,0
K1 Wi-Fi GPL: passive scan mgmt subtypes=0,0,0,0,0x46,0,0,0,0x8,0,0,0,0,0x1,0,0
K1 Wi-Fi GPL: passive scan-offload done-ack return=0x0
K1 Wi-Fi GPL: RTL8852BS2 passive scan-offload complete
```

逐 dwell（13 个信道全部有帧）：ch1 7/7/6、ch2 9/9/2、ch3 6/6/0、ch4 5/5/0、ch5 7/7/0、ch6 5/5/0、
ch7 3/3/0、ch8 6/6/0、ch9 3/3/0、ch10 6/5/0、ch11 9/7/0、ch12 7/7/0、ch13 9/9/0
（frames / mgmt / beacon）。管理帧子类型：Probe Request 70 帧、Beacon 8 帧、Action 1 帧。

工具判定 `PASS: K1 wireless RAM image reached NSH`，**无 FAIL 行**，18 项要求全部满足，
`bring-up failed` 出现 0 次。

**这一轮把续五的开放问题回答了**：接收机在整个 2.4 GHz 频段都工作——13 个信道每个 dwell 都收到
3–9 个管理帧（主要是别的设备在做主动扫描发的 Probe Request），所以"信道 1 之外没有 Beacon"是
**环境**结论（附近可用范围内只有那一台 AP 的两个 BSS，其余设备的 AP 很可能在 5 GHz），不是灵敏度
或 AGC 缺失造成的接收缺陷。仍然没有据此放宽任何判据：BSS 表里另外两条（`08:15:ae:4a:ae:09`、
`64:13:ab:db:f6:28`）只有数据帧、没有 Beacon/Probe-Response，因此没有 SSID/capability，
只能算"看见过这个 BSSID 的流量"。

`564f3be2e6d2` 的 `dwell-mask=0x3`（ch1 + ch2）这次有了第二种解释：2.4 GHz 相邻信道中心只差
5 MHz、占用 22 MHz，信道 1 的 AP 在信道 2 的 dwell 里本来就收得到——`ds-ch=0x1` 始终给出真实
信道，这正是记录 DS Param IE 的意义。

顺带：RMAC PPDU 计数器这一轮不再自相矛盾（`before crc-ok=0 crc-fail=0 fa=0` →
`after crc-ok=0x4 crc-fail=0x7 fa=0x1`，三个 delta 都是正数），但 11 个 PPDU 与实收 82 个空口帧、
328 个 PPDU-status 描述符仍不成比例，**计数器选择索引的缺口不算关闭**，依旧不能用它判定接收机
健康。

### 2026-08-29（续七）：把已验证的 13 信道被动扫描接到 `wlan0`（尚未实板验收）

判据在运行 8 已达成、运行 9/10/12 复现，因此本轮按验收顺序第 5 项把**同一套已验证的扫描**
接到一个真实的 `wlan0` 网络设备上。**没有新增任何射频/基带步骤，也没有新的空口能力**：
`wlan0` 只是把 boot 时那次 sweep 变成可按需重跑、并通过 wireless extensions 读回的接口。

#### 改动

1. **`chip/k1/k1_rtl8852bs_netdev.[ch]`（Apache-2.0，本项目实现）**：`netdev_register(...,
   NET_LL_IEEE80211)` + `d_ioctl`。`SIOCSIWSCAN` 在 `nxmutex` 保护下同步跑一次
   `k1_rtl8852bs_runtime_scanofld_passive_scan()`；`SIOCGIWSCAN` 把 BSS 表序列化成
   wireless-extensions 事件流。每个 BSS 依次发 `SIOCGIWAP`（`sa_family=ARPHRD_ETHER`）、
   `SIOCGIWESSID`、`SIOCGIWFREQ`（`e=0`、`m=channel`）、`SIOCGIWENCODE`。**不伪造
   `IWEVQUAL`**，所以 `wapi` 的 signal level 列恒为 0——RSSI 校准还没做，宁可打印 0 也不编。
   它只调用 GPL 组件在 `k1_rtl8852bs_gpl.h` 中的公开 API，与 `k1_wireless.c` 的调用方式相同，
   不含任何原厂寄存器表、结构体或代码改编（已记入 `docs/K1_SOURCE_AND_LICENSES.md`）。
2. **注册时机**：`k1_wireless.c` 里 `wlan0` 的注册排在 boot sweep **之后**，且以
   `probe_ret >= 0` 为前置——sweep 失败就不注册。这是刻意的：这个设备存在的意义就是重跑那次
   已经证明能收到 Beacon 的 sweep，在 sweep 失败后还注册一个 `wlan0`，正是
   「不要仅注册一个 `wlan0` 宣称完成」要避免的事。成功时打印
   `K1 Wi-Fi: wlan0 scan device registered`。
3. **新 profile `wireless_wlan0_scan_diag`**：`CONFIG_K1_RTL8852BS2_WLAN_NETDEV=y` +
   最小 NET + apps 的 `wireless/wapi`（BSD-2-Clause，故显式
   `CONFIG_ALLOW_BSD_COMPONENTS=y`）。
4. **`tools/run_k1_wireless_smoke.py`**：新增 `--require-wlan0-scan`、`--wlan0-ifname`、
   `--wlan0-scan-timeout` 与 `verify_wlan0_scan()`。它要求 boot 日志出现注册行、没有
   `wlan0 register error=`，`ifconfig` 里存在该接口，`wapi pscan wlan0` 输出
   `wlan0 sweep ret=0 end=1 bss=N`（`N>0`），表头 `bssid / frequency / signal level /
   encode / ssid` 存在，且解析出的 2412–2472 MHz、非全零 BSSID 行数**恰好等于 `N`**。

#### 三个实现层面的发现

1. **固件信道表是"替换"而不是"追加"**：`ADD_SCANOFLD_CH` 每次提交都用 `dword0` 的
   `NUM_OF_CH`/`SIZE_OF_CHINFO` 覆盖固件侧列表（原厂始终传 `clear_after_send=true`），
   所以重复提交同一份 13 条列表不会变成 26 个信道的 sweep。`fw_chlist_busy` 由
   ADD_SCANOFLD_CH 的 done-ack C2H 清除，start 重新置位、scan-end 再清除。因此
   add → 等 done-ack → start → 等 scan-end 这个序列**可以安全重复执行**，这正是
   `SIOCSIWSCAN` 能被反复调用的前提。
2. **RV64 上事件流的填充必须是指针宽度**：`wapi_event_stream_extract()` 不先拷贝事件，
   而是**直接从流里读 `u.data.pointer`（8 字节）**。in-tree bcmf 用的 4 字节 SSID 填充在
   RV64 上会让 SSID 长度 mod 8 落在 1..4 的每个 BSS 之后的所有事件错位 4 字节。改成
   `sizeof(FAR void *)` 向上取整；读端按 `u.essid.length` 取 SSID、按 `iwe->len` 前进，
   多出的填充字节被直接跳过，所以两端一致。
3. **`ninfo`/`nwarn` 属于 `CONFIG_DEBUG_NET_*`**（NETWORK 通道），不是
   `wlinfo`/`wlwarn`。沿用 bcmf netdev 的写法就必须在 profile 里打开
   `CONFIG_DEBUG_NET{,_ERROR,_WARN,_INFO}`，否则那行唯一的证据会被编译掉。GCC 定义了
   `CONFIG_CPP_HAVE_VARARGS`/`CONFIG_HAVE_FUNCTIONNAME`，因此实际串口行带函数名前缀：
   `k1_wlan_start_scan: wlan0 sweep ret=0 end=1 bss=4 ...`。

#### 一个必须记下来的自查：父 profile 选错会静默丢掉整条接收链

第一次构建时新 profile `#include` 的是 `wireless_fw_runtime_scanofld_rx_diag`。它虽然叫
"rx_diag"，但**不打开** `RUNTIME_BB_RF`、`RF_CONTEXT`、`RUNTIME_PHY_CR`（及其
`PHY_CR_FWOFLD` 重放）、`RUNTIME_BB_RESET`、`RUNTIME_RF_CR`（**A-die pad 使能就在这条路径
里**）与 `K1_SDIO_WIFI_HIGHSPEED_ONLY`。也就是说，那个镜像会用一条"续三之前"的聋接收链去跑
sweep，`bss` 必然为 0，`probe_ret < 0`，`wlan0` 根本不会注册——而且失败现象会看起来像
`wlan0` 有问题。已改为 `#include "../wireless_fw_runtime_scan_rf_readback_diag/defconfig"`，
即运行 8/11/12 实际验证过的那个 profile。对比 `cmake_out/k1-run11-scan-band/.config` 与新
`.config`，K1 侧符号除 `WLAN_NETDEV` 外完全一致。

顺带一个构建工具的坑：`tools/build_k1.sh` 在既有 build 目录上不会因为 defconfig 变化重新
生成 `.defconfig.processed`/`.config`——第一次重建后 SHA256 一字未变。改 profile 之后必须加
`--clean`（或换 build 目录），否则拿到的是旧配置的镜像。

#### 构建产物（仅构建，尚未上板）

```bash
tools/build_k1.sh --clean \
  --config board/k1/muse_pi_pro/configs/wireless_wlan0_scan_diag \
  --build-dir cmake_out/k1-wlan0-scan \
  --package --package-dir out/k1-wlan0-scan --jobs 8
```

`nuttx.sha256 = 296e95905cf2c144cde3313ee8b7078ba1b9bbd615d85ba880f59b89d3d67bd8`，
text 705260 / data 9568 / bss 24416。相对运行 12 的同一射频链镜像（text 652362）多
52898 字节，全部是网络层：`netdev` 核心与 wireless ioctl、`netlib`、`wapi` 命令行工具和
本组件的 netdev。`check_k1_elf.sh` 九项全 PASS，`tools/check_k1_sources.sh` 十一项全 PASS；
`riscv-none-elf-nm` 中 `k1_rtl8852bs_netdev_register`、`k1_wlan_ioctl`、`k1_wlan_start_scan`、
`k1_wlan_get_scan_results`、`k1_wlan_scan_format`、`netdev_wifr_ioctl`、`wapi_main` 均存在。

最小 NET 足迹（`.config` 实测）：`SOCK_CTRL` 绕过地址族，所以不需要 IPv4/IPv6/TCP/UDP 语义；
只有 `NET`、`NET_ETHERNET`（提供 `NET_ETH_PKTSIZE` 与 `d_mac.ether`）、`NETDEV_IOCTL`、
`NETDEV_WIRELESS_IOCTL`、`NETDEV_LATEINIT`（同时把依赖 `!NETDEV_LATEINIT` 的 EMAC 挡在镜像外）、
`DRIVERS_WIRELESS`、`DRIVERS_IEEE80211`、`NETUTILS_NETLIB`（`wapi` 会调
`netlib_getifstatus()`，该项默认 n）、`WIRELESS_WAPI{,_CMDTOOL}`、`WIRELESS_WAPI_STACKSIZE=8192`。
`NSH_NETINIT` 显式关闭——它在 NET 下默认 y，会去 up 一个根本不能承载流量的接口。

#### 明确不成立的说法

`wlan0` **只报告扫描结果**：没有 TX、没有数据 RX 路径、不关联、不认证、无密钥、无 RSSI、
无 IP 地址，其余 wireless 请求一律返回 `ENOTTY`；主动扫描与 SSID/信道过滤会打 `nwarn`
并被忽略（仍按被动全频段跑）。只有 Beacon/Probe-Response 解出的 BSS 会出现在结果里，
只见过数据帧的 BSSID 依旧只是计数器。`SIOCSIWSCAN` 是同步的，会占用调用任务数秒。

**本轮没有上板。** 在实板 run 通过之前，不得把这一项称为"完成"。

#### 待执行的实板验收命令

仍是 U-Boot `loadx + go` 的 RAM-only 运行，无 `saveenv`、无 eMMC/SPI/eFuse 写入；需要用户
在监听器打开后按一次 RST。

```bash
tools/run_k1_wireless_smoke.py \
  --payload out/k1-wlan0-scan/contest-nuttx-flat.bin \
  --wrapper out/k1-wlan0-scan/k1-go-wrapper.bin \
  --manual-reset --gzip-payload --boot-timeout 300 \
  --require-dle-scc --require-hci-flow-control \
  --require-firmware-layout --require-firmware-mss-legacy-signature \
  --require-firmware-full-download --require-firmware-runtime \
  --require-runtime-transport --require-runtime-h2c-loopback \
  --require-runtime-mac-core --require-runtime-bb-rf \
  --require-runtime-phy-cr --require-runtime-bb-reset \
  --require-rf-context --require-runtime-rf-cr \
  --require-runtime-control-plane --require-runtime-address-cam \
  --require-runtime-role-cam-done-ack \
  --require-runtime-scanofld-channel-done-ack \
  --require-runtime-scanofld-passive --require-runtime-scanofld-rx \
  --require-scan-rf-readback --require-scan-phy-counters \
  --require-runtime-data-tx-descriptor \
  --require-bringup-success --require-wlan0-scan
```

`--require-wlan0-scan` 是本轮唯一的新判据；它前面那 24 项针对的都是这条射频链原有的
marker，逐项在运行 12 的串口日志里核对过确实出现（运行 12 的命令行本身没有留档，所以这里
是按 profile 打开的诊断项重新推出来的，不声称与那次调用逐字相同）。

**第一次上板最可能的失败点**，先写在这里以免误读为工具 bug：`verify_wlan0_scan()` 要求
`wapi` 解出的合法行数**等于**驱动报告的 `bss=N`。如果某个 BSS 的 `ds_channel` 与
`dwell_channel` 都是 0，`SIOCGIWFREQ` 就会给出 0，该行被 2412–2472 MHz 过滤掉，数量对不上。
这是刻意的严格：它恰好是信道信息没接通的真实证据，应当去修信道来源，而不是放宽判据。

### 2026-08-29（续八）：`wlan0` 首次上板 → 前 24 项全通过，最后一项暴露 CMD53 512 字节上限

第一次把「续七」的 `wireless_wlan0_scan_diag` 镜像送上板（RAM-only `loadx + go`，无
`saveenv`／无 eMMC/SPI/eFuse 写入），日志
`out/k1-serial/k1-wlan0-scan-20260829T084638Z.log`（2339939 字节）。

前 24 项 `--require-*` 全部通过：开机自检的那一次 sweep 收到真实 Beacon
（`passive scan RX ... beacon=0xe bss=0x1 bssid=50:4f:3b:e2:e6:d2`，`BSS entries=0x6`
`dropped=0` `parse-err=0`），随后
`K1 Wi-Fi: wlan0 scan device registered`。也就是说射频链、扫描链、netdev 注册这三段都成立。

失败只发生在新增的 `--require-wlan0-scan`，即 `wapi` 触发的第二次 sweep：

```
K1 Wi-Fi GPL: passive scan BSS entries=0x3 dropped=0x0 reads=0x133 oversize=0x0
              oversize-max=0x0 parse-err=0x0 rx-total=0x133 crc-err=0x0 icv-err=0x0
K1 Wi-Fi GPL: passive scan-offload wait error=0x16 C2H=0x3 done-ack=0x1
              scan-events=0x2 enter-mask=0x3 next=0x1 end=0x0 last=ch0x2
              reason=0x3 status=0x1 band=0x0
k1_wlan_start_scan: wlan0 sweep ret=-22 end=0 bss=2 data-only=1 dropped=0
```

`error=0x16` 是 `EINVAL`。这次 sweep 已经收了 6 个 Beacon、建了 3 个 BSS
（`504f3be2e6d2` ssid "SB"、`564f3be2e6d2` 隐藏、`58be72f17448` 只见数据帧）才在第 2 个
信道上中断，`reads=0x133 == rx-total=0x133` 说明每次读恰好取回一个 rpkt——所以不是扫描
逻辑、不是 C2H、不是 dwell。

#### 根因：运行时 RX 读取走的是 byte-mode CMD53，硬上限 512 字节

`k1_rtl8852bs_runtime_rx_read()` 接受设备在 `SDIO_RX_REQ_LEN`(0x1108) 里公布的长度，上限
是本路径缓冲区 `K1_RTL8852BS_SCAN_OFLD_RX_MAX = 8192`；但它把这个长度直接交给
`k1_sdio_wifi_read()` → `k1_sdio_wifi_cmd53()` 的通用分支，而通用分支是
`if (length > K1_SDIO_CMD53_MAX_BYTE_COUNT /* 512 */) return -EINVAL;`。CMD53 byte mode
的计数字段只有 9 位（0 表示 512），所以 512 是协议上限，不是本组件随手设的数。

于是任何一个大于 512 字节的聚合（一个大 Beacon＋32 字节 RXD，或两帧被 NIC 聚合到一次
`RX_REQ_LEN` 里）都会让 sweep 以 `EINVAL` 中断。这解释了为什么现象与流量相关：开机那次
sweep 最大帧 406＋32＝438 字节，全程没越界所以通过；`wapi` 那次遇到了更大的聚合就断。
`oversize=0`（该计数器只统计 `ENOSPC`，即 `request_length > 8192`）把失败读的长度锁定在
(512, 8192]。

排除法：这条路径上其它 `EINVAL` 来源都是静态的或不可能触发——各处 NULL 指针检查、
`dwell_poll()` 的 `channel_index < 0`（ch2 不可能）、`h2c_resource_wait()` 返回的是
`ENOSPC`、FWDL 写入守卫对每个包都是定长判断。

#### 修法：按 SDIO 规范拆分成 block-mode ＋ 一个 byte-mode 余数

Linux SDIO core 对固定地址的大读取（`sdio_memcpy_fromio`，rtw89 读 RX port 就是这么读的）
本来就是这么做的：512 对齐的部分用 block-mode CMD53（BLOCK_SIZE/BLOCK_COUNT ＋
`MMCSD_MULTIBLOCK`），余数用一个 byte-mode CMD53，地址保持不变。所以这不是绕过，而是补上
原厂事务形态。

- `chip/k1/k1_sdio.c`：新增 `enum k1_sdio_wifi_cmd53_route_e`
  （`GENERIC`／`FWDL_FIFO`／`RX_FIFO`），`k1_sdio_wifi_cmd53()` 多带一个 route 参数。
  通用路径仍然只有 byte mode，FWDL 路径不变；新增的 `RX_FIFO` 路径守卫要求
  读方向、function 1、`increment == false`、地址等于
  `K1_SDIO_WIFI_RX_FIFO_ADDRESS = 0x1f00`、长度不超过 bounce buffer，且只接受
  「整数个 512 字节块」或「一个小于块的余数」两种形状。调用方无法通过换地址或换长度把这条
  路径扩成通用大传输 API。
- `chip/k1/k1_sdio.c` / `.h`：新增 `k1_sdio_wifi_rxfifo_read()`，把一次公布长度拆成若干
  ≤ bounce buffer 的传输依次读出。本 profile 里 `FW_FULL_DOWNLOAD_DIAGNOSTIC=y` ⇒
  `K1_SDIO_WIFI_FWDL_BLOCK_MODE` 成立 ⇒ bounce 2048、`MAX_BLOCK_COUNT=4`，因此 8192 字节
  最多拆成 4 次 4 块传输＋1 次余数，不需要动 bounce buffer 大小。设备侧 F1 的 FBR block
  size 本来就写的是 512（`board/k1/muse_pi_pro/src/k1_wireless.c:537`），与
  `K1_SDIO_CMD53_BLOCK_SIZE` 一致。
- `chip/k1/k1_rtl8852bs_gpl.c`：`k1_rtl8852bs_runtime_rx_read()` 改调该 helper，并在失败时
  打印 `runtime RX read error=<errno> length=<公布长度>`。这个打印是故意留的：下一次上板
  无论假设成立与否都能自证——安静通过说明 512 上限就是主因；打出长度则说明是另一个
  `EINVAL` 源（例如 R5 的 `OUT_OF_RANGE`）。

失败时不重试、不续读：余下的聚合留在设备 FIFO 里，任何丢失都仍然反映在计数器上。

#### 构建产物（仅构建，尚未上板）

```bash
tools/build_k1.sh --clean \
  --config board/k1/muse_pi_pro/configs/wireless_wlan0_scan_diag \
  --build-dir cmake_out/k1-wlan0-scan \
  --package --package-dir out/k1-wlan0-scan --jobs 8
```

`nuttx.sha256 = 8095db35d168fd2b259ab147f7db6031a360403a22c8619dbf68a12b94a5f36c`，
text 705694 / data 9568 / bss 24416（相对上一版 text ＋434 字节，全部是拆分 helper 与
那行失败打印）。`check_k1_elf.sh` 九项全 PASS，`tools/check_k1_sources.sh` 十一项全 PASS，
`riscv-none-elf-nm` 中 `k1_sdio_wifi_rxfifo_read` 与 `k1_rtl8852bs_runtime_rx_read` 均存在。

**本轮修改同样没有上板。** 验收命令与「续七」完全一致（25 项 `--require-*`），只是把
payload/wrapper 换成这一版镜像；仍需用户在监听器打开后按一次 RST，之后约 40 秒不要碰板子
（XMODEM 传输中的任何复位都会让整轮重来）。

#### 运行记录里另一条值得留下的教训

同一天的第二次尝试曾被我误判成「错过了 U-Boot 停止窗口」。读完整份工具日志后事实相反：
`=>` 提示符拿到了，两个看门狗都停了，wrapper 与 chunk 1–10 都传完了，板子是在
chunk 11 block 13 处**自发复位**（BROM `sys: 0x200 / try sd...` 后接 SPL banner）。看门狗
时间线排除了 WDT（60 秒超时在 [0.762]/[0.767] 启动、约 [3.0] 停止，复位发生在约 [39]），
所以原因是第二次按到 RST 或电源抖动。复位后 U-Boot 自动引导了 Linux，XMODEM 重试耗尽、
退回 128 字节包，`loadx 0x13050000` 落进了正在运行的内核，最终报
`FAIL: timed out waiting for b'Ready for binary'`。**诊断串口问题一定要读整份日志，不要只读
尾部。**

### 2026-08-29（续九）：CMD53 拆分读取实板确认，`wlan0` 失败点移到 `%g` 与判据本身

日志 `out/k1-serial/k1-wlan0-scan-20260829T091409Z.log`。仍是 RAM-only `loadx + go`。

#### 「续八」的 CMD53 修复在实板上成立

两条独立证据：

1. 整份日志里 **没有** `runtime RX read error=` —— 那行失败打印是这一版特意加的，安静就是
   通过。sweep 两次都跑到 scan-end：`k1_wlan_start_scan: wlan0 sweep ret=0 end=1 bss=3
   data-only=1 dropped=0`（上一轮是 `ret=-22 end=0`，死在第 2 个信道）。
2. block-mode trace 精确对上了机制。`CMD53 block count=0x4` 出现 168 次，但**全部**在第
   11878 行之前，也就是 FWDL 阶段（2048 字节包＝4 块）；而每一次 `wapi` sweep 里恰好出现
   一次 `CMD53 block count=0x1`。那一次就是原来会返回 `EINVAL` 的那个 >512 字节聚合：
   前 512 字节走一块 block-mode，余数走一个 byte-mode CMD53。开机自检那次 sweep 依旧
   一条 block-mode trace 都没有——它的聚合从没超过 512 字节，与「续八」的诊断完全一致。

`wapi` 也确实通过 `SIOCGIWSCAN` 把报告读回来了：

```
bssid / frequency / signal level / encode / ssid
18:3c:b7:8e:88:d0	*float*	0	0800	CMCC-ZAN3
50:4f:3b:e2:e6:d2	*float*	0	0800	SB
56:4f:3b:e2:e6:d2	*float*	0	8000
```

#### 但 `--require-wlan0-scan` 仍然 FAIL，而且是两处工具/配置侧的错，不在驱动里

`FAIL: wapi scan results carried no 2.4 GHz BSS row with a valid BSSID`。

**第一处：`*float*`。** `apps/wireless/wapi/src/wapi.c:822` 用 `%g` 打印这一列，因为
`wapi_freq2float()` 返回 `double`（`freq->m * pow(10, freq->e)`）。profile 没开
`CONFIG_LIBC_FLOATINGPOINT`，于是 `nuttx/libs/libc/stream/lib_libvsprintf.c:949` 对
`e..g`/`E..G` 一律输出字面量 `"*float*"`。也就是说这一列在此前的镜像里根本读不出来。
`pow()` 本来就由 `CONFIG_LIBM_TOOLCHAIN` 提供，所以只需要开 printf 那一侧：
profile defconfig 加 `CONFIG_LIBC_FLOATINGPOINT=y`（text ＋5074 字节，`__dtoa_engine`）。

**第二处：判据把这一列当成了 MHz。** 我原先要求 2412–2472，而驱动送出的是**信道号**。
驱动是对的，不是 bug：`nuttx/include/nuttx/wireless/wireless.h:518` 明确写了 `struct iw_freq`
的语义是「`0-1000 = channel`，`> 1000 = frequency in Hz`」，in-tree 的
`bcmf_driver.c:1084` 也正是 `u.freq.e = 0; u.freq.m = info->ctl_ch`。本组件
`k1_rtl8852bs_netdev.c:187` 与之一致。所以改的是 `verify_wlan0_scan()`：

- 先显式拒 `*float*`，并在错误信息里点名 `CONFIG_LIBC_FLOATINGPOINT`，避免下次又被误读成
  信道没接通；
- 第二列按文本捕获再 `float()` 解析（`%g` 可能给 `1`、`1.`、`1.00000` 任一形状），要求是整数
  且落在 1–13。

**严格性没有被放宽**：这条判据存在的意义是「信道信息没接通就必须失败」，而没解出信道的 BSS
在这里就是 0，0 不在 1–13 里，照样 FAIL。放宽的只是我对编码方式的错误假设。

驱动自己的 per-BSS 日志已经预示了开启 float printf 之后这一列会是什么：

```
BSS[0] bssid=564f3be2e6d2 ds-ch=0x1 dwell-ch=0x1 beacons=0x5
BSS[1] bssid=504f3be2e6d2 ds-ch=0x1 dwell-ch=0x1 beacons=0x3 probe-rsp=0x1 ssid=5342
BSS[2] bssid=183cb78e88d0 ds-ch=0x6 dwell-ch=0x6 beacons=0x1 ssid=434d43432d5a414e33
BSS[3] bssid=1055e4f70feb ds-ch=0x0 dwell-ch=0x6 beacons=0x0 data=0x1
```

前三个（ds-ch 1/1/6）就是 `wapi` 打出的三行；第四个只见到数据帧、没有 Beacon，因此不导出，
这正是 sweep 行里的 `data-only=1`。三行 valid ⇒ 与 `bss=3` 相等，数量校验也能过。

#### 构建产物（仅构建，尚未上板）

同一条 `tools/build_k1.sh --clean ...` 命令。
`nuttx.sha256 = a73d324becdec6ff23eba4016dc44009ded46eaf5dbd348c6a717ad2c2872ad5`，
text 710768 / data 9568 / bss 24416。`.config` 实测 `CONFIG_LIBC_FLOATINGPOINT=y`，
`riscv-none-elf-nm` 中 `__dtoa_engine` 存在。`check_k1_elf.sh` 九项、
`tools/check_k1_sources.sh` 十一项全 PASS。

**本轮修改仍未实板验收。** `wlan0` 不得称为完成。

### 2026-08-29（续十）：`wlan0` 扫描在实板通过 25 项全链

续九结束时判据本身还没被执行过一次。这一节记录三次上板（run 5/6/7），结论是
**`--require-wlan0-scan` 通过了**，而中间两次的失败一次是板子偶发复位、一次是我自己
把正确的判据改坏了。三种失败形态都写在这里，因为它们各自都会再出现。

#### run 5：板子在扫描中途自己复位，不是判据失败

镜像 `a73d324b…` 原样上板。boot、两次 `wdt stop`、11 段 XMODEM、`unzip`、`go` 全部正常，
NuttX 跑到 `wlan0` 注册之后，在 `wapi pscan wlan0` 执行到一半复位：

```
38912: K1 Wi-Fi SDIO: data block count=0x0000000000000000
38913: K1 Wi-Fi SDIO: data present=0x0000000001f70000<NUL><NUL>sys: 0x200
       try sd... / bm:3 / ERROR: CMD8 / ERROR: sd f! l:76 / bm:4 / nor m:0xef d:0x6017
       → SPL → U-Boot → 启动 eMMC 上的原厂 Linux
39045: FAIL: timed out waiting for b'nsh>': wapi pscan wlan0
```

识别特征，三条同时成立就是硬件级复位、和代码无关：

1. BROM 的 ` sys: 0x200` 直接切进一条 NuttX 打印的中间（这里是两个 NUL 之后接 banner），
   说明 UART 输出被打断在半行，不是程序自己走到了别处；
2. 全日志 `assert` / `PANIC` / `mcause` / `EPC` 命中数为 0——软件陷入一定会打 trap dump；
3. 复位后 U-Boot 不再被拦下（工具的一次 `s` 已经用掉），板子一路启到 eMMC 里的原厂系统，
   日志尾部是 `Bluetooth: hci0: Out-of-order packet arrived`。

判它不是代码问题还有一个横向证据：run 4 用几乎相同的镜像（只差浮点 printf，text 相差 5074 字节）
把同一段代码走通过两次——run 4 的 `wapi pscan wlan0` 在 33608 行发起、46133 行打印
`sweep ret=0 end=1 bss=3`，第二轮到 58667 行；run 5 死在 38913 行，正好在这段 trace 中途。

**处理方式是原样重跑**，不要为它改代码。另外我给用户的"按完 RST 约 40 秒不要碰板子"是错的：
XMODEM 只占前 40 秒，之后 NuttX 还要跑完两轮 `wapi pscan`，总共约 3 分钟都要 hands-off。
同类复位在 run 2 的 XMODEM 阶段也发生过一次。

#### run 6：`*float*` 修好了，露出来的数字是 MHz —— 判据是我改错的

`CONFIG_LIBC_FLOATINGPOINT=y` 生效，信道列打出了真实数字：

```
k1_wlan_start_scan: wlan0 sweep ret=0 end=1 bss=2 data-only=3 dropped=0
bssid / frequency / signal level / encode / ssid
56:4f:3b:e2:e6:d2	2412	0	8000
50:4f:3b:e2:e6:d2	2412	0	0800	SB
```

`2412` 是信道 1 的 MHz，不是信道号 1。根因在 `apps/wireless/wapi/src/wireless.c:370-400`,
`wapi_scan_event()` 的 `SIOCGIWFREQ` 分支自己做了换算：

```c
if (event->u.freq.e == 0)
  {
    /* Some drivers do not report frequency, but a channel. */

    if (event->u.freq.m >= 1 && event->u.freq.m <= 13)
      {
        info->freq = 2407 + 5 * event->u.freq.m;
      }
    else if (event->u.freq.m == 14)
      {
        info->freq = 2484;
      }
    else if (event->u.freq.m >= 36 && event->u.freq.m <= 165)
      {
        info->freq = 5000 + 5 * event->u.freq.m;
      }
  }
```

所以驱动按 `wireless.h` 的 `0-1000 = channel` 语义报 `e=0` ＋ 信道号是对的，
`wapi` 在 `%g` 之前把它折成 MHz 也是对的，**两侧都不用动**。续八/续九里我把验收脚本
从 2412–2472 MHz 改成判 1–13 信道号，是把本来就成立的判据改坏了——run 4 只是被 `*float*`
挡住看不见而已。这是这一轮唯一的返工，教训是：判据依赖 userspace 工具的输出时，
要先读那个工具的转换代码，不能只读驱动侧填了什么。

改回同时接受两种形态，并都折算回信道号（`tools/run_k1_wireless_smoke.py`）：

```python
        if 1 <= value <= 13:
            channel = value
        elif 2412 <= value <= 2472 and (value - 2407) % 5 == 0:
            channel = (value - 2407) // 5
        else:
            continue
```

严格性没有放松，反而是这个换算替我们兜住了：`wapi` 只换算它认得的信道，
信道没解出来时 `m = 0`、那一列留在 0，两种形态都判不过。用 run 6 的真实串口字节
（`out/k1-serial/k1-wlan0-scan-20260829T093907Z.log`）离线回放验证，替换 `nsh_command_retry_shell_error`
喂进录下来的字节即可，不用上板：

| 用例 | 期望 | 实测 |
| --- | --- | --- |
| run 6 原始字节 | PASS | PASS，`2 BSS: … ch1, … ch1` |
| 信道未解出（列 = 0） | 拒绝 | 拒绝 |
| `*float*` | 拒绝并指名 Kconfig | 拒绝 |
| 行数与 `bss=` 不符 | 拒绝 | 拒绝 |
| 5 GHz（5180） | 拒绝 | 拒绝 |
| 非 5 MHz 栅格（2413） | 拒绝 | 拒绝 |
| 全零 BSSID | 拒绝 | 拒绝（计数不符） |
| 裸信道号（6） | PASS | PASS |

**这一轮只改主机侧脚本，镜像没有重建**，所以 run 7 用的还是同一个 `a73d324b…`。

#### run 7：25 项 `--require-*` 全链通过

```
46138: k1_wlan_start_scan: wlan0 sweep ret=0 end=1 bss=4 data-only=1 dropped=0
46141: bssid / frequency / signal level / encode / ssid
       a6:39:b3:66:3b:34	2447	0	8000
       a4:39:b3:76:3b:34	2447	0	0800	B
       56:4f:3b:e2:e6:d2	2412	0	8000
       50:4f:3b:e2:e6:d2	2412	0	0800	SB
46146: PASS: K1 wlan0 passive scan reported 4 BSS (data-only=1 dropped=0):
       a6:39:b3:66:3b:34 ch8, a4:39:b3:76:3b:34 ch8, 56:4f:3b:e2:e6:d2 ch1, 50:4f:3b:e2:e6:d2 ch1
46147: PASS: K1 wireless RAM image reached NSH
```

第二行 PASS 是整链的判据：它在 `tools/run_k1_wireless_smoke.py:1440` 位于所有
`--require-*` 检查之后、`return 0` 之前，任何一项失败都会先抛 `XmodemError`，
所以它出现就等于 25 项全过；日志里也搜不到任何 `FAIL` 行。

- 镜像：`a73d324becdec6ff23eba4016dc44009ded46eaf5dbd348c6a717ad2c2872ad5`，
  text 710768 / data 9568 / bss 24416
- 串口日志：`out/k1-serial/k1-wlan0-scan-20260829T094725Z.log`
- 开机 sweep：`BSS entries=0x5 dropped=0 reads=0x124 parse-err=0 crc-err=0 icv-err=0`（27939 行）
- `wapi` sweep：`reads=0x1fa`、同样 `parse-err=0 crc-err=0 icv-err=0`（40514 行）
- 全程 `runtime RX read error=` 命中数 0
- 行数（4）与驱动自报的 `bss=4` 相等；`data-only=1` 那个 BSS 只见过数据帧、没有
  Beacon/Probe-Response，按设计不导出
- 隐藏 SSID 的行（`encode` 为 `8000`）最后一列是空的；上面几段引用把行尾的制表符去掉了，
  因为 `tools/check_k1_sources.sh` 会对文档里的行尾空白报错

**一条必须记下的边界**：run 7 的 sweep 聚合全部 ≤512 字节——33605 行之后没有任何
block-mode CMD53 trace（全日志 `CMD53 block count=0x4` ×168 与 `0x2` ×1 都在 11875 行之前的
固件下载阶段）。也就是说 **>512 字节拆分读取的实板证据来自 run 4**（每次 sweep 里恰好一次
`CMD53 block count=0x1`，位于 40469 / 53003 行），不是 run 7。聚合大小取决于当时空口有多少
帧被 NIC 聚合在一起，两次结果都真实。以后动 RX 读路径，两份证据都要看。

#### 范围与下一步

通过的只有"扫描能从 userspace 读回真实 BSSID 和真实信道"。**没有 TX、没有数据 RX 路径、
不关联、不认证、无密钥、无 RSSI（不伪造 `IWEVQUAL`）、无 IP。** 说"Wi-Fi 能扫到 AP 了"
是准确的，说"Wi-Fi 通了"是误报。

下一步是关联/认证，第一块砖是 TX：`--require-runtime-data-tx-descriptor` 至今只验证了描述符
构造，一帧都没发出去。先做到能发 Probe Request 并收到 Probe Response，再写 `SIOCSIWESSID`
和 Authentication/Association；这条路径同时会逼出"固件自己的 scan report 为空"和
"RMAC PPDU 计数器不可信"这两个已知缺口。TX 正确性依赖的初始化缺口（`set_enable_bb_rf(hal, 0)`
的 disable 半边、`halbb_dm_init()`、`halrf_dm_init()` 正文、五张 `init_rf_reg` store 表、
halbb `phy_reg_gain`）在补齐之前，不要把发不出去归因于 MAC 层。

### 2026-08-30（续十一）：主动扫描增量——缺口锁定在 WDE→CMAC 交接，新增 host 侧管理帧 TX 正向对照

被动扫描到 `wlan0` 这一段已经实板通过（续十）。这一节记录**主动扫描**这一项：固件必须
真的把一帧 Probe Request 发出去，主机必须收到 Probe Response。**它至今没有通过**，26 项
验收里只有这一项失败，所以本节全部是"缺口在哪里"的证据，不是完成报告。

#### 判据与工具

判据没有放松：只有 `passive scan RX … bss=0x1` 且带有效 Beacon/Probe-Response BSSID 才允许
往 `wlan0` / 关联 / 认证推进；主动扫描额外要求 `active scan probe response complete`。
两个脚本把这条链固定下来：

- `tools/build_k1_scanofld_active.sh` —— 构建 `wireless_fw_runtime_scanofld_active` profile
- `tools/run_k1_scanofld_active.sh` —— 上板执行，`--boot-timeout` 是等待物理 RST 的那个旋钮
  （`halt_at_uboot_from_serial()`，默认 300 秒；每 10 秒打一行
  `[serial] still waiting for a physical RST`）

有一条使用上的边界要记住：**监听器在提示按 RST 之前就已经把 payload 读进内存并 gzip 好了**
（日志第 2 行 `[host] gzip … bytes`）。所以窗口开着的时候在磁盘上重新构建，不会改变这次
上板要跑的镜像；想让新镜像生效必须先停掉监听器再重新武装。反过来说，这也意味着开发可以
和一个已武装的窗口并行进行。

#### 主动扫描链本身

`k1_rtl8852bs_fwdl_runtime_scanofld_active_diagnostic()` 的顺序，与原厂
`mac_add_pkt_ofld()` + `ADD_SCANOFLD_CH` 一致：

1. `runtime_probe_request_build()` —— 裸 802.11 管理帧（A1 广播、A2 eFuse self MAC、
   A3 通配 BSSID、SSID 长度 0、Supported/Extended Rates），不含主机描述符：固件给
   offload 的包自己加发送描述符
2. `scanofld_set_self_mac()` —— 记录 RX 侧判 A1 的地址，否则发给别人的 Probe Response
   会被算成给自己的回复
3. `runtime_tx_prerequisites()` —— 共存/PTA 仲裁与发射功率，读—写—回读
4. `runtime_sch_tx_en_management()` —— `R_AX_CTN_TXEN 0xC348` 的 `MGQ` BIT(8) 与
   `CPUMGQ` BIT(10)，OR 进去再回读
5. `pkt_ofld_add()` → `scanofld_chlist_submit()` → 被动 sweep 复用
6. 四个 `fault_snapshot()`（`pre-ofld` / `post-ofld` / `post-chlist` / `post-sweep`）
   与两次 `tx_state_sample()`

#### 板级 run 8/9/10：帧停在 WDE→CMAC 交接，且没有任何错误位

run 10 的四个快照结论最硬：`pre-ofld` / `post-ofld` / `post-chlist` **逐字节相同**

```
empty0=0x07ff079f empty1=0x001d003d qempty4=0x000fffff pub-pages=0
wde-hif=0 wde-wlcpu=0 wde-pktin=0 ple-txpl=0 ple-wlcpu=0 ple-h2c=0
```

只有 `post-sweep` 变了：`empty0=0x07fd039f qempty4=0x000fbfff wde-wlcpu=0x7`。

也就是说：**包不是在 offload 上传时占页的，是在 sweep 期间进了某个队列然后再没出来**，
这条恰好排掉了 run 9 的"模板常驻"假设。同时：

- 四个阶段的每一个 TX 侧 error ISR 全读 0
- `ptcl-fsm0/fsm1/phy=0`，`cpumgq-busy=0 mgq-busy=0`
- 两个 TMAC 计数器全 0，没有 NAV/CCA/response abort

前三条合起来只剩一个位置：**帧停在 WDE 到 CMAC 的交接上，在 CMAC PTCL 之前，而且没有任何
错误被抬起来**；那 7 个页记在固件自己的 WLAN_CPU 配额上，挂在一个 PKTIN 组的队列里。

#### 已经排除、不要再提的假设

下面每一条都有实板证据，重复验证是浪费上板机会：

- 全零的 TX-PPDU 表不是打印假象；loopback 的 `COUNT_CLR` 写不进去
- `R_AX_PKTIN_SETTING 0x9A00` / `B_AX_WD_ADDR_INFO_LENGTH` 复位值不是缺陷
- `R_AX_SIFS_SETTING 0xC624`、`R_AX_PTCL_FSM_MON 0xC6E8` 只在 PCIe/SW 模式有效
- `R_AX_PREBKF_CFG_0 0xC338` 在 SDIO 上不能写
- 未编程的 CPUMGQ EDCA 不会造成病态退避；CPUMGQ lifetime 过期；"CPUMGQ 没使能"
- 共存/PTA 仲裁；发射功率；BB `0x1804` txinfo-dBm
- Probe Request 帧字节、chinfo 布局、MAC loopback 寄存器状态
- `B_AX_CTN_CHK_TXNAV` / NAV upper
- RX 侧 / A1-match / ADDR_CAM 假设；`pub-pages=0` 是合法值
- SDIO 上 `B_AX_PCIE_MODE = 1` 不是 bug；`rtw_hal_rf_ic_cfg_init` 已经实现
- `ptcl-common=0x4003` 与原厂 HW 模式一致

另有两个 run-5 缺陷已在 run 6 修掉，**不允许回退**：`scheduler_init_ax()` 的语义是
*清除* `B_AX_BTCCA_EN`；写 LTE grant 字之前必须先置 `B_AX_LTE_MUX_CTRL_PATH` BIT(26)。

#### run 12：host 侧管理帧 TX 正向对照

到目前为止每一次测量的都是"固件自己去发"的那一帧：packet-offload 表里有它、信道表指名了它、
应该发它的那次 sweep 把每个发射计数器留在 0 而且一个错误位都没抬。**由主机自己写进管理
FIFO 的一帧，从 dispatcher 往后走的是同一条 CMAC / PTCL / 基带路径，但完全不经过固件的扫描
机器**，所以两者放在一起才能区分"这颗芯片根本发不出去"和"固件的发射路径没有启动"。

这一条同时不是一次性工作：关联和认证也必须这样发，所以这个描述符就是以后控制面要用的。

字段值全部取自原厂 `txdesc.h` 的 `MAC_AX_8852B_SUPPORT` 块与 `txdes_proc_mgnt_8852b()`，
并与 mainline `rtw89_core_tx_update_mgmt_info()` 交叉核对。WD BODY 24 字节，WD INFO 再 24 字节：

| 字段 | 位置 | 值 | 理由 |
| --- | --- | --- | --- |
| `STF_MODE` | body0 BIT(10) | 1 | SDIO 走 store-and-forward |
| `WDINFO_EN` | body0 BIT(22) | 1 | 管理帧必须带 WD INFO |
| `WD_PAGE` | body0 BIT(7) | **0** | 只有 PCIe 变体置它 |
| `CH_DMA` | body0 sh16 msk0xf | 8 | `MAC_AX_DMA_B0MG = 8` |
| `HW_SSN_SEL` / `EN_HWSEQ_MODE` | body0 | 0 / 0 | 用软件序号，不依赖未配置的 MAC 表计数器 |
| `TXPKTSIZE` | body2 sh0 msk0x3fff | 帧长（不含描述符） | |
| `QSEL` | body2 sh17 msk0x3f | **0x12** | `RTW89_TX_QSEL_B0_MGMT`；0x10 是 B0_BCN |
| `MACID` | body2 sh24 msk0x7f | 0 | 运行期 role/CAM 用的就是 MACID 0 |
| `WIFI_SEQ` | body3 sh0 msk0xfff | 软件序号 | |
| `USERATE_SEL` | info0 BIT(30) | 1 | 固定速率，绕开速率表 |
| `DATARATE` | info0 sh16 msk0x1ff | 0x000 | `RTW89_HW_RATE_CCK1`，2.4 GHz |
| `DISDATAFB` | info0 BIT(10) | 1 | 禁速率回退 |
| `BMC` | info1 BIT(11) | 1 | 广播帧不会被 ACK，不能按单播重试 |

其余全 0：无加密、无聚合、无 RTS、无 lifetime 覆盖、无 header 转换。
`init_8852b.c:820` 的 `wd_checksum_en = 0`，所以**不需要软件算 WD 校验和**。

**两处继承下来的错误已纠正**，任何一处都会静默地做出一个不发射的描述符：`wd_page` 在 SDIO
上必须是 0（原来计划写 1）；`QSEL` 是 0x12（原来计划写 0x10，那是 Beacon 队列）。

FIFO 编码沿用已在实板验证过的公式（原 H2C 通道 12 用的是同一条）：
`0x00010000 | (ch << 12) | length_units`，单位 8 字节、掩码 0xfff，写法是定地址 CMD53
（`k1_sdio_wifi_write(1, addr, false, buf, len)`）。band-0 管理队列因此是 `0x18000 | units`。
42 字节的 Probe Request 算出来是 `wd0=00480400 wd2=0024002a wi0=40000400 wi1=00000800`、
`fifo=0001800c bytes=00000060`、需要 2 个 PLE 页 / 1 个 WDE 页。

正向对照挂在 `scanofld_active_diagnostic()` 的**错误路径上、判决打印之后**
（`k1_rtl8852bs_runtime_mgmt_tx_probe()`，在 `maclbk_probe()` 之前），所以它既不能改变判决，
也不能把失败变成通过：它对 Probe Response 计数没有任何贡献。日志前缀是
`K1 Wi-Fi GPL: mgmt tx …`，不含 `" bss="` 也不含 `" probe-rsp="`，验收正则不会误命中。
它会打描述符四个字、FIFO 地址、写入前后的 ch8 已用页与 WP 可用页（页涨上去又落回来＝帧被
消费掉了，涨上去不落＝卡住，完全不动＝dispatcher 拒收），外加 `mgmt-before` / `mgmt-after`
两次 `tx_state_log` 与一次 `fault_snapshot("mgmt-post", true)`。

#### 原厂/mainline 参考代码的位置

`/home/sw/.cache/k1-refs/vendor/` 下是**按固定 revision 取下来的单文件**，不是完整树：

```
https://raw.githubusercontent.com/spacemit-com/linux-6.6/31c449aeaad8c7759bc983ca0e26946e5b6746dc/
  drivers/net/wireless/realtek/rtl8852bs/<path>     # 原厂
  drivers/net/wireless/realtek/rtw89/<file>         # mainline rtw89
```

**警告**：revision 是钉死的，和当前 upstream 不同步；`rtw89/sdio.c` 在这个 revision 下是 404，
所以 SDIO 侧只能看原厂 `_sdio.h`（`SDIO_TX_BASE 0x00010000`、
`SDIO_CMD_ADDR_TXFF_SHIFT 12`、`TXFF_0..TXFF_12`、`SDIO_CMD_ADDR_RXFF 0x1F00`）。
方法上的一条纪律：**"X 之前必须先发生什么"要先查原厂树，只有原厂子集缺的部分才回退到
mainline**；另外不要再整棵克隆 Linux 树，用单文件下载或临时目录并在用完后删除。

### 2026-08-30（续十二）：读原厂 dmac_init 后定位到从未初始化的 STA scheduler

本段没有板级数据（run 12 的窗口再次无人按 RST，镜像已换成 run 13），全部结论来自
原厂 `rtl8852bs` 子树，按"先读原厂、缺什么再看 mainline"的顺序做。

#### 被排除的三条先决条件（不要再追）

* `role.c:_add_role()` 对 STA 角色**不发 JOININFO**——`mac_h2c_join_info()` 只在
  `info->self_role == MAC_AX_SELF_ROLE_AP` 时调用；`init_cctl_info()` 与
  `mac_upd_dctl_info()` 在原厂里整段 `#if 0`。原厂 STA 路径只有
  `role_init → mac_fw_role_maintain → mac_upd_addr_cam → set_role_bss_clr`，本端口
  已经全部具备（`set_role_bss_clr` 只写 BSS color，HE-less Probe Request 用不到，
  复位值即 0）。上一段把 JOININFO / CCTLINFO / DCTLINFO 列为 rank-1 是照 mainline
  `rtw89_mac_vif_init()` 推的，原厂并不需要，降级。
* `mport.c:mac_port_init()` **从不置** `B_AX_PORT_FUNC_EN`：它开头只在端口原本使能时
  用 `MAC_AX_PCFG_FUNC_SW` 关闭，全函数没有再打开；打开是 PHL 另外调用
  `rtw_hal_mac_port_cfg(PCFG_FUNC_SW, true)` 的事，未关联的 STA 扫描不做。
* `fwofld.c:mac_general_pkt_ids()`（`FWCMD_H2C_CL_FW_INFO` / `FUNC_GENERAL_PKT`）本端口
  没实现，但原厂 `rtw_hal_mac_pkt_update_ids()` 只填 probersp/pspoll/nulldata/
  qosnull/cts2self，**根本不填 `probereq`**，所以它不是主动扫描的先决条件。

#### chinfo 编码：本端口与原厂一致

`hal_api_mac.c:5039 rtw_hal_mac_scan_ofld_add_ch()` 只设 `tx_pkt` 与
`probe_req_pkt_id`，与 `k1_rtl8852bs_runtime_scanofld_chlist_build()` 完全相同；
`mac_def.h:7425` 的 `mac_ax_scanofld_chinfo` 也确认 28 字节 7 dword 布局、
`tx_pkt` = dword1 byte1 bit4、`probe_req_pkt_id` = dword1 byte2 都对。mainline 换用
`num_addition_pkt` + `additional_pkt_id[]` 并把 `probe_req_pkt_id` 留 0xFF，是同一固件
的另一条等价通路，可作为后备实验，但不能说当前编码是错的。

#### rank-1：`sta_sch_init()` 从未执行

`trxcfg.c:927 dmac_init()` 的顺序是
`dle_init → preload_init → hfc_init → sta_sch_init → mpdu_proc_init → sec_eng_init →
sec_info_tbl_init`，`cmac_init()` 是
`scheduler_init → addr_cam_init → rx_fltr_init → cca_ctrl_init → nav_ctrl_init →
spatial_reuse_init → tmac_init → trxptcl_init → rmac_init → cmac_com_init → ptcl_init →
cmac_dma_init`。其中 `sta_sch_init()`（`trxcfg.c:162`）本端口**完全没有**：全仓库
grep `0x9e10` / `SS_CTRL` / `sta_sch` 无任何写入点，只有 0x9EF4 这个错误 ISR 被采样。

STA scheduler 正是"把 WDE 里非空的队列报给 CMAC scheduler"的那一级。它没使能时，
固件入队的帧会被计入某个 WDE 配额然后等一个永远不知道它存在的消费者——不丢包、
不置错误位、CMAC 计数器不动，与 run 8/9/10 四点快照记录的
`empty0=0x07fd039f qempty4=0x000fbfff wde-wlcpu=0x7` 完全一致。

8852B 硬件发送模式下的原厂序列（`mac_reg_ax.h:2869`，`TRXCFG_WAIT_CNT=2000`,
`TRXCFG_WAIT_US=1`）：

| 步骤 | 操作 |
| --- | --- |
| 1 | `R_AX_SS_CTRL (0x9E10) |= B_AX_SS_EN BIT(0)` |
| 2 | 轮询 `B_AX_SS_INIT_DONE_1 BIT(31)`，2000 × 1 µs |
| 3 | `|= B_AX_SS_WARM_INIT_FLG BIT(29)` |
| 4 | 清 `B_AX_SS_NONEMPTY_SS2FINFO_EN BIT(28)`（只有软件发送模式才置） |

`_patch_ss2f_path()` 对 8852B 是 `PATCH_DISABLE`（`chk_patch_ss2f_path()` 逐 cv 返回
DISABLE），所以没有对应实现。

run 13 把这段实现为 `k1_rtl8852bs_runtime_sta_sch_init()`，挂在
`k1_rtl8852bs_runtime_tx_prerequisites()` 里、coex/TX power 之前，只在主动扫描诊断
里跑，被动扫描那条已验收的路径一个字节都不改。它先打印
`sta-sch ctrl before=` / `polls=` / `init-done=`，再打印 `sta-sch ctrl after=`：
**如果 before 已经同时带 BIT(0) 与 BIT(31)，这条假设就被这一行自己否掉了**，不需要
再写第二个镜像去验证。

### 2026-08-30（续十三）：主动扫描的真正根因是 DMAC 时钟使能少了两位

本段把主动扫描从「唯一失败项」变成硬件已验证通过。根因不在扫描卸载、不在 H2C、
不在 Probe Request 报文，而在 `R_AX_DMAC_CLK_EN (0x8404)` 少写了两个时钟位。

#### 1. 板上访问不再需要人按 RST

板子一直停在 NSH 提示符上。往 `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`
（115200）发一个裸 `\r\n` 就回 `nsh>`，`help` 里有 `reboot`。因此
`tools/run_k1_wireless_smoke.py --nsh-reboot`（`halt_at_uboot_from_nsh()`，line 526）
可用：NSH `\n` → 等 `nsh>` → `reboot` → 等 U-Boot → `reset` → 再等 U-Boot。
全程 RAM-only，无 `saveenv`、无 eMMC/SPI/eFuse 写入。run 13/14/15 都是这样跑的。
注意 `--manual-reset` 与 `--nsh-reboot` 在 argparse 里是互斥组，不能往现成 wrapper
的命令行后面追加，要另起一条调用。

#### 2. `sta_sch_init()` 确实缺失，但不是本症状的原因

run 13 的自检行给出了结论：`sta-sch ctrl before=0x00e40000`（BIT(0)=0、BIT(31)=0，
复位后 STA scheduler 本来就是关的，本端口从未碰过它），`polls=0`、`init-done=1`、
`after=0xa0e40001`（BIT(31)|BIT(29)|BIT(0)，BIT(28) 为 0，与原厂终态逐位一致）、
`sta-sch init status=0`。**代码保留**——它是原厂 `dmac_init()` 的必要一步——但它
什么都没改变：run 13 的 `post-sweep` 与 run 10 逐字节相同
（`empty0=0x07fd039f qempty4=0x000fbfff wde-wlcpu=0x7`）。

#### 3. 决定性证人：host 注入的管理帧和固件的帧卡在同一个地方

run 13 第一次真正提交了 host 侧管理帧（`mgmt tx … bytes=0x60 wp-avail=0x280
ch8-used=0 need-ple=2 mgq-en=0x0`）。DLE 收下了它：`wde-hif=0x1`、`ple-txpl=0x1`、
`empty0` 从 0x07fd039f 变成 0x07dc039f（bit 16 与 bit 21 被清）。但
`delta-mactx-mpdu=0`、`delta-mactx-dma=0`，所有 TX-PPDU 计数器为 0，
`cmac-drop=0`、`dmac-drop=0`、`cca-abort=0`。

这把假设空间从「扫描卸载配置」压缩到「**DMAC→CMAC 的公共交接段，位于入队之后、
TMAC 之前**」：固件的帧和 host 的帧走的是两条不同的入口，却停在同一处。
（该诊断只挂在 `error:` 分支上，所以主动扫描一旦成功它就不再打印——run 14 里
`mgmt tx` 一行都没有，正是因为主动扫描返回了 OK。）

#### 4. 排除项（不要再追）

- **`preload_init` 对 8852B 是空操作。** `dle_8852b.c:450 preload_init_set_8852b()`
  直接 `return MACSUCCESS;`，`preload_cfg_set_8852b()` 同样。所以
  `R_AX_TXPKTCTL_B0_PRELD_CFG0 (0x9F48)` / `CFG1 (0x9F4C)` / `B_AX_B0_PRELD_FEN
  BIT(31)` 与本芯片无关。`preload_init()` 在 `dle.c:3299`，由
  `chk_preload_allow()` 把门。
- **`ser=0xf00000XX` 的低字节是自由跑动的字段**，不是故障：同一次运行内就会变
  （run 10 `c9`→`6d`，run 13 `55`→`8a`，run 14 `bd`→`7c`→`bb`→`14`）。
- **`disp-other=0x100` 是常量**，`pre-ofld`/`post-sweep`/run 10 三处一模一样。

#### 5. 四个使能字里只有一个和原厂不一致

| 寄存器 | 本端口 | 原厂 | 结论 |
| --- | --- | --- | --- |
| `R_AX_DMAC_FUNC_EN 0x8400` | `0xfb7d0000` | `init_8852b.c:1016` 同一张位表 | 一致 |
| `R_AX_DMAC_CLK_EN 0x8404` | `0x1f070000` | `0x0b1f0000` | **缺 BIT(19)、BIT(20)** |
| `R_AX_CMAC_FUNC_EN 0xC000` | `0xf000003f` | `init.c:219 cmac_func_en()` | 一致 |
| `R_AX_CK_EN 0xC004` | `0x4000003f` | 同上 | 一致 |

顺序也对（CMAC 先 ck_en 后 func_en；DMAC 先 func_en 后 clk_en）。

原厂 `dmac_func_en_8852b()` 写的 CLK_EN 是 `MAC_SEC(16) | BBRPT(17) |
DISPATCHER(18) | DLE_CPUIO(19) | PKT_IN(20) | STA_SCH(24) | TXPKT_CTRL(25) |
WD_RLS(27) = 0x0B1F0000`。本端口写 `0x1F070000`（bit 16,17,18,24,25,26,27,28）。
`k1_rtl8852bs_dle_scc_init()` 另外把 `K1_RTL8852BS_DLE_ENABLE_MASK =
(1<<26)|(1<<23)` OR 进 CLK_EN，所以 WDE/PLE 的时钟本来就有——真正的差异只有
**bit 19 `B_AX_DLE_CPUIO_CLK_EN` 和 bit 20 `B_AX_PKT_IN_CLK_EN`**
（外加一个无害的 MPDU_CKEN BIT(28)）。

契合得很准：固件自己的帧从 **CPU I/O** 进 DLE、记在 `wde-wlcpu` 配额上，host 的帧
从 **packet-in** 进 DLE、记在 `wde-hif` 配额上——恰好就是这两个没时钟的块。
时钟关着时，两种帧都能被"收下"并记账，却永远不会被推进下一级，也不会置任何错误位。
run 13 的宽证人对两种帧记录的正是这个现象。

#### 6. 改动（`chip/k1/k1_rtl8852bs_gpl.c`）

```c
#define K1_RTL8852BS_DMAC_CLK_EN_CPUIO     (1u << 19)
#define K1_RTL8852BS_DMAC_CLK_EN_PKT_IN    (1u << 20)
#define K1_RTL8852BS_DMAC_CLK_EN_FULL      (0x1f070000u | \
                                            K1_RTL8852BS_DMAC_CLK_EN_CPUIO | \
                                            K1_RTL8852BS_DMAC_CLK_EN_PKT_IN)
```

写入点在 `k1_rtl8852bs_runtime_mac_function_enable()`。该函数只回读校验两个
FUNC_EN、不校验 CLK_EN，所以另加了
`k1_rtl8852bs_runtime_block_enable_log()`，在 `runtime_tx_prerequisites()` 里于
`sta_sch_init()` 之前打印 `0x8400`/`0x8404`/`0xC000`/`0xC004` 的**实际生效值**，
把"后来被别处清掉"这种情况变成一行日志而不是又一次板上运行。

这个改动落在**公共** init 路径上，所以另外 25 项验收会被下一次运行重新走一遍。

#### 7. run 14 的板上结果：页面开始流动，主动扫描通过

回读证实两位已生效（`0x1f9f0000` = `0x1f1f0000 | (1<<23)`，bit 23 来自
`dle_scc_init()` 的 `DLE_ENABLE_MASK`）：

```
block enable before dmac-func=0xfffd0000 dmac-clk=0x1f9f0000
                    cmac-func=0xf000003f cmac-clk=0x4000003f
```

四个 DLE 快照（`pre-ofld` / `post-ofld` / `post-chlist` / `mid-sweep` /
`post-sweep`）全部变成：

```
empty0=0x07ff079f empty1=0x001d003d qempty4=0x000fffff pub-pages=0x0
wde-hif=0x0 wde-wlcpu=0x0 wde-pktin=0x0 ple-txpl=0x0 ple-wlcpu=0x0 ple-h2c=0x0
```

对比 run 8–13 的 `empty0=0x07fd039f qempty4=0x000fbfff`、`wde-wlcpu` 逐 dwell
1→7 只涨不落：**页面现在会被消费掉，队列每次回到空**。

主动扫描本身：

- `probe-request packet-offload done-ack return=0x0`
- `active scan probe request bytes=0x2a sa=84fc1406797b`
- `active scan TX state after … mactx=0x19`（before 为 0）
- `passive scan RX … probe-rsp=0x1 … bss=0x1 channel=0xd`
- `passive scan RX addr rsp-self=0x1`，且该帧 `RXD6=0x0614fc84 RXD7=0x00007b79`
  → A1 = `84:fc:14:06:79:7b`，就是本机 MAC：**收到的是发给自己的单播
  Probe Response**，它只可能来自一次真实辐射出去的 Probe Request
- `RTL8852BS2 active scan probe response complete`（不再是 `error=0x3d`）

`--require-runtime-scanofld-active` 的四条子判据（packet-offload done-ack、
完成行、`probe-id=0x0` 的主动信道表、窗口内 `probe-rsp>=1`）全部满足，
其余 25 项启动期标记也都满足（`missing` 为空才会进到最后一步）。

#### 8. run 14 唯一的失败是 host 侧超时，不是驱动回退

```
FAIL: timed out waiting for b'nsh>': wapi pscan wlan0
```

`--wlan0-scan-timeout` 默认 60 s。从 `wapi pscan` 回显到日志结尾一共又打了
660 286 字节，115200 波特下光是把字符送出来就要 57.3 s——控制台全程饱和。
日志最后两行已经是 `passive scan C2H channel=0x0d reason=0x5`（扫描结束）和
`passive scan report bytes=0x1c`，只差那句 `wlan0 sweep ret=0 end=1 bss=N` 没来得及打。
`verify_wlan0_scan()` 在所有启动期判据之后才跑（`run_k1_wireless_smoke.py:1507`），
所以这次失败不影响前面 25 项的结论。run 15 用 `--wlan0-scan-timeout 300` 重跑。

这也说明：诊断 profile 的 `K1 Wi-Fi SDIO:` 逐命令打印（9 MB 日志里的绝大部分）
已经成为板上运行的时间瓶颈，后续要把它按需关掉。

#### 9. host 工具的两个超时缺陷（已修）

修好 DLE 时钟之后，扫描真的开始收发帧，控制台输出从 9 MB 涨到 10.3 MB，两个一直存在
但从未被触发的 host 侧缺陷立刻暴露：

1. `--wlan0-scan-timeout` 默认 60 s 不够（见上一节）。
2. **`min(0.2, deadline - time.monotonic())` 在 deadline 过期后会变成负数**，
   `select.select()` 直接抛 `ValueError: timeout must be non-negative`，
   把本该是「timed out waiting for the NSH prompt: …」的干净报错变成 traceback。
   run 15 就是这样死的：boot 阶段 10.3 MB 的输出在 115200 下要 ~893 s，刚好越过
   `--boot-timeout 900`。`tools/run_k1_wireless_smoke.py` 7 处 +
   `tools/load_k1_xmodem.py` 2 处已全部改为
   `max(0.0, min(0.2, deadline - time.monotonic()))`。

结论：**板上运行现在是控制台带宽受限的**，`--boot-timeout` 要给到 2400 s，
下一步应把 `K1 Wi-Fi SDIO:` 逐命令打印做成可关的（它占日志 90% 以上）。

#### 10. run 16：26 项验收全部通过

`--boot-timeout 2400 --wlan0-scan-timeout 300`，`--nsh-reboot`，仍是 RAM-only。
最后两行是：

```
PASS: K1 wlan0 passive scan reported 3 BSS (data-only=1 dropped=0):
      56:4f:3b:e2:e6:d2 ch1, 50:4f:3b:e2:e6:d2 ch1, e0:45:6d:10:71:1f ch13
PASS: K1 wireless RAM image reached NSH
```

后一行在 `run_k1_wireless_smoke.py` 里位于所有 requirement 之后、`return 0` 之前，
任何一项失败都会先抛 `XmodemError`，所以它的出现就是 **26 项全过**的证明。
`wlan0 sweep ret=0 end=1 bss=3 data-only=1 dropped=0`；全程 `bring-up failed`、
`wlan0 register error=`、`runtime RX read error=` 各 0 次。

第 26 项的四条子判据在 run 16 里逐条齐全：

| 子判据 | run 16 实测 |
| --- | --- |
| Probe Request 报文卸载 | `packet-offload done-ack return=0x0`（id=0x0, bytes=0x2a） |
| 主动信道表 | `channels=1-13 sequence=5 probe-id=0x0`（前后两次被动表仍是 `probe-id=0xff`） |
| 完成行 | `RTL8852BS2 active scan probe response complete` |
| Probe Response 实收 | `probe-rsp=0x1`、`rsp-self=0x1`，A1=`84:fc:14:06:79:7b`=本机 |

`rsp-self` 是这里唯一不可伪造的一项：一帧**发给本机 MAC 的单播 Probe Response**
只可能是某个 AP 对一次真实辐射出去的 Probe Request 的回应。run 14 与 run 16 两次独立
运行都拿到了它，四个 DLE 快照也都回到全空。

主动扫描不再是失败项。下一步的收尾工作与优先级见
`docs/CLAUDE_HANDOFF_K1_WIRELESS.md`。

### 2026-08-30（续十四）：关掉 SDIO 逐命令打印，并补齐 `spatial_reuse_init()`

#### 1. 板上一轮从 ~15 分钟降到 70 秒

run 16 的完整验收日志是 8,487,502 bytes，其中 163,124 行 `K1 Wi-Fi SDIO:` 占
8,306,377 bytes，**97.9%**。按 115200 baud 算，这些字节本身就要十几分钟传完，而
run 14 与 run 15 两次"失败"最后都查明是这条带宽造成的主机侧超时假象，不是驱动回退。

统计确认这 163,124 行**全部**来自 `k1_sdio_sendcmd()` 里 `if (trace)` 那一个块：

| 标签 | 行数 |
| --- | --- |
| `command launch cmd/register/present` | 14,389 × 3 |
| `data cmd/arg/mode/command/block size/block count/present/host` | 10,889 × 8 |
| `data ADMA address/descriptor/data` | 10,889 × 3 |
| 其余（`CMD53 block count`、`APMU AXI`、`CMD5` 探测等） | 179 |

失败路径不在这个块里：`command busy`、`command error`、`command status`、
`command timeout`、`data error status` 都在 `if (trace)` 之外无条件打印，所以关掉它
不会丢任何指示故障的输出。26 条验收判据也没有一条读 `K1 Wi-Fi SDIO:` 前缀。

因此新增 `CONFIG_K1_SDIO_WIFI_COMMAND_TRACE`（`default n`，依赖
`K1_SDIO_WIFI && K1_EARLY_BOOT_LOG`），`k1_sdio_sendcmd()` 在未开启时直接
`trace = false`。原来的运行期 `k1_sdio_wifi_suppress_command_trace()` 与 GPL 文件里
9 对 `true…false` 调用都保留，开启该配置后行为与以前完全一致，
`tools/decode_rtl8852_sdio_trace.py` 仍然可用。选编译期开关而不是"永久运行期抑制"是
因为 `scanofld_passive_wait` 和 `medium_access_log` 这两对调用跑在扫描期间，它们尾部的
`suppress(false)` 会把抑制重新打开；编译期关掉就不存在这个覆盖问题。

顺带修掉一个潜在编译问题：`command_trace_suppressed` 字段在
`#ifdef CONFIG_K1_SDIO_WIFI` 里，而原来 `k1_sdio_sendcmd()`（eMMC 也走这条路）
无条件引用它。

#### 2. run 17（同一 26 项，`--nsh-reboot`，RAM-only）

| 指标 | run 16 | run 17 |
| --- | --- | --- |
| 日志字节 | 8,487,502 | 189,453 |
| `K1 Wi-Fi SDIO:` 行 | 163,124 | 179 |
| `K1 Wi-Fi GPL:` 行 | 1,178 | 1,172 |
| 整轮墙钟（含重启、XMODEM 上传 369 KB、启动、全部诊断、`wapi pscan`） | ~15 分钟 | **70 秒** |
| 判据 | 26/26 PASS | 26/26 PASS |

`FAIL:` 零次。`block enable before dmac-clk=0x1f9f0000` 说明续十三的时钟修复仍在生效。
`wlan0 sweep ret=0 end=1 bss=2 data-only=2 dropped=0`：这一轮只听到 2 个 AP（run 16 是
3 个），是环境差异；`bss=` 只统计有 Beacon 或 Probe Response 支撑的条目，
`data-only=` 是**另外**被排除掉的、只在 data frame 里出现过的 BSSID
（`k1_rtl8852bs_runtime_scanofld_export_result()`），所以判据没有被放宽。

#### 3. `dmac_init()` / `cmac_init()` 剩余步骤的逐项审计

只读原厂源码，不占用上板机会。结论：

| 原厂步骤 | 端口状态 |
| --- | --- |
| `dle_init` / `hfc_init` / `sta_sch_init` | 已实现 |
| `preload_init` | 8852B 上 `preload_init_set_8852b()` 立即返回成功，空操作 |
| `mpdu_proc_init` | 已实现，四个写入逐值相同（`0x02a95a95`、`0x0000aa55`、`0x010e05f0`、`MPDU_PROC` 的 `APPEND_FCS｜A_ICV_ERR`） |
| `sec_eng_init` / `sec_info_tbl_init` | **未改编**。函数体不在本地原厂缓存里；只影响加密，open-system 认证不需要，留给 WPA2 四次握手阶段 |
| `scheduler_init` / `addr_cam_init` / `rx_fltr_init` / `cca_ctrl_init` / `nav_ctrl_init` | 已实现 |
| `rst_port_info` | 纯主机侧 `PLTFM_MEMSET`（`adapter->port_info`、`bcn_rpt_stats`），没有寄存器写，端口没有对应结构，**无需实现** |
| `spatial_reuse_init` | **本次补上**，见下 |
| `tmac_init` / `trxptcl_init` / `rmac_init` / `cmac_com_init` / `ptcl_init` / `cmac_dma_init` | 已实现；下面四个寄存器的缺失都已确认不适用 |

四个原厂写到、端口没写的寄存器，逐个排除：

- `R_AX_SIFS_SETTING 0xC624`、`R_AX_PTCL_FSM_MON 0xC6E8`：只在 PCIe/SW 模式有效（续十二已记录）
- `R_AX_RX_TIME_MON 0xCEEC`：在 `#if MAC_AX_8852C_SUPPORT || 8192XB || 8852D` 里，
  并且外层 `is_chip_id()` 只匹配 8852C/8192XB/8852D，**8852B 走不到**
- `R_AX_AGG_LEN_VHT_0 0xC618`：在 `_patch_vht_ampdu_max_len()` 里，受
  `chk_patch_vht_ampdu_max_len()` 条件保护，只改 VHT AMPDU 最大长度，与扫描/认证/关联无关
- `R_AX_DLK_PROTECT_CTL 0xCE02`：其实**已覆盖**——它是 `R_AX_RCR 0xCE00` 的高半字，
  端口用 `{RCR, 0xfff20000, 0x20f20000}` 一条 masked update 写掉了

#### 4. `spatial_reuse_init()` 的两个字段

原厂 `spatial_reuse.c:133` 只有两个字节写：`R_AX_RX_SR_CTRL 0xCE4A` 清
`B_AX_SR_EN BIT(0)` 与 `B_AX_SR_CTRL_PLCP_EN BIT(1)`；`R_AX_BSSID_SRC_CTRL 0xCE4B`
置 `B_AX_PLCP_SRC_EN BIT(0)`。两者与 `R_AX_MACID_MATCH 0xCE48` 同在一个 32-bit 字里，
端口没有 8-bit MAC 访问器，所以合成一条 masked update 加进
`g_k1_rtl8852bs_runtime_mac_core_fields[]`（放在 `tmac_init` 组之前，与原厂
`cmac_init()` 的顺序一致）：

```c
#define K1_RTL8852BS_MACID_MATCH             0xce48u
#define K1_RTL8852BS_SPATIAL_REUSE_MASK      0x01030000u
#define K1_RTL8852BS_SPATIAL_REUSE_VALUE     0x01000000u
```

`mask` 只盖 bit16（`SR_EN`）、bit17（`SR_CTRL_PLCP_EN`）、bit24（`PLCP_SRC_EN`），
所以 `B_AX_SRG_CHK_EN`、`B_AX_SR_OP_MODE`、`BSSID/BSS-colour/partial-AID match` 以及
低两个字节的 MACID match 都按原厂的样子留着不动。它和扫描能不能工作无关（扫描在没有它
的时候已经通过），补上只是为了减少与原厂的偏差，并让后续关联阶段不必再回来查这一条。

#### run 18 的主动扫描失败是环境抖动，不是 `spatial_reuse_init` 引起的回归

带上面这条改动的第一轮（run 18）在三项上失败：

```
FAIL: NuttX started without: RTL8852BS2 active scan completion,
      RTL8852BS2 Probe Response RX, successful K1 RTL8852BS2 bring-up
K1 Wi-Fi GPL: active scan probe response error=0x000000000000003d
ERROR: K1 RTL8852BS2 bring-up failed: -61
```

`0x3d` = 61 = `ENODATA`，是 `scanofld_passive_diagnostic_common(false, true, …)`
在「Probe Request 发出去了、但没有一帧 A1 指向本机的 Probe Response」时的返回值。
按「每批失败立即停、先分清确定性缺陷与环境抖动」的规矩，**先用同一个二进制原样重跑**
（不重新编译，整轮 70 秒），得到 run 19：

```
K1 Wi-Fi GPL: RTL8852BS2 active scan probe response complete
PASS: K1 wireless RAM image reached NSH
PASS: K1 wlan0 passive scan reported 3 BSS (data-only=1 dropped=0):
      64:13:ab:db:f6:28 ch11, 56:4f:3b:e2:e6:d2 ch1, 50:4f:3b:e2:e6:d2 ch1
```

同一镜像通过，所以 run 18 不是 `spatial_reuse_init` 造成的回归。另有四条独立证据
指向同一结论，记录下来以免下次重复排查：

| 证据 | run 17（通过） | run 18（失败） | 结论 |
| --- | --- | --- | --- |
| `MAC core field=` 回读报错 | 无 | 无 | 0xce48 的 masked update 写进去并回读一致，index 4 通过 |
| 固件发送通知 | `pre-tx=0xd post-tx=0xd post-tx-fail=0 fw-txfail=0` | 同上，逐字段相同 | Probe Request 在 13 个信道上都真的发了，固件没报一次发送失败 |
| 被动扫描接收量 | `beacon=0x13 mgmt=0x15 bss=0x1` | `beacon=0x12 mgmt=0x13 bss=0x1`（同一个 BSSID `50:4f:3b:e2:e6:d2`） | 接收机健康，收得并不比通过的那轮少 |
| `rsp-self` / `rsp-other` | `0x2 / 0x0` | `0x0 / 0x1` | 空中确实有 Probe Response，只是发给别的 STA |

最后一行不能反过来读成「自己的回复被误判成别人的」：`rsp-self`/`rsp-other` 是
**host 侧软件**按收到帧的 A1 与本机 MAC 逐字节比较分出来的
（`k1_rtl8852bs_gpl.c:11980` 附近的注释与计数器），没有任何寄存器能把发给自己的单播
算到 `rsp-other` 上去。加上 `SR_EN` 本来就是被清掉（原厂 `cmac_init()` 也清），
`PLCP_SRC_EN` 只是在 spatial reuse 关闭时给它选 BSS color 来源，对 A1 过滤与
Probe Response 接收都不产生作用。

因此这条改动保留。主动扫描依赖「dwell 期间 AP 真的回一帧」，本来就有环境抖动的余量：
run 16 听到 3 个 AP、run 17 是 2 个、run 18 一个都没回、run 19 又是 3 个。
`--require-runtime-scanofld-active` 回归时的第一步永远是**原样重跑一次**，
确认是确定性缺陷之后再动代码。


### 增量 3a：向真实 AP 发 Authentication Request 并收到它的回复

这是本移植第一次把帧发给**单个对端**，也是第一次要求一个「只有接受了我们请求的 AP
才会发出」的回复。实现分五块，都在 `chip/k1/k1_rtl8852bs_gpl.c`：

1. `k1_rtl8852bs_runtime_auth_request_build()`：24 字节 802.11 头 + 6 字节 body
   （alg=0 open system、seq=1、status=0）。A1/A3 = AP 的 BSSID，A2 = eFuse 自身 MAC；
   frame control `0x00b0`（type=management, subtype=11）。**序列控制字段当时留了 0，
   说「由描述符负责」——这是错的，见下面增量 3b 里的根因**：帧里的这个字段就是上空气的
   那个，描述符里的 `AX_TXD_HW_SSN_SEL`/`AX_TXD_EN_HWSEQ_MODE`（能让硬件代填的两位）
   在本移植里都是 0。
2. `k1_rtl8852bs_runtime_mgmt_tx_build()` 增加 `bool broadcast` 参数。原来它硬写
   `info1 = K1_RTL8852BS_MGMT_TXI_BMC`，因为唯一调用者发的是广播 Probe Request。
   **单播管理帧必须清掉这个位**，否则硬件既不会等 AP 的 ACK，也不会在 ACK 缺失时重传。
   现有的 Probe Request 调用点传 `true`，行为不变。
3. `k1_rtl8852bs_runtime_mgmt_tx_frame()`：精简发送核心。描述符、固定地址 CMD53、
   页计数排空轮询都和已经被证明能穿到 TMAC 并真辐射的
   `k1_rtl8852bs_runtime_mgmt_tx_probe()` 完全一致，只是把十几行仪表输出去掉、
   把状态返回而不是打印——因为它跑在 scan-offload 的 dwell 里面，控制台是轮询式的，
   在 250 ms 的窗口里打印十行会吃掉相当一部分等回复的时间。
4. 发送时机挂在**进入信道通知**上（`k1_rtl8852bs_runtime_scanofld_passive_match()`
   里 `match->dwell_pending = true` 之后）。这一条是整个增量的关键：

   > SCANOFLD holds the current channel until this FW_OFLD/SCANOFLD_DRV_CTRL/NEXT_CH
   > command is received.  （`k1_rtl8852bs_gpl.c:9563` 自带注释）

   **这句注释只在 `period` 没到之前成立，当时的文字漏了这一半。** 原厂
   `mac_ax_scanofld_chinfo` 对两个时间字段的定义（`mac_def.h:7374`，vendor 缓存）是：
   `period` = "how long to stay on this ch. unit: ms"，`dwell_time` = "dwell time if
   recv bcn. unit: ms. set 0 to disable dwell"。也就是说固件停在一个信道上**最多**
   `period` 毫秒，到点它自己就走，host 的 NEXT_CH 只能把这段时间**缩短**。`period` 是
   `u8`，上限 255 ms。

   所以 host 侧的 dwell 截止必须**严格小于** `period`，否则每个信道都是一场 host 可能
   输掉的竞争：host 的计时从它**读到**进入信道通知才开始，而固件的计时从它进入信道就
   开始。本移植现在是 `K1_RTL8852BS_SCAN_OFLD_PASSIVE_PERIOD_MSEC` = 250 ms（写进
   chinfo 交给固件）配 `K1_RTL8852BS_SCAN_OFLD_PASSIVE_DWELL_MSEC` = 180 ms（host 自己
   的截止），留出的 70 ms 要覆盖通知的读取延迟加上一个 dwell 能打印的控制台输出，剩下的
   仍然跨过一个以上的 100 ms beacon interval。

   在这个前提下，从进入信道通知到 host 的截止之间，射频**确定**停在目标 AP 的信道上，
   既能发也能在同一信道收到回复。这是原厂状态机自己的行为，不是绕过它的 hack——
   本移植还没有 `rtw8852b_set_channel_{mac,bb,rf}`，这是目前唯一能在指定信道发送的办法。
5. `k1_rtl8852bs_fwdl_runtime_auth_diagnostic()`：跑两轮 sweep。第一轮普通被动扫描，
   只为选目标——取**收到 Beacon 最多**的那个 AP（`bss[i].channel` 已经实现了
   「优先 DS Parameter Set 里的信道，否则用 dwell 信道」的偏好）；然后
   `auth_arm(bssid, channel)`，第二轮 sweep 在进入该信道时发出请求，同一轮 sweep 的
   RX drain 负责观察回复；最后 disarm 并打印报告。

判据是**发给本机的 Authentication 帧**：`k1_rtl8852bs_scanofld_observe_wifi()` 里新增的
观察块只在 `A1 == 自身 MAC` 时才计入 `rsp-self` 并解析 alg/seq/status，而这个比较和
Probe Response 的那个一样是 **host 侧软件逐字节 memcmp**，没有任何寄存器能让别的 STA
的帧算成给我们的回复。sweep 期间接收过滤是放开的（嗅探模式），所以必须靠 A1 判定。

**这一步不是关联，也不声称链路建立。** ADDR_CAM 里仍然是 no-link role。能**看到**
AP 的 Authentication Response 就是这一步的成果；Association 属于增量 3b（用 AP 的 BSSID 更新 ADDR_CAM/role：
`struct k1_rtl8852bs_addr_cam_info_s` 里的 `network_type`/`self_role`/`bssid`/`aid`
已经就位，现在只填了 `self_mac`）。仍然是 RAM-only：不装密钥、无数据通路、无 IP、
不动 wlan0 行为，也不写 eMMC/SPI flash/eFuse/U-Boot 环境变量。

新增开关与产物：`CONFIG_K1_RTL8852BS2_RUNTIME_AUTH_DIAGNOSTIC`（依赖
`..._SCAN_OFLD_ACTIVE_DIAGNOSTIC`）、profile
`board/k1/muse_pi_pro/configs/wireless_auth_diag/`、构建脚本 `tools/build_k1_auth.sh`、
harness 判据 `--require-runtime-auth`（同时检查目标 BSSID 非零、发送 `status=0x0`、
报告行里 `req=` 与 `rsp-self=` 均非零、以及完成行）。

#### run 20：AP 回了 Authentication Response，status=0（成功）

镜像 `wireless_auth_diag`，`tools/build_k1_auth.sh`（`BUILD_EXIT=0`，只有 6 条既有的
`defined but not used` 警告，新代码零警告），`--nsh-reboot` 两阶段恢复，无需按 RST。
27 条 `--require-*` 全通过（上一轮 26 条 + 新增的 `--require-runtime-auth`），
`RUN_EXIT=0`，基线没有回归：

```
K1 Wi-Fi GPL: auth target bssid=504f3be2e6d2 channel=0x1 beacons=0x7 ssid-len=0x2
K1 Wi-Fi GPL: auth request tx channel=0x1 bytes=0x1e status=0x0
K1 Wi-Fi GPL: auth req=0x1 tx-status=0x0 frames=0x1 rsp-self=0x1 rsp-target=0x1
              alg=0x0 seq=0x2 status=0x0 a2=504f3be2e6d2
K1 Wi-Fi GPL: RTL8852BS2 authentication response complete
PASS: K1 wlan0 passive scan reported 3 BSS (data-only=3 dropped=0):
      ea:12:2d:f6:42:46 ch11, 56:4f:3b:e2:e6:d2 ch1, 50:4f:3b:e2:e6:d2 ch1
PASS: K1 wireless RAM image reached NSH
```

逐字段读这份报告：

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `bytes` | `0x1e` = 30 | 24 字节头 + 6 字节 body，帧长与构建函数一致 |
| `tx-status` | `0x0` | 描述符建好、管理队列有页、CMD53 被接受、页计数排空 |
| `frames` | `0x1` | sweep 期间一共看到 1 个 subtype=11 的管理帧 |
| `rsp-target` | `0x1` | 它的 A2 是我们选的那个 BSSID |
| `rsp-self` | `0x1` | 它的 A1 是本机 eFuse MAC——host 侧 memcmp 判定 |
| `alg` / `seq` / `status` | `0x0` / `0x2` / `0x0` | open system、Authentication **Response**、**成功** |
| `a2` | `504f3be2e6d2` | 与目标 BSSID 逐字节相同 |

`status=0` 是 AP 自己给出的成功码：它收到了我们的 Authentication Request，
接受了这次 open-system 认证，并把结果发回给本机 MAC。这条链路上第一次出现
「只能由接受了本机请求的那台设备产生」的证据，之前的 Probe Response 只证明请求被辐射了。

**仍然要按实际情况报告**：这不是关联，也不是链路。下一步（增量 3b）是用 BSSID 更新
ADDR_CAM 与 role 并告诉固件本机已加入这个 BSS，之后才是 Association
Request/Response。Wi-Fi 整体依然未完成：没有四次握手、没有数据通路，蓝牙也只到
HCI open。

> **原来这里写的「硬件没有 ACK 这一帧，AP 会重传若干次然后把交换超时掉」是没有证据的
> 推测，已删除。** 增量 3b 的 run 25 给出了相反的证据：每一次**收到回复**的交换，
> `TX PPDU` 的 `lcck` 恰好 +2（请求一个，多出来的一个只能是本机发的），而 run 24 里
> 三次**没收到回复**的交换每次只 +1。run 25 的 prejoin 那一次是在两条 join 命令之前、
> ADDR_CAM 还是 no-link 时发生的，同样是 +2。计数器只数 PPDU、不给帧类型，所以这是很强
> 的相关而不是解码；但「硬件不 ACK」这个说法与它冲突，不能再当结论用。

### 增量 3b：把「本机已加入这个 BSS」告诉固件和硬件（JOININFO + ADDR_CAM）

提交 `20c3191`。开关 `CONFIG_K1_RTL8852BS2_RUNTIME_JOIN_DIAGNOSTIC`（依赖
`..._RUNTIME_AUTH_DIAGNOSTIC`）、profile
`board/k1/muse_pi_pro/configs/wireless_join_diag/`、构建脚本 `tools/build_k1_join.sh`、
harness 判据 `--require-runtime-join`。板上 run 25
（`out/k1-serial/k1-join-20260830T101829Z.log`）**28 项 `--require-*` 全通过**，
比增量 3a 的 27 项只多了这一条，基线没有回归。

这一步做的事情，和它**没有**做的事情要分清：它把原厂 connect 路径发的两条命令按原厂的
顺序和内容发出去，然后要求同一次交换在固件和硬件都相信「本机是这个 BSS 的 station」
之后**仍然成立**。它不是关联：没有 Association Request，没有 AID（报告里的 `aid=0x0`
是真值不是占位），没有装密钥，没有创建网络设备，没有数据通路——port 0 依旧读到
`PORT_FUNC_EN=0`、`NET_TYPE=0`，因为 `mac_port_init()` 还没移植。
（这里原来还写了「port 的 TSF 也没有和 AP 的 Beacon 同步 …… TSF 冻结」。**那句是错的**：
增量 3g 证明 TSF 在 `PORT_FUNC_EN=0` 时就已经跟着 AP 走了，见该节「更正」。）

三段实现都在 `chip/k1/k1_rtl8852bs_gpl.c`：

1. **选目标**：先跑一轮普通被动 sweep，用和增量 3a 相同的办法挑收到 Beacon 最多的 AP。
   BSSID 只有在**真正被听到过**之后才允许写进硬件，而且只有 Beacon 或 Probe Response
   算听到过。报告行里的 `proven=0x1` 记录的是这个目标就是 AP 亲自回过请求的那一个。
2. **两条命令**：`MEDIA_RPT/JOININFO`（MACID、infrastructure 网络类型、client
   self role、band 0、port 0）在前，同一个 MACID 的 `MAC/ADDR_CAM_UPDATE`（target
   MAC 与 BSSID 都是这台 AP，self role 为 client）在后。两条都要 done ack，且返回值都
   必须为 0。**`FWROLE_MAINTAIN` 故意不重发**：原厂 connect 路径也不重发，它保留
   bring-up 时建立的固件 role，只改内容。
3. **重入约束**：两次提交都放在**两轮 sweep 之间**，绝不在扫描 RX 循环里做——
   `k1_rtl8852bs_runtime_done_ack_wait()` 会抽同一个 RX FIFO，在 RX 循环里调它会把
   扫描自己要收的帧吃掉。`k1_rtl8852bs_runtime_mgmt_tx_frame()` 则可以从 RX 循环里调。

判据为什么要连 Beacon 计数一起要求：把一个 BSSID 和 infrastructure 网络类型写进
ADDR_CAM，正是那种可能悄悄开始过滤接收的改动。一次「报告 join 成功、同时其实已经收不到
东西了」的运行比直接失败更糟，所以 `join bss=`/`*-beacons=` 是必须项而不是诊断。

#### run 24 的失败与根因：host 自建的管理帧一直用序列号 0

run 24（`out/k1-serial/k1-join-20260830T095957Z.log`）以 `station join error=0x3d`
（ENODATA）失败：三次交换的 `frames=0x0`——AP 一次都没回。而同一轮里增量 3a 的那次交换
（同一个 AP、同一个信道、几十毫秒之前）拿到了 `rsp-self=0x1 status=0x0`。

先用板上仪表把发送侧和接收侧都排除掉：

- 三次交换每次 `delta-mactx-mpdu=0x1 delta-mactx-dma=0x1 delta-lcck=0x1`，
  `macid-pause=0`、`macid-sleep=0`、`cmac-drop=0`、`dmac-drop=0`——请求**确实上了空气**。
- 每次发送之后紧接着记录到的就是目标 AP 的 Beacon（`prejoin-beacons=0x9`，
  bss 5/4/5）——射频在信道 1 上、也听得见这台 AP。
- sweep 期间接收过滤是放开的，日志里连**别的** station 的单播 QoS data 和一帧别的
  station 的 Deauthentication 都收到了——如果 AP 回了给本机的帧，一定会被收到。
- 没有任何一帧 Deauthentication 是发给本机的。

剩下的解释只能是 AP **不回重复的请求**，而移植自己的代码写明了为什么：
`k1_rtl8852bs_runtime_auth_request_build()` 当时的注释和实现都是「序列控制字段留 0，
由描述符负责」，`..._auth_transmit()` 也把 `0u` 当描述符序列号传下去。于是一次运行里
每一个 Authentication Request 都呈现同一个 `<address 2, 序列号, 分片号>` 三元组，
而这正是 IEEE 802.11 clause 10.3.2.14 重复检测的输入：接收方可以缓存最近收到的这些
三元组，并在 **MAC 层**（在那个本来会回答的状态机**下面**）丢掉重复帧——帧被 ACK 了，
然后被丢掉。所以一台永远发序列号 0 的 station，每个 AP 只会回它一次，之后就是沉默。

「描述符负责」这句本身也是错的：帧里的序列控制字段就是上空气的那个，而描述符里能让
硬件代填序列号的 `AX_TXD_HW_SSN_SEL` / `AX_TXD_EN_HWSEQ_MODE` 两位在本移植里都是 0
（原厂 `trx_desc_8852b.c:214-255` 里 dword0 的这两位，以及 dword3 的
`SET_WORD(info->sw_seq, AX_TXD_WIFI_SEQ)`）。

修法：`g_k1_rtl8852bs_mgmt_sequence` 一个 12 位的**本机**计数器，
`k1_rtl8852bs_runtime_mgmt_sequence_next()` 每次发送取一个，同时写进帧的序列控制字段
（`frame + 22`，`sequence << 4`，分片号 0）和 WD BODY dword3，让硬件的记账和真正辐射
出去的内容一致。号码**不管发送路径是否接受都要消耗掉**：被拒的请求也可能已经进了 FIFO，
而上过空气的号码绝不能重用。广播 Probe Request 不从这个计数器取号——它一轮只发一次，
靠地址而不是序列号被回答。顺带一个佐证：原厂 chinfo 里有
`rand_seq_num` = "enable random seq num for probe req"，固件自己也在管这件事。

#### run 25：三次交换用了三个不同的序列号，三次都被回答

同一个镜像（`tools/build_k1_join.sh` 全部门禁通过，ELF SHA256
`c680a4b4a774acc3ebce2b74b9acf041486699be9feafcfda2ea024719e176ad`，
`text 739542 data 9768 bss 24656`），`--nsh-reboot`，RAM-only：

```
K1 Wi-Fi GPL: auth target bssid=564f3be2e6d2 channel=0x1 beacons=0x9 ssid-len=0x0
K1 Wi-Fi GPL: auth request tx channel=0x1 bytes=0x1e sn=0x0 status=0x0
K1 Wi-Fi GPL: auth req=0x1 req-sn=0x0 tx-status=0x0 frames=0x1 rsp-self=0x1
              rsp-target=0x1 alg=0x0 seq=0x2 status=0x0 a2=564f3be2e6d2
K1 Wi-Fi GPL: RTL8852BS2 authentication response complete
K1 Wi-Fi GPL: join target bssid=564f3be2e6d2 channel=0x1 beacons=0x9 cap=0x421 proven=0x1
K1 Wi-Fi GPL: auth request tx ... sn=0x1 status=0x0
K1 Wi-Fi GPL: prejoin auth req=0x1 req-sn=0x1 ... rsp-self=0x1 ... status=0x0
K1 Wi-Fi GPL: join info done-ack return=0x0
K1 Wi-Fi GPL: auth request tx ... sn=0x2 status=0x0
K1 Wi-Fi GPL: joininfo auth req=0x1 req-sn=0x2 ... rsp-self=0x1 ... status=0x0
K1 Wi-Fi GPL: join CAM done-ack return=0x0
K1 Wi-Fi GPL: auth request tx ... sn=0x3 status=0x0
K1 Wi-Fi GPL: join auth req=0x1 req-sn=0x3 ... rsp-self=0x1 ... status=0x0
K1 Wi-Fi GPL: join step prejoin-bss=0x6 prejoin-beacons=0x8 prejoin-rsp=0x1
              joininfo-bss=0x7 joininfo-beacons=0x8 joininfo-rsp=0x1
              cam-bss=0x6 cam-beacons=0x2 cam-rsp=0x1
K1 Wi-Fi GPL: join bss=0x6 network-type=0x2 aid=0x0 bssid=564f3be2e6d2
K1 Wi-Fi GPL: RTL8852BS2 station join complete
```

四次交换（3a 那次加 join 的三次）的序列号是 0/1/2/3，四次都拿到
`rsp-self=0x1 status=0x0`。这同时说明两件互相独立的事：序列号确实是 run 24 的根因；
以及两条 join 命令都没有破坏这次交换——**命令之前**（prejoin）、**两条之间**
（joininfo）、**两条都发完之后**（join）各一次，三次都被回答，三次的 sweep 也都还在数
Beacon 和 BSS。

`TX PPDU` 的 `lcck` 在这一轮是 `0x0 → 0x2`（主动扫描）`→ 0x4`（3a）`→ 0x6`（prejoin）
`→ 0x8`（joininfo）`→ 0xa`（join）：**每一次被回答的交换恰好 +2**，而 run 24 里每一次
没被回答的交换只 +1。多出来的那一个 PPDU 只能是本机发的，最经济的解释就是硬件对收到的
Authentication Response 回了 ACK；而且 prejoin 那次是在两条命令之前、ADDR_CAM 还是
no-link 时发生的，同样 +2。计数器只数 PPDU 不给帧类型，所以这是很强的相关而不是解码。

#### 顺便定下来的几件事

- **和原厂顺序的差异（还没改，3c 要处理）**：原厂/mainline 是在**认证之前**发
  `JOININFO` 且 `dis_conn=true`，只在**关联时**才翻成 `dis_conn=false` 并把 port 设成
  INFRA、调 `mac_port_init()`。本增量为了让「命令前/命令后」形成对照，把
  `disconnected=false` 提前到了认证之后关联之前，这在原厂语义上是不对的，属于已知欠账。
  `_hal_stainfo_to_macrinfo()`（vendor `hal_api_mac.c:3094-3180`）可以逐字段核对：
  `opmode = is_connect ? MAC_AX_ROLE_CONNECT : MAC_AX_ROLE_DISCONN`、
  `tsf_sync = rlink->hw_port`、INFRA/NO_LINK 都是 `MAC_AX_SELF_ROLE_CLIENT`，
  NO_LINK 只拷 `self_mac`，INFRA 才拷 `target_mac`/`bssid` 并填 `aid`。
- **`pause_tx_data` 不会挡住管理帧**：原厂定义是 "whether disable tx (except manage
  pkt) after sending probe req"（`mac_def.h:7398`）。曾经怀疑它挡掉了 Authentication
  Request，可以排除。
- **串口日志里 C2H 行的位置是 host 读到它的时间，不是固件产生它的时间**：一行 C2H 出现
  在某两行之间，不能用来推断固件事件的先后。凡是用日志顺序做的因果推断都要先过这一关。
- **TX 仪表的日志前缀已改名**为 `TX state`（11 个 TX PPDU 计数、`R_AX_MACTX_DBG_SEL_CNT`
  的 MPDU/DMA 计数、`ctn-txen`、`ptcl-common`、`macid-sleep`、`macid-pause`、
  `cmac-drop`、`dmac-drop`、`loopback`、`cca-abort`）与 `TX PPDU`，旧日志里的旧前缀不要
  再当成缺失。

### 增量 3c：把一次管理帧交换做成可重复的（parked 信道 + 有界重传），以及关联为什么还没成

提交 `7c1cbe4`。开关 `CONFIG_K1_RTL8852BS2_RUNTIME_ASSOC_DIAGNOSTIC`（依赖
`..._RUNTIME_JOIN_DIAGNOSTIC`）、profile
`board/k1/muse_pi_pro/configs/wireless_assoc_diag/`、构建脚本
`tools/build_k1_assoc.sh`、harness 判据 `--require-runtime-assoc-response` 与
`--require-runtime-assoc`。板上 run 30
（`out/k1-serial/k1-assoc-20260830T120636Z.log`）本 runner 的 30 项 `--require-*`
过了 27 项。**关联没有成**：没过的三项就是 Association Response、关联本身、以及依赖
它们的 bring-up 成功。这一节先说它为什么没成，再说这一增量真正解决掉的是什么。

#### 关联没成的原因：周围没有一个「既广播 SSID 又不加密」的 BSS

run 30 发了 6 个 Association Request（对开放 BSS 3 个，用它 sibling BSSID 广播的
SSID 再发 3 个），`frames=0x0`——AP 一帧都没回。同一轮的 sweep 普查给出了原因：

| BSSID | cap | Privacy | RSN | SSID |
| --- | --- | --- | --- | --- |
| `504f3be2e6d2` | `0x431` | 有 | 20 字节，CCMP+PSK | `SB` |
| `564f3be2e6d2` | `0x421` | 无 | 无 | 长度 0（隐藏） |
| `a639b3663b34` | `0x421` | 无 | 无 | 长度 0（隐藏） |
| `487d2ee005ea` | `0x031` | 有 | 20 字节 | `TP-LINK_05EA` |
| 其余 20 余个 | `0x_31`/`0x_11` | 有 | 20/24 字节 | 有 |

**扫到的每一个广播 SSID 的 BSS 都开了 Privacy 并带 RSN；不开 Privacy 的两个都不广播
SSID。** 关联诊断按「privacy=0x0」挑目标，于是挑中隐藏 SSID 的 `564f3be2e6d2`——它是
旁边那台 AP 的一个隐藏 VAP。Association Request 必须带**这个 BSS 自己的 SSID**
（IEEE 802.11 11.3.5.3），本机不知道它，于是：

- 「advertised」那次带长度 0 的 SSID element：AP 无法匹配，静默丢弃；
- 「sibling」那次带 `SB`（是 `504f3be2e6d2` 的 SSID，不是它的）：不匹配，同样丢弃。

认证不受影响，两次尝试各自都拿到了 `rsp-self=0x1 status=0x0`——**Authentication
Request 不带 SSID**，所以它能成、关联不能成，这两件事并不矛盾，也不是发送侧的问题。

结论很直接：这个射频环境里能走到关联的只有 WPA2-PSK 的 AP（`504f3be2e6d2`，而且它已经
被证明会回本机的认证请求）。要拿到真的 Association Response，请求里必须带上和它 beacon
里一致的 RSN element（CCMP 成对、CCMP 组、PSK AKM），这就是增量 3d 的第一件事；四次
握手和装密钥还在它后面。

#### 这一增量真正解决的：一次交换以前是抛硬币

run 27（`out/k1-serial/k1-assoc-20260830T113232Z.log`）以 `station join error=0x3d`
失败：join 步骤第三次 sweep 的那**一个** Authentication Request 没被回答
（`cam-rsp=0x0 frames=0x0`），而同一台 AP 回答了它前面两次 sweep。一次 180 ms dwell 里
只许发一个请求、只等这一个 dwell，这就是抛硬币。四个改动把它变成可重复的：

1. **parked 信道表**。交换 armed 期间，13 条 scan-offload 信道表项**全部**填目标信道，
   射频就留在能听到回答的那个信道上，而不是发完就走。park 跟着 arm/disarm 走，别的
   sweep 一行都不受影响。日志里是 `passive scan channel-list parked channel=0x1
   entries=0xd`，普查 sweep 仍然打 `channels=1-13`。
2. **parked sweep 要按表项数重新装填 dwell**。原来的门控是「每个信道只装一次 dwell」的
   位掩码，parked 表反复进同一个信道，于是整轮只装了一次，退休之后再没有 dwell 到期、
   再没有 next-channel 命令发出去，固件就一直等——run 28 的 `-110`
   （`enter-mask=0x1 next=0x1 end=0x0 scan-events=0x2`）就是这个。现在 parked 表按 13
   条表项各装一次 dwell，固件走完表并报 SCAN_END，run 29 起 parked sweep 的
   `end=0x1` 正常了。
3. **有界重传**。认证和关联各最多 3 个请求，每个取新的序列号（避免 3b 查明的
   IEEE 802.11 10.3.2.14 重复检测），发送时机是 parked 信道的每一次 enter 通知，条件是
   「还没收到回答」。run 30 里 `sn=0x7,0x8,0x9` 和 `sn=0xd,0xe,0xf` 就是两组三次重传。
4. **armed 期间不许打轮询串口**。中途那次 TX 见证快照实测 65 行 6378 字节，115200 8N1
   下约 550 ms，而 dwell 只有 180 ms，而且它正好花在刚发过请求的那个 dwell 里
   （run 26）。现在它只打一行 `tx witness mid-sweep snapshot suppressed exchange=0x1`，
   逐帧解码转储同样在 armed 期间关掉。

效果：run 30 的 join 步骤三次 sweep **全部**被回答（`prejoin-rsp=0x1 joininfo-rsp=0x1
cam-rsp=0x1`），打出了 `RTL8852BS2 station join complete`；两次关联尝试也各自先拿到了
自己的 Authentication Response。

#### 对 3b 那条更正的再更正：这个固件不会自己换信道

3b 的文档写过「`mac_ax_scanofld_chinfo` 的 `period` 是停留时长，所以固件会自己离开，host
的 NEXT_CH 只能把 dwell 缩短」。字段定义没错，但**这个固件的实际行为不是这样**：

- 至今每一轮 sweep 的每一次 enter 通知，前面紧挨着的都是本机发的 next-channel 命令；
  `fw-next` 在所有运行里始终是 `0x0`。
- run 28 停发命令之后，本机在同一个信道上又等了好几秒——表项里配的 `period` 是 250 ms，
  表还剩 11 条没走——固件既没有 LEAVE 也没有新的 ENTER。

所以：**host 是 sweep 唯一的节拍器**。这和 180 ms 的 host dwell 短于 250 ms 的
`period` 是自洽的（本机总是先抢到），但「固件会自己走」这一条在这个端口上从未被观测到，
凡是依赖它的推理都不成立——parked sweep 停在原地不动就是直接后果。

#### done-ack 等待需要 8 KiB 的 RX 缓冲，512 字节不够

run 29（`out/k1-serial/k1-assoc-20260830T120235Z.log`）在 parked sweep 修好之后立刻换了
一个失败：认证步骤通过了（`RTL8852BS2 authentication response complete`），join 步骤在
JOININFO 的 done-ack 上死掉，`runtime single done-ack wait … error=0x1c`（`-ENOSPC`）。

根因是 parked sweep 的一个副作用：sweep 结束后射频**留在**目标 AP 正在发 beacon 的信道
上，SDIO RX FIFO 里待取的聚合帧于是经常不止一帧。`k1_rtl8852bs_runtime_rx_read()` 对
「请求长度 > 缓冲区」的处理是**不消费、返回 `-ENOSPC`**（这是对的，FIFO 必须整块读），
于是 done-ack 等待第一次读就失败，之后每一次读都失败同一个长度。

`K1_RTL8852BS_RUNTIME_DONE_ACK_RX_MAX` 从 512 改成 8192，和 sweep 自己的读缓冲一样大
（sweep 至今 `oversize=0x0`，从没报过超限），并且把 `-ENOSPC` 时设备宣告的长度一起打进
错误行（`length=`），以后要再调大就有数据可依。

#### run 27 → 30 的账

| run | 结果 | 根因 |
| --- | --- | --- |
| 27 | `-61`，join `error=0x3d` | 一次 sweep 只发一个请求，第三次 sweep 没被回答 |
| 28 | `-110`，认证步骤超时 | parked 表只装了一次 dwell，host 停发命令、固件干等；**认证其实成功了**（`rsp-self=0x1 status=0x0`），是 sweep 的超时把它盖掉了 |
| 29 | `-28`，join `error=0x1c` | done-ack 512 字节缓冲遇上 parked 之后的多帧聚合，`-ENOSPC` |
| 30 | `-61`，关联 `error=0x3d` | 环境里没有「广播 SSID 且不加密」的 BSS，Association Request 无法带对 SSID |

run 28 那件事值得单独记：修好之前它的 `-110` 让人以为认证没成，其实 `auth req=0x1 …
rsp-self=0x1 rsp-target=0x1 alg=0x0 seq=0x2 status=0x0 a2=504f3be2e6d2` 就在同一份日志
里。**一次成功的交换被一个还没结束的 sweep 的返回值盖掉**，这类错误比失败本身更难看见，
所以 3d 之前先把它写下来。当时还怀疑过是新加的重传没有触发；反汇编
（`riscv-none-elf-objdump` 看 `k1_rtl8852bs_runtime_scanofld_passive_match`）确认两个
`jal` 调用点都在、守卫条件逐条对得上，而重传的守卫要求「还没收到回答」——回答已经在第一
个 dwell 里到了，所以它**本就不该**触发。

#### 下一步

增量 3d：给 Association Request 加 RSN element（version 1、组密码 CCMP、成对密码 CCMP、
AKM PSK、RSN capabilities 0），目标改成已经被证明会回认证请求的 WPA2-PSK AP
`504f3be2e6d2`；`K1_RTL8852BS_ASSOC_REQUEST_MAX_SIZE`（现在 96）要跟着放大，harness 的
`privacy=0x0` 判据要改成「目标带 CCMP+PSK 的 RSN」而不是删掉。拿到带 AID 的
Association Response 之后才是 `sec_eng_init`/`sec_info_tbl_init` 和四次握手。3b 欠的
上游顺序（`JOININFO` 在认证前 `dis_conn=true`、关联时才翻 `dis_conn=false` 并把 port
设成 INFRA、调 `mac_port_init()`）在 3d 一起还。

### 增量 3d：Association Request 带 SSID ＋ RSN element，AP 授予 AID（关联完成）

提交 `b2d9e6f`，板上 run 31，日志 `out/k1-serial/k1-assoc-20260830T125240Z.log`，
镜像仍是 `wireless_assoc_diag`，ELF SHA-256
`2fe2282ddb4113892d8ac983d3e75c42f495cb368caf45e9fe4fe9b7a07b199e`。
**本 runner 的 30 项 `--require-*` 全部通过，退出码 0**，包括 3c 没过的那三项。

#### 改了什么（只有两件事，都在请求内容和目标选择上）

1. **请求里补上该 BSS 自己的 SSID 和一条 RSN element。** 元素按 clause 9.3.3.6 排在两张
   速率表之后：id 48、长度 20，body = version LE16 1、组密码 `00 0f ac <group>`、
   成对密码计数 LE16 1 ＋ `00 0f ac 04`、AKM 计数 LE16 1 ＋ `00 0f ac 02`、
   RSN capabilities LE16 0。**没有任何密钥材料**——element 声明的是要求，不是密钥。
   capability 里按目标的 Privacy 位置 1（`0x421` → `0x431`）。
   `K1_RTL8852BS_ASSOC_REQUEST_MAX_SIZE` 96 → 128（24 头 + 4 body + 34 SSID +
   10 rates + 6 ext rates + 22 RSN = 100）。
2. **目标选择从「跳过一切 Privacy BSS」改成打分排序。** 公布 SSID(+4) > 不加密(+2) >
   已被证明会回认证请求(+1)，Beacon 数破平。本端说不清其要求的 Privacy BSS
   （只有 TKIP 成对密码、厂商私有 suite、只有 SAE）仍然跳过并返回 `-EOPNOTSUPP`；
   `k1_rtl8852bs_runtime_assoc_askable()` 在两侧都要求 `privacy == rsn`。

组密码必须**回抄** BSS 公布的值，不能固定写 CCMP：混合模式 BSS 会合法地公布一个比成对
密码更弱的组密码，AP 会拿请求里的组密码和自己的比。run 31 的目标是 `rsn-group=0x4`
（CCMP），而同一次普查里 `789682af9f60` 就是 `rsn-group=0x2`（TKIP）。

#### 上板之前先在主机上逐字节验过

把 `k1_rtl8852bs_runtime_assoc_request_build()` 连它需要的宏抽到一个一次性的 gcc 程序里
（`FAR`/`OK`/`k1_rtl8852bs_write_le16`/`k1_rtl8852bs_addr_cam_mac_valid` 打桩），
20 条断言全过后才花一次上板：开放 BSS ＋ 空 SSID = 46 字节且 capability 不带 Privacy；
SSID `SB` ＋ RSN = **70 字节**、capability `0x0431`、元素字节
`30 14 01 00 00 0f ac 04 01 00 00 0f ac 04 01 00 00 0f ac 02 00 00`；
混合模式回抄 `rsn-group=2` 而成对密码仍是 4；32 字节 SSID ＋ RSN = 100 字节正好装得下；
四条拒绝路径（Privacy 无 RSN、RSN 无 Privacy、组密码没解出来、99 字节缓冲区）各返回
`-EOPNOTSUPP`/`-EINVAL`。板上实测 `bytes=0x46` 与主机预期完全一致。

#### 板上证据

```
assoc target bssid=504f3be2e6d2 channel=0x1 beacons=0xd cap=0x431 privacy=0x1
  ssid-len=0x2 rsn=0x1 rsn-group=0x4 rsn-ccmp=0x1 rsn-psk=0x1 rsn-tx=0x1 proven=0x1
assoc advertised begin ssid-source=advertised ssid-len=0x2 channel=0x1 privacy=0x1 rsn=0x1
auth request tx  channel=0x1 bytes=0x1e sn=0x6 status=0x0
assoc request tx channel=0x1 bytes=0x46 ssid-len=0x2 cap=0x431 rsn=0x1 rsn-group=0x4
  sn=0x7 status=0x0
assoc advertised auth req=0x1 req-sn=0x6 tx-status=0x0 frames=0x1 rsp-self=0x1
  rsp-target=0x1 alg=0x0 seq=0x2 status=0x0 a2=504f3be2e6d2
assoc advertised      req=0x1 req-sn=0x7 req-bytes=0x46 tx-status=0x0 frames=0x1
  rsp-self=0x1 rsp-target=0x1 rsp-cap=0x431 status=0x0 aid=0x1 a2=504f3be2e6d2
assoc exchange rsp=0x1 status=0x0 aid=0x1 auth-rsp=0x1 beacons=0x18 bss=0x2 sweep=0x0
assoc info done-ack return=0x0
assoc CAM  done-ack return=0x0
assoc confirm bss=0x6 beacons=0xb aid=0x1
RTL8852BS2 station association complete
```

`rsp-self` 是 host 侧把 A1 和 eFuse 自身 MAC 逐字节比出来的，`status`/`aid` 是从 body
读的（AID 按 clause 9.4.1.8 掩掉高两位保留位），所以这一行只可能来自一帧真的、发给本机的
Association Response。同一次 sweep 的管理帧子类型直方图独立佐证：
subtype 11（Authentication）=1、subtype 1（Association Response）=1，
subtype 8（Beacon）=0x31。**一次请求就被回答**，而 run 30 是六次全沉默——
发送路径两轮完全一样，差别只在请求内容。

#### 两条能直接复用的结论

- **Association Request 沉默，先查请求内容，不要先动 MAC/PHY。** 两处硬要求：
  clause 11.3.5.3 要带该 BSS 自己的 SSID（认证请求不带 SSID，所以隐藏 SSID 的 BSS 会
  「认证能过、关联静默」）；clause 12.6.3 对带 Privacy 的 BSS 要求给出密码套件，
  否则请求在本移植任何一行代码被检验之前就已经被拒。
- **关联成功不改变 port 的硬件状态。** run 31 结束时 `c400=0x1e01b`：bit2
  `PORT_FUNC_EN`=0、`NET_TYPE`=0（NO_LINK）。整个认证/关联是在 scan-offload 停驻的
  dwell 上由软件收发管理帧完成的，`mac_port_init()` 仍然没移植。

#### AP 随后把我们踢了，这是预期行为

确认 sweep 里目标 BSS 的 `mgmt-other=0x1`、子类型直方图 index 12 = 1，
**即 AP 发了一帧 Deauthentication**。本端没有 PMK/PTK、也没有 `sec_eng_init`，
四次握手一帧都答不上来，AP 超时后踢掉这个 station 完全正确。
（本端没有按 A1 过滤 deauth，所以「这一帧是发给本机的」是强推断而非证明；
要变成证明，需要给 deauth 也加一条 A1 == self 的计数。）

#### 下一步

增量 3e：`sec_eng_init` / `sec_info_tbl_init` ＋ WPA2 四次握手——这是现在唯一挡在
「关联成功」和「能收发数据」之间的东西。同一批把 3b/3c/3d 欠的原厂顺序还掉
（`JOININFO` 在认证前 `dis_conn=true`、关联时才翻 `dis_conn=false` 并把 port 设成 INFRA、
调 `mac_port_init()`），并把 `mac_port_init()` 的 band0/port0 子集补上：对 STA 就是
`R_AX_PORT_CFG_P0`(0xC400) 先 `FUNC_SW=0`、`TXBCN_RPT_EN`/`RXBCN_RPT_EN` 清零、
`NET_TYPE`(bit11:10)=2(INFRA)、`TBTT_PROHIB_EN`(bit13) ＋ `BRK_SETUP`(bit16)=1、
`RX_BSSID_FIT_EN`(bit4)=1、`TSF_UDT_EN`(bit3)=1、`BCNTX_EN`(bit12)=0，再配
`BCN_INTV`/`BSS_CLR`/`TBTT_AGG=1`/`HIQ_WIN`/`HIQ_DTIM`/hiq `pkt_drop`/
`BCN_HOLD_TIME=400`/`BCN_MASK_AREA=0`，**最后**才 `PORT_FUNC_EN`(bit2)=1，
延时 10 µs 再写 `BCN_ERLY=160`/`BCN_SETUP_TIME=4`/`TBTT_ERLY=5`
（原厂 `mport.c:2010-2305`、常量在 `mport.h:23-31`）。
另外值得顺手补的证据：把认证/关联响应的前 32 字节原样打到串口——run 31 的响应帧字节
被 armed 期间的输出抑制吃掉了，判据只能依赖解析后的字段。

### 增量 3e：WPA2-PSK 四次握手跑通（Msg3 的 MIC 验过）

提交 `b712b4e`，板上 run 33，日志 `out/k1-serial/k1-wpa-20260830T145145Z.log`，
镜像 `wireless_wpa_diag`，ELF SHA-256
`3632ba044f2feeada25743a44d3ab601f4de9e4781c28477d9d83cae2cd47b4e`。
**本 runner 的 31 项 `--require-*` 全部通过，退出码 0**，其中三项是本轮新增的。

#### 卡住四次握手的不是密码学，是接收侧的一个复位值

`R_AX_DATA_FLTR (0xce30)` 在这颗片子上的复位值是 `0x00000000`——16 个数据帧子类型
每个 2 bit 全为 `0b00` = drop。也就是说 RMAC 把**所有数据帧**都丢了，一帧都不转给
SDIO 主机。EAPOL-Key 的第一条消息是单播数据帧，所以在补上这个写之前，无论 PMK 派生、
PTK 展开、MIC 计算写得多对，握手都不可能开始。

上游 `rx_fltr_init()`（在 `cmac_init()` 里）把管理、控制、数据三类过滤器统统设成
forward-to-host；本组件只复刻了 `cmac_init()` 的一个静态子集，三个都没写。管理帧过滤器
`0xce28` 复位值本来就是 `0x55555555`，这正是 3a–3d 的 Authentication/Association
Response 和 Beacon 能收到、而 3d 结尾那帧 Deauthentication 也能看到的原因——运气而非设计。
控制帧过滤器 `0xce2c` 复位值同样是 `0x55555555`。

现在扫描 dwell 前后快照并打印 `0xce28/0xce2c/0xce30` 三个值，只把数据帧过滤器写成
forward-to-host，结束后还原；控制帧过滤器只记录不写（ACK/BA 由硬件应答，主机不需要，
把繁忙信道上的全部控制帧灌进 SDIO 接收路径纯属浪费带宽）。三行日志（run 33）：

```
before ce20=0xf0170001 ce28=0x55555555 ce2c=0x55555555 ce30=0x00000000
scan   ce20=0xf017000f ce28=0x55555555 ce2c=0x55555555 ce30=0x55550055
after  ce20=0xf0170001 ce28=0x55555555 ce2c=0x55555555 ce30=0x00000000
```

写 `0x55555555` 回读是 `0x55550055`：子类型 4–7（Null、CF-Ack、CF-Poll、
CF-Ack+CF-Poll，都不带 body）硬件不接受这个写，保持 drop；0–3 和 8–15 生效。因此回读
判据只校验能承载 EAPOL 的两个子类型——Data(0) 的 bit 1:0 与 QoS Data(8) 的 bit 17:16，
掩码 `0x00030003`，整寄存器值照旧打串口。

**run 32 就是死在这条判据上**（不是硬件问题，是我把判据写成了整寄存器逐位相等）：
过滤器使能返回 `-EIO`，扫描以 `passive scan-offload error=0x5` 收场，后面 37 项 marker
一个都没跑。记在这里是因为它示范了一件事：给硬件寄存器写回读判据时，先确认哪些位是可写的。

另外把扫描 RX 聚合缓冲从 8 KiB 提到 16 KiB——数据帧现在会进主机，聚合会变大，而超长聚合
是「跳过并计数」，代价是它后面那些帧一起丢。run 33 实测没用上：`oversize=0`
`oversize-max=0` `parse-err=0` `crc-err=0` `icv-err=0`，13 个 180 ms parked dwell 里
一共只上来 `data=0x22`（34 帧）数据帧。

#### 加了什么

四个原语，自下而上：SHA-1、HMAC-SHA1、PBKDF2-SHA1、IEEE 802.11 的 PRF，然后是成对密钥
展开。`PMK = PBKDF2(passphrase, SSID, 4096, 32)`；
`PTK = PRF-384(PMK, "Pairwise key expansion",
min(AA,SPA)||max(AA,SPA)||min(ANonce,SNonce)||max(ANonce,SNonce))`；
KCK/KEK/TK 是 PTK 的三个 16 字节段。PMK 在 sweep **之前**派生：4096 轮 PBKDF2 约 16384 次
SHA-1 压缩，塞不进一个 180 ms 的 dwell。

本端要发的两帧：Msg2 153 字节，带本机 nonce 和一条 RSN element——**这条元素是从已发出的
Association Request 里逐字节抄回来的**，clause 12.7.2 要求两处必须完全一致；Msg4 131 字节。
MIC 是把 802.1X 头加整个 key frame、其中 MIC 字段读作全零，做 HMAC-SHA1-128；验 Msg3 走
同一段代码。Key Descriptor Version 3（AES-128-CMAC / AES-SIV）**拒绝并计数**，不拿
HMAC-SHA1 去糊一个必然错的答案。组密钥不解包：Msg3 的 MIC 覆盖的是**到达时的密文字节**，
所以不需要 AES 实现、也不需要一个能放结果的安全引擎就能验。

#### 上板之前先在主机上验了 21 条

SHA-1/HMAC/PBKDF2 的 ground truth 取自 Python 的 `hashlib`/`hmac`；PRF、成对密钥展开和
EAPOL-Key 帧按标准**第二次独立实现**一遍，让 C 和一个独立实现对比，而不是和自己对比。
覆盖 RFC 2202 的 HMAC 向量、超过一个 block 的密钥、两个 PBKDF2 向量、PRF-160、
「交换两端得到同一个 PTK」的对称性、Msg2/Msg4 的完整字节、「自己签的自己能验」、
「翻一个 bit MIC 就变」，以及五条拒绝路径。Msg2 = 153 字节、Msg4 = 131 字节，
和板上的 `msg2-bytes=0x99`、`msg4-bytes=0x83` 完全一致。

#### 板上证据（run 33）

```
K1 Wi-Fi GPL: wpa pmk ssid-len=0x2 status=0x0
K1 Wi-Fi GPL: wpa msg2 tx bytes=0x99 sn=0x7 status=0x0
K1 Wi-Fi GPL: wpa msg4 tx bytes=0x83 sn=0x8 status=0x0
K1 Wi-Fi GPL: assoc exchange rsp=0x1 status=0x0 aid=0x1 auth-rsp=0x1
  beacons=0x19 bss=0xd data-self=0x2 deauth-self=0x0 deauth-reason=0xffff
  wpa=0x1 msg1=0x1 msg2=0x1 msg3=0x1 mic=0x1 msg4=0x1 bssid=504f3be2e6d2
K1 Wi-Fi GPL: assoc advertised eapol=0x2 eapol-self=0x2 malformed=0x0
  ver-refused=0x0 rsn-ie=0x16 msg1=0x1 msg1-info=0x8a anonce=659fb5c5
  ptk=0x1 msg2=0x1 msg2-bytes=0x99 msg2-status=0x0 msg3=0x1
  msg3-info=0x13ca msg3-keydata=0x38 mic=0x1 mic-fail=0x0 msg4=0x1
  msg4-status=0x0 complete=0x1
K1 Wi-Fi GPL: RTL8852BS2 station WPA2 four-way handshake complete
```

`data-self=0x2` 是整轮里发给本机的数据帧总数，正好是 Msg1 和 Msg3 两帧，没有多余的。
`msg1-info=0x8a` = version 2 ＋ Pairwise ＋ Ack、无 MIC 无 Secure，是教科书里的 Msg1；
`msg3-info=0x13ca` = version 2 ＋ Pairwise ＋ Install ＋ Ack ＋ MIC ＋ Secure ＋
Encrypted Key Data，`msg3-keydata=0x38`（56 字节）是包好的组密钥。`rsn-ie=0x16`（22 字节）
是抄回来的那条 element。`deauth-self=0x0`、`deauth-reason=0xffff`：**AP 这次没踢我们**，
而 3d 那轮结尾它踢了——reason 15（四次握手超时）正是密码错或答不上时会看到的样子。

#### 为什么一次 MIC 验证就够

Msg3 的 MIC 只能由「用同一个 PMK、按同样的字节顺序派生出同一个 PTK」的一端算出来。所以
`mic=0x1 mic-fail=0x0` 这一项同时确认了：密码对、PBKDF2 对、PRF-384 的 label 和拼接顺序
对、地址与 nonce 的 min/max 排序对、以及本端 Msg2 的**每一个字节**都被 AP 按同样的解释读到了。
这比自测能给的强，因为对面不是我们写的。

#### 密码怎么处理的

三层，密码一次也没进仓库：被提交的 profile 把
`CONFIG_K1_RTL8852BS2_RUNTIME_WPA_PASSPHRASE` 留空，运行时老实返回 `-ENOKEY`；
`tools/build_k1_wpa.sh` 从 `~/.config/k1-wifi-psk.env`（0600，仓库外）读值，在 `cmake_out`
下用 `umask 077` 生成一个 0600 的 defconfig，它 `#include` 被提交的 profile 再追加这一行。
**链接出来的镜像 .rodata 里带着这个字符串**（在 flat bin 里 grep 得到 1 处、ELF 里 2 处），
所以 `out/k1-wpa` 不能发布——脚本自己的头注释就写着这句。上板后按 env 文件里的变量
grep 过：仓库 0 命中、串口日志 0 命中。

#### 对 3d「下一步」的一条更正

3d 结尾写的是「增量 3e：`sec_eng_init` / `sec_info_tbl_init` ＋ 四次握手」。**握手不需要
安全引擎**：四条 EAPOL 帧全部是不加密的明文帧，安全引擎是握手**之后**装密钥、让数据帧被
CCMP 保护才需要的东西。这次把顺序拆对了，握手先跑通，密钥安装留给下一增量。

#### 还没做的

握手完成**不等于链路能收发数据**：本端没有把 TK 装进安全引擎（没有 `sec_eng_init` /
`sec_info_tbl_init`），组密钥也没解包，所以这之后的每一帧仍然是明文，AP 侧已经在等
CCMP 保护的帧了。`mac_port_init()` 仍未移植，仍然是在 parked 的扫描 dwell 里发帧而不是
驻留在一个工作信道上，`wlan0` 仍然只会扫描，没有 DHCP、没有联网。

#### 下一步

1. `sec_eng_init` / `sec_info_tbl_init` ＋ 把 TK 写进 security CAM，让数据帧真的被 CCMP
   保护；顺带 RFC 3394 的 AES key unwrap 把 GTK 解出来装组密钥。
2. 还掉 3b/3c/3d 欠的原厂顺序（`JOININFO` 认证前 `dis_conn=true`、关联时才翻
   `dis_conn=false` 并把 port 设成 INFRA），以及 `mac_port_init()` 的 band0/port0 子集
   （寄存器顺序见 3d 的「下一步」）。
3. `rtw8852b_set_channel_{mac,bb,rf}`：驻留在信道 1，不再从 parked dwell 里发帧。

### 增量 3f：把 TK 与 GTK 装进硬件（固件四条命令全部 ack）

提交 `84d64e2`，板上 run 34，日志 `out/k1-serial/k1-wpa-20260830T174332Z.log`，
镜像 `wireless_wpa_diag`，ELF SHA-256
`09087a0c06ad84d9e1aa262a24222c24fc604ab91c2b90bf11c8fc61ab559da8`。
**本 runner 的 33 项 `--require-*` 一条没失败**，最后一行是
`PASS: K1 wireless RAM image reached NSH`，其中 `--require-runtime-wpa-keys` 是本轮新增。

#### 原厂的顺序：先地址 CAM，后安全 CAM

`mac_sta_add_key()`（`security_cam.c:667`）装一把密钥要发**两条** H2C，顺序是固定的：

1. `insert_key_to_addr_cam()` 先发地址 CAM 更新（cat 1 / class 6 / func 0，
   与 join、assoc 已经发过两次的**同一条命令**，`agg_en=1`、`done_ack=1`），
   把密钥槽写进 ADDR_CAM 的 dword9/dword10；
2. 再发安全 CAM（cat 1 / class 0xa / func 1，content 40 字节，`offset=0`、
   `len=0x20`、`done_ack=1`），把 16 字节密钥送下去。

反过来发的话，安全 CAM 里有密钥而地址 CAM 还没有指向它的槽，硬件是按一个它不认的槽去查表。
这条顺序不是猜的，是原厂唯一的调用者就这么写的；也正因为地址 CAM 那条命令和 join/assoc
发的是同一条，重发它在本端已经被证明安全过两次。

槽位策略同样照抄：`sec_ent_mode` 决定哪个槽能放哪类密钥，CCMP/CCMP 是 mode 2
（`rtw_phl_trans_sec_mode()`），`check_key_index()` 的合法区间是单播 0–1、组播 2–4、
BIP 5–6。所以本端把 TK 放 slot 0（安全 CAM entry 0，key id 0）、GTK 放 slot 2
（entry 1，key id 由 Msg3 给），`sec_ent_valid = (1<<0)|(1<<2) = 0x5`。

#### 加了什么（五块，自下而上）

1. **安全引擎 bring-up**，`sec_eng_init()` 里本端这条路要用的子集：
   `R_AX_SEC_ENG_CTRL (0x9d00)` 或上 `0x073f` 并清掉 `B_AX_TX_PARTIAL_MODE (bit11)`，
   `R_AX_SEC_MPDU_PROC (0x9d04)` 或上 `0x3`。板上实测 ctrl `0x80002800` → `0x8000273f`
   （复位值里 bit11 本来是 1，必须清掉），mpdu-proc `0x0` → `0x3`。
2. **RFC 3394 AES key unwrap ＋ GTK KDE 解析**：KEK 是 `PTK[16:32]`，完整性值是 8 个
   `0xa6`；KDE 是 type `0xdd`、OUI `00-0F-AC`、data type 1。Msg3 的 56 字节包装 key data
   解出 48 字节明文（`keydata-plain=0x30`），里面拿到 16 字节 GTK 和 key id 1。
   **握手本身仍然验的是到达时的密文字节**，所以解包不在「握手成不成」这条判据的路径上——
   3e 那轮没有 AES 也能验 MIC，正是这个原因。
3. **安全 CAM 载荷序列化**：dword0 = `index | offset<<8 | length<<16`，dword1 = 密钥类型
   ＋ ext-key bit(4) ＋ spp-mode bit(5)，dword2..5 是 16 个密钥字节、不做字节序翻转。
   一条 entry 宽 `0x20` 字节，一条命令从 offset 0 整条写完，这也是原厂唯一调用者的做法。
   CCMP-128 是 type 6、`ext_key=0`、`spp_mode=0`，序列化出来 dword1 = `0x6`。
4. **模块级密钥槽状态 ＋ `key_install()`**：槽状态放在模块级而不是提交函数里面，因为固件是
   **累加**的——原厂每装一把密钥都把整条 ADDR_CAM entry 重发一遍，所以第二把密钥那次重发
   必须带着第一把的槽。地址 CAM 那条命令失败就把 mode/valid/entry/keyid 四个字段一起回滚，
   免得后面任何一次重发带上固件从没接受过的槽。这里不存任何密钥字节，槽号、CAM index、
   key id 都是寻址而不是秘密。
5. **在关联完成路径上调用**，位置在「四次握手完成」那一行**之后**：装密钥失败也不会改写
   3e 那条证据，日志照旧说清握手本身走到哪一步；失败通过既有的 `goto error` 变成这一步的
   errno（`station WPA2 error=`）。汇总行只打槽位、CAM index、key id 和四个命令的结果。

#### 上板之前先在主机上把序列化跑了一遍

给 wpa profile 打开 `CONFIG_K1_RTL8852BS2_RUNTIME_ADDRESS_CAM_DIAGNOSTIC` 是个硬门
（`board/k1/muse_pi_pro/src/k1_wireless.c:805`：`probe_ret` 为负会把后面所有诊断——包括 WPA
那条——全部跳过），任何一个期望值写错就要赔一次上板。所以改成把地址 CAM／安全 CAM 诊断连同
它依赖的 1059 个 `#define` 抽出来在主机上编译运行，断言全过之后才上板：

```
runtime address CAM dword2=0x11133f5d dword13=0x1b0a2afd H2C=1/6/0
runtime address CAM sec dword9=0x8205aa dword10=0x1000005 security CAM dword0=0x200011 dword1=0x6 H2C=1/a/1
```

`dword9=0x8205aa` 里 `sec_ent_mode=2`（SH16 MSK 0x3）、slot 2 的 key id = 2（SH22）；
`dword10=0x1000005` 里 `sec_ent_valid=0x5`、`sec_ent0=0x0`、`sec_ent2=0x1`。
安全 CAM `dword0=0x200011` = index 0x11 | offset 0 | len `0x20`。
上板验的因此是更强的那件事：固件**接受**了这四条命令。

（顺手修了一个只在日志里看得见的坑：诊断里四条拒绝路径是层叠在 ext-key 那个用例上的，
不会重写 `sec_content`，所以打出来的是 `dword1=0x36` 而不是真正会发出去的 `0x6`。
现在打印前重新构造一次被接受的用例，并断言 `dword1 == 6`。）

#### 板上证据（run 34）

```
K1 Wi-Fi GPL: sec-eng ctrl before=0x80002800 after=0x8000273f mpdu-proc before=0x0 after=0x3
K1 Wi-Fi GPL: sec-eng init status=0x0
K1 Wi-Fi GPL: assoc advertised eapol=0x4 eapol-self=0x4 malformed=0x0 ver-refused=0x0
  rsn-ie=0x16 msg1=0x1 msg1-info=0x8a anonce=f1c68362 ptk=0x1 msg2=0x1 msg2-bytes=0x99
  msg2-status=0x0 msg3=0x1 msg3-info=0x13ca msg3-keydata=0x38 mic=0x1 mic-fail=0x0
  keydata-plain=0x30 unwrap=0x0 kde=0x0 gtk=0x1 gtk-len=0x10 gtk-id=0x1 msg4=0x1
  msg4-status=0x0 complete=0x1
K1 Wi-Fi GPL: RTL8852BS2 station WPA2 four-way handshake complete
K1 Wi-Fi GPL: TK CAM H2C queued sequence=0xe pages=0x20 FIFO=0x1c00c
K1 Wi-Fi GPL: TK CAM done-ack return=0x0
K1 Wi-Fi GPL: TK SEC H2C queued sequence=0xf pages=0x20 FIFO=0x1c009
K1 Wi-Fi GPL: TK SEC done-ack return=0x0
K1 Wi-Fi GPL: GTK CAM H2C queued sequence=0x10 pages=0x20 FIFO=0x1c00c
K1 Wi-Fi GPL: GTK CAM done-ack return=0x0
K1 Wi-Fi GPL: GTK SEC H2C queued sequence=0x11 pages=0x20 FIFO=0x1c009
K1 Wi-Fi GPL: GTK SEC done-ack return=0x0
K1 Wi-Fi GPL: station keys sec-mode=0x2 sec-valid=0x5 tk-ent=0x0 tk-keyid=0x0 tk-cam=0x0
  tk-sec=0x0 tk=0x1 gtk-ent=0x1 gtk-keyid=0x1 gtk-cam=0x0 gtk-sec=0x0 gtk=0x1
K1 Wi-Fi GPL: RTL8852BS2 station WPA2 keys installed
```

四条 `done-ack return=0x0` 是这一增量的全部硬证据：固件收下了地址 CAM 的槽更新和两条
安全 CAM 写入，并且回了 0。`gtk-keyid=0x1` 是 AP 在 Msg3 里给的 key id，不是本端编的。

#### 这轮 AP 把 Msg1 发了三遍

`eapol=0x4` 而 3e 那轮是 `0x2`：日志里 `wpa msg2 tx` 出现了三次（`sn=0x7/0x8/0x9`），
也就是 AP 重传了两次 Msg1，直到第三次之后才收到本端的 Msg2 并接着发 Msg3。原因是本端仍然
在 parked 的扫描 dwell 里发帧，两个 dwell 之间发不出去，AP 的重传定时器先响了。握手仍然
一次过（`mic=0x1 mic-fail=0x0 complete=0x1`），但这正是「没有驻留信道」要付的利息，
也是 `mac_port_init()` ＋ `set_channel` 该排在下一位的又一条理由。

#### 这一步证明了什么、没证明什么

证明的是：安全引擎按原厂的值起来了，GTK 从 Msg3 里解出来了，四条命令按原厂顺序发出去并被
固件确认，槽位／CAM index／key id 是 mode 2 下合法的组合。

**没有证明任何一帧被 CCMP 保护过。** 发送描述符的安全字段还没填（没有 `sec_type`／
`sec_cam_idx` 那几位），也没有数据路径去填；接收侧同理，`icv-err`／`crc-err` 仍然是扫描
统计里的两个 0，不代表有过解密。所以这一增量能说的只有「密钥装进去了、固件认了」。

#### 密钥怎么处理的

和 3e 同一套三层（profile 里留空、构建脚本从 `~/.config/k1-wifi-psk.env` 读、
`out/k1-wpa` 不发布），本轮另加两条：H2C 的栈载荷缓冲和装密钥的 `sec` 结构在交给传输层之后
**立刻 memset 清零**，无论命令成功还是失败；汇总行只有槽位、索引、key id 和 errno，
没有任何由 TK 或 GTK 派生出来的值。上板后按 env 文件里的变量 grep 过：
仓库 0 命中、run 34 的串口日志 0 命中。

#### 还没做的

除了上面那条「没有加密流量」：`mac_port_init()` 仍未移植（port 0 仍是 `c400=0x1e01b`，
`PORT_FUNC_EN=0`、`NET_TYPE=NO_LINK`），3b/3c/3d 欠的 `JOININFO` 顺序（认证前
`dis_conn=true`、关联时才翻 `dis_conn=false` 并把 port 设成 INFRA）仍然欠着，
`wlan0` 仍然只会扫描，没有 DHCP、没有联网。

#### 下一步

1. `mac_port_init()` 的 band0/port0 子集 ＋ `rtw8852b_set_channel_{mac,bb,rf}` 驻留信道 1，
   然后才是发送描述符的安全字段——这三件凑齐才能谈「被 CCMP 保护的数据帧」。
2. 还掉 3b/3c/3d 欠的原厂 `JOININFO` 顺序。
3. `rtw_hal_bb_dm_init` / `rtw_hal_rf_dm_init`（DACK/RCK/IQK/DPK/TSSI），
   以及把认证／关联响应的前 32 字节原样打到串口这条一直没补的证据。

### 增量 3g：把 CMAC port 0 按原厂顺序打开（顺带证伪「TSF 被冻住」）

提交 `3aadcd8`，板上 run 35，日志 `out/k1-serial/k1-wpa-20260830T194339Z.log`，
镜像 `wireless_wpa_diag`，ELF SHA-256
`d5a8f93651b35260668b03d7384812c45b453da5ab9666870f080e12f7ff84c7`。
**本 runner 的 34 项 `--require-*` 一条没失败**，最后一行是
`PASS: K1 wireless RAM image reached NSH`，其中 `--require-runtime-port-init` 是本轮新增。

#### 原厂顺序照抄，功能使能排在最后

`mac_port_init()`（`mport.c:2010`）在 band 0 / port 0 / INFRA / `mbid_num=0` 这条路径上
的写入顺序，本端一步不差地复现：两个 beacon 上报使能清零 → 网络类型 2 →
TBTT prohibit 窗口 → 收 BSSID 过滤 → TSF 更新使能 → beacon 发送关 → beacon 间隔 100 →
BSS colour 0 → TBTT aggregate 1 → 高优先队列窗口和它的两个 update 位 → DTIM 0 →
sub-space 清零 → beacon hold 400 → beacon mask 0 → **最后才是 `PORT_FUNC_EN`** →
`dly_port_us(10)` → 然后才是被原厂校验函数管着的三条：beacon early 160、
beacon setup 4、TBTT early 5。

顺序里唯一值得强调的就是「功能使能排在最后」：前面所有字段都要在 port 开始按 TBTT 干活
之前就位，原厂把 `FUNC_EN` 放在第 16 步，本端也放在第 16 步。

#### 大部分字段固件自己已经摆对了

每一次写都是「读—比较—写」，并且把字段名、写前的值、要写的值都打出来，所以串口直接回答了
一个之前只能猜的问题：这些字段里有多少是固件 bring-up 已经留成 station 想要的样子的。
答案是大部分：

```
port init tx-rpt   was=0x2   set=0x0   written
port init rx-rpt   was=0x1   set=0x0   written
port init net-type was=0x0   set=0x800 written
port init bcn-prct was=0x12000 set=0x12000 skip
port init rx-sw    was=0x10  set=0x10  skip
port init rx-sync  was=0x8   set=0x8   skip
port init tx-sw    was=0x0   set=0x0   skip
port init bcn-intv was=0x64  set=0x64  skip
port init bss-clr  was=0x0   set=0x0   skip
port init tbtt-agg was=0x1   set=0x1   skip
port init hiq-win  was=0x2   set=0x0   written
port init hiq-upd  was=0x0   set=0x3   written
port init dtim-num was=0x1   set=0x0   written
port init sub-spc  was=0x0   set=0x0   skip
port init bcn-hold was=0xc80000 set=0x1900000 written
port init bcn-mask was=0x0   set=0x0   skip
port init func-en  was=0x0   set=0x4   written
port init bcn-erly was=0xa0  set=0xa0  skip
port init bcn-setup was=0x2  set=0x4   written
port init tbtt-erly was=0x50000 set=0x50000 skip
```

真正变了的是网络类型、功能使能、两个 beacon 上报使能、高队列窗口和它的 update 位、
DTIM、beacon hold、beacon setup 这几项；整块寄存器的差值就是
**`c400 0x1e01b -> 0x1e81c`** 和 **`c404 0x00c80002 -> 0x01900004`**，
和写代码时按原厂算出来的预期完全一致（`c408`/`c40c`/`c410`/`c414` 一位没动）。

#### 三个校验函数连 clamp 一起复现

原厂对最后三条写入各有一个校验：`_bcn_setup_chk`（`mport.c:562`）、
`_bcn_erly_chk`（`:735`）、`_tbtt_erly_chk`（`:820`）。它们做两件事：越界直接
`MACFUNCINPUT` 报错，以及和相邻字段比较后**把值夹一下**（例如 setup ≥ 当前 bcn-erly 就
夹成 `up_lmt-1`）。本端把三个都复现了，包括夹的算法，而且一旦会夹就单独打一行
`... clamped=`，绝不悄悄改掉调用者给的值。run 35 三条一次都没夹。

顺带记一条读原厂才看清的事：`bcn_hold` 和 `bcn_mask` 也各有检查，但原厂调用它们的位置在
`FUNC_EN` 之前，那时 `port_stat` 还是 `DIS`，而两个检查开头就是「stat==DIS 直接返回成功」
——所以在原厂的这个顺序里它们是空转的。本端照样按空转处理，并把 `hold-limit` 打出来备查
（`hold-limit=0xbe0 hold=0x190 checked=0`）。

#### 为什么必须按字节写

`0xc413`（TBTT aggregate）、`0xc427`（DTIM）、`0xca08`（TSF 时间戳控制）、
`0xc590`（高队列窗口）四个地方原厂用的是 `MAC_REG_W8`。这不是风格问题：`0xc427` 所在
dword 的起始地址是 `0xc424`，那是 `R_AX_BCN_ERR_FLAG_P0`，一次 dword 读改写会把它一起
重写。本端的寄存器访问在 `0x1000-0x1f00` 之外走的是间接 CMD52 路径，字节访问是原生的，
所以直接按字节写，和原厂一致。

#### 3b/3c/3d 欠的 `JOININFO` 顺序还掉了

原厂（`rtw89_core_sta_add()` / `rtw89_core_sta_assoc()`）发两次 station join：认证之前那次
带 `dis_conn=true`，关联成功之后重发一次才翻成 `false`。本端之前两次都发 `false`，等于在
还没认证的时候就跟固件说「这个 STA 已经连上了」。现在这个状态是函数参数，板上：

```
K1 Wi-Fi GPL: join info  H2C queued sequence=0xa disconn=0x1 pages=0x20 FIFO=0x1c006
K1 Wi-Fi GPL: assoc info H2C queued sequence=0xc disconn=0x0 pages=0x20 FIFO=0x1c006
```

#### 两处点名不猜

* **disable 流程没移植**：原厂 `mac_port_init()` 开头对已经在跑的 port 有一段
  停用流程。本端一次 boot 只会跑一个诊断（assoc **或** wpa，`k1_wireless.c` 里是
  `#ifdef/#else`），port 只被初始化一次，所以第二次调用是幂等的提前返回并打
  `port init already run stat=`，不是假装做了 teardown。
* **`mac_wde_pkt_drop()` 的正文不在手上的原厂子集里**：所以第 12 步的
  `REL_HIQ_PORT` 是读一下 `R_AX_BCN_DROP_ALL0`（`0xc560`）然后打一行
  `not-ported` 的空操作，不去猜一条 H2C。板上 `c560=0x0 p0=0x0`，没有待丢的 beacon，
  这个空操作在本路径上也确实无事可做。

#### 失败为什么不中断关联

`port init` 的返回值调用方不看。理由是 `FUNC_EN` 一旦写下去，从中间放弃会留下一个
半配置的 port，而且会把这一轮跑起来就是为了收集的四次握手证据一起丢掉。取而代之的是：
`RTL8852BS2 port init complete` 只在「序列跑完 **且** TSF 在走」时才打，
`--require-runtime-port-init` 认的就是这一行加上 `status=0x0`、`tsf=0x1` 的结果行，
所以回归照样让整轮失败。

#### 更正：port 的 TSF 从来没被冻住

3d/3e/3f 的文档和 `wireless_assoc_diag` 的注释都写过「`mac_port_init()` 没移植，所以
port 的 TSF 还是冻着的」。这句话是错的，本轮从序列内部证伪：`dly_port_us(10)` 是原厂
放在 `FUNC_EN` 和 beacon early 之间的一个**基于 TSF 的等待**，它靠反复读 `c438` 判断
时间有没有前进，读到不动就返回「tsf not running」。板上：

```
port init tsf-delay running=0x1 error=0x0
port0 post-assoc-cam c438=0x320d1d86 ...
port0 post-port-init c438=0x3211044e ...
```

TSF 在 `FUNC_EN` **之前**就已经是 AP 自己的计时器（run 34 的 `c438=0x121_xxxxxxxx`
在 `FUNC_EN=0` 时就在走），原因是固件 bring-up 已经把收 BSSID 过滤和 TSF 更新使能留成
开着的，而地址 CAM 里有 BSSID。`FUNC_EN` 管的是这个 port 要不要按 TBTT 干活，不是计时器
要不要走。Kconfig 帮助和那条 profile 注释都改过来了。

#### port 打开之后握手仍然一次过

这是写代码前记下的风险（INFRA ＋ `FUNC_EN` 可能扰动目前从停驻扫描 dwell 上发 EAPOL 的
路径）。run 35 的结果和 run 34 一模一样：`eapol=0x4`、`wpa msg2 tx` 三次
（`sn=0x8/0x9/0xa`，AP 重传了两次 Msg1）、`mic=0x1 mic-fail=0x0 complete=0x1`、
两把密钥照样装进硬件（`sec-valid=0x5`、四条 done-ack 全 0）。也就是说打开 port 既没有
修好「在 dwell 之间发不出去」，也没有把它弄坏——修它要靠下一步的驻留信道。

#### 这一步证明了什么、没证明什么

证明的是：本端能按原厂顺序把 band0/port0 配成 INFRA 并使能，每一个字段都读回来了，
三个校验函数的边界本端算得和原厂一致，port 的计时器是 AP 的，`JOININFO` 的连接状态
不再骗固件。

**没有证明任何一帧被 CCMP 保护过，也没有证明本端驻留在了信道 1。** 发送描述符的安全字段
仍然是空的，帧仍然从停驻的扫描 dwell 上发出去；`wlan0` 的行为一个字节没变，仍然只会扫描，
没有 DHCP、没有联网。仍然是 RAM-only：eMMC / SPI flash / eFuse / U-Boot 环境一个都没写。

#### 下一步

1. `rtw8852b_set_channel_{mac,bb,rf}`：真正驻留信道 1，不再从 parked dwell 上发帧
   （增量 3h）。
2. 发送描述符的安全字段（`sec_type` / `sec_cam_idx`）——密钥已经在 CAM 里、槽也有了，
   缺的只是发送路径去引用它，这是第一次有机会发出一帧被 CCMP 保护的数据帧（增量 3i）。
3. `rtw_hal_bb_dm_init` / `rtw_hal_rf_dm_init`（DACK/RCK/IQK/DPK/TSSI）那一批，
   以及把认证／关联响应的前 32 字节原样打到串口这条一直没补的证据。

### 增量 3h：不跑扫描的驻留收发窗口（先证伪「信道不是驻留的」）

代码已实现，上板跑过两轮：**run 36 证伪了本节的一个设计假设**（关联跑完射频并不在目标
信道上），**run 37 撞上 sweep 完成判定里的一个竞态**（它自己把关联和四次握手全过了，
`mic=0x1 msg4=0x1`，但确认 sweep 报了 `-ETIMEDOUT`）。两个原因都修完之后 **run 38 一次跑通**，本节的判据到这一轮才全部拿到证据。
除了下面三条更正——它们的证据来自 run 35——本节的设计与判据都是写在上板之前的，
两轮板上结果和它们引出的改动补在本节末尾。

镜像 `wireless_wpa_diag`，新符号 `CONFIG_K1_RTL8852BS2_RUNTIME_RESIDENT_DIAGNOSTIC=y`；
run 38 用的带钥匙 ELF SHA-256
`40117d228c8f568d34326bf3b5d5c6d068012891883c60142fdbade04aae7915`
（`text 777324 data 9768 bss 25296`），`--no-key`
`3e6ef1efe08ef24a99be0b300c15aca326f9f9c07c39078bfdf948e438e26d7f`
（`text 777308 data 9768 bss 25296`）；告警仍然只有长期存在的那 6 条。
（run 36 跑的是 `95c74920…`／`323525a1…`，run 37 跑的是 `b32aa5d4…`／`56e4d197…`。）

#### 更正一：信道一直是驻留的

3e/3f/3g 都写过「本端仍然在 parked 的扫描 dwell 里发帧而不是驻留在一个工作信道上」，并且
因为这句话把 `rtw8852b_set_channel_{mac,bb,rf}` 排成下一步的第一位。被替换的原句是：

- 3e「还没做的」：「`mac_port_init()` 仍未移植，仍然是在 parked 的扫描 dwell 里发帧而不是
  驻留在一个工作信道上」
- 3f「这轮 AP 把 Msg1 发了三遍」：「原因是本端仍然在 parked 的扫描 dwell 里发帧，两个
  dwell 之间发不出去，AP 的重传定时器先响了……这正是『没有驻留信道』要付的利息」
- 3g「这一步证明了什么、没证明什么」：「也没有证明本端驻留在了信道 1……帧仍然从停驻的扫描
  dwell 上发出去」

run 35 自己的 RF 回读证伪了第一句。那一轮打了 144 条 `scan RF readback`，每次 sweep 前
（`before`）后（`after`）各一次、两条路径 ＋ D-die 镜像都读，连起来是一条链：

```
522  pre-si-reset ch-reg=0x1001 ch=0x1   <- 固件 bring-up 留下的，本端还没扫过
532  before       ch-reg=0x1001 ch=0x1
715  after        ch-reg=0x1c0d ch=0xd   <- 1-13 全扫，停在 13
1058 before       ch-reg=0x1c0d ch=0xd   <- 下一次 sweep 开始时还是 13
1774 after        ch-reg=0x1c01 ch=0x1   <- park=1 的 sweep，停在 1
1845 before       ch-reg=0x1c01 ch=0x1
```

**每一次 sweep 的 `before` 都等于上一次 sweep 的 `after`，一次例外都没有。** sweep 结束时
没有任何东西把信道退回去，`ch` 字段就停在最后一次 dwell 的信道上；高位那几位只在第一次
sweep 时从 `0x1000` 变成 `0x1c00`，之后 143 条回读一直是 `0x1c00`。

也就是说「信道是固件顺手停在那儿的」这个说法本身没错，但结论反了：**它停在那儿正是驻留**，
本端要的信道 1 在 park=1 的 sweep 之后就在 RF 里，用不着先移植 `set_channel` 才有。

#### 更正二：sweep 之外收不到东西，是本端自己关的

sweep 结束时真正变的是本端自己的三个接收过滤器。同一份 run 35 日志：

```
515 scan RX filter before ce20=0xf0170001 ce28=0x55555555 ce30=0x00000000
516 scan RX filter scan   ce20=0xf017000f ce28=0x55555555 ce30=0x55550055
760 scan RX filter after  ce20=0xf0170001 ce28=0x55555555 ce30=0x00000000
```

`ce20`（`R_AX_RX_FLTR_OPT`）低位那 `0xe` 是 A1 匹配、广播、多播三位；`ce30`（数据帧子类型
过滤）在 dwell 之外是 `0x0`——**所有数据子类型都丢**。所以 sweep 之外这台主机既收不到
Beacon（广播帧），也收不到发给自己的单播数据帧（EAPOL Msg1 就是一帧 QoS Data）：不是射频
不在信道上，是 `k1_rtl8852bs_scan_rx_filter_restore()` 把过滤器关回去了。`ce30=0x0` 这个
初值增量 3e 记过一笔（「卡住四次握手的不是密码学，是接收侧的一个复位值」），只是当时只把它
当成 dwell **之内**要打开的东西，没把「dwell 之外它又被关回去」和「dwell 之间收不到」
连起来。

#### 更正三：AP 重传两次 Msg1 的原因仍然不明，「dwell 之间发不出去」不是它

3f/3g 把 `eapol=0x4` 解释成「本端在两个 dwell 之间发不出 Msg2」。run 35 的日志排下来不
支持这个解释：整个握手发生在**同一个 park=1 的 sweep 之内**，那个 sweep 反复对信道 1 重新
`next-channel`，`ce20`/`ce30` 全程是打开的值；而且 `wpa msg2 tx sn=0x8` 和 `sn=0x9` 是
紧邻的两行，中间没有任何 dwell 边界。两份 Msg1 更像是本端一次 FIFO 轮询里排队取出来的
（AP 在本端把 FIFO 抽干之前就重传了），那是接收轮询延迟，不是发送被挡住。

**这条只撤回错的解释，不换一个新解释。** 真实原因（接收轮询延迟／Msg2 没到 AP／AP 自己的
重传策略）本轮不猜，`eapol=0x4` 仍然是一个未解释的读数。

#### 这个增量因此改做什么

原计划的 3h 是移植 `rtw8852b_set_channel_{mac,bb,rf}`：几百条寄存器写，为了拿到一个本端
已经有了的东西。既然信道已经驻留，真正没被证明的就剩一件——**没有任何 sweep 在跑的时候，
本端自己的收发循环还能不能工作。** 3h 因此改成一个有界的驻留收发窗口，跑在关联／握手／
装钥匙之后，`wireless_wpa_diag` 的最后一步。符号只有 wpa profile 打开，但调用点放在
`if (wpa) { … }` 之外，所以 assoc-only profile 打开它也能跑（`depends on
K1_RTL8852BS2_RUNTIME_ASSOC_DIAGNOSTIC`）。

#### 它写什么、不写什么

**只写三个接收过滤器，而且全部写回。** 复用现成的
`k1_rtl8852bs_scan_rx_filter_enable()` 把 `ce20`/`ce28`(mgmt)/`ce30`(data) 打开，窗口结束
后 `..._restore()` 原值写回。**一个信道寄存器都不动**：没有 `set_channel`、没有 H2C、
没有 sweep，所以窗口里发生的任何事都只能是收发路径的性质，不可能是「刚才那几百条写把它
救活了」。

进出各采一次「定义信道」的 13 个值，逐项比：

| 层 | 寄存器 | 是什么 |
| --- | --- | --- |
| MAC | `c010` | `R_AX_WMAC_RFMOD`，带宽 |
| MAC | `c628` | `R_AX_TXRATE_CHK` 的 `B_AX_BAND_MODE` BIT(4) |
| BB | `49c0` / `49c4` | SCO / 带宽 |
| BB | `0734` | [27:16] 信道号 |
| BB | `0700` / `2344` / `4738` / `4aa4` | 接收路径 / CCK block / segment A / segment B |
| RF | `0x18` ×2 ＋ D-die 镜像 ×2 | 两条路径的信道寄存器（走现成的只读回读函数） |

任何一项动了，就把字段名、进入值、退出值打成一行 `resident channel moved <name>
enter=… exit=…`，并让这一步返回 `-EIO`——「窗口静默」和「有人把信道挪了」因此不会混成
同一个结论。

窗口 3000 ms：轮询 SDIO RX FIFO（`k1_rtl8852bs_runtime_rx_read()` 的 `-EAGAIN`/`-ENOSPC`
语义照旧），进入 300 ms 后发一帧**定向** Probe Request（A1=A3=BSSID、A2=本机、SSID 元素
带目标自己的名字、速率表和野卡版一致），没等到回应就每 500 ms 重发，最多 4 次，每次换
sequence number。**这一帧是把窗口从「能收」变成「能收也能发」的那件事**：Probe Response
只会回给发过 Probe Request 的地址，收到它就同时证明了两个方向。

计数分开记，邻居的帧一条都不算进判据：目标 BSSID 的 Beacon / 别人的 Beacon、
Probe Response / 「A1 是本机」的 Probe Response、目标发来的数据帧、目标发来的
Deauth/Disassoc 及其 reason code、解析错误、超长帧。这样「窗口静默」和「关联早就被 AP
拆了」是两个可区分的结果。

#### 串口长什么样

```
K1 Wi-Fi GPL: resident park before parked=0xd bb-ch=0xd target=0x1 sweep=0x1
K1 Wi-Fi GPL: resident park after parked=0x1 bb-ch=0x1 target=0x1 sweep-err=0x0 \
  bss=0x1 bcn-target=0xa
K1 Wi-Fi GPL: RTL8852BS2 station resident park complete
K1 Wi-Fi GPL: resident window enter channel=0x1 ssid-len=0x2 window-ms=0xbb8
K1 Wi-Fi GPL: resident channel enter c010=… c628=… bb49c0=… bb49c4=… bb0734=… \
  bb0700=… bb2344=… bb4738=… bb4aa4=… rf18-a=… rf18-b=…
K1 Wi-Fi GPL: resident window parked=0x1 bb-ch=0x1 target=0x1
K1 Wi-Fi GPL: resident probe tx sn=… bytes=… status=0x0
K1 Wi-Fi GPL: resident channel exit  c010=… （同上 11 项）
K1 Wi-Fi GPL: resident window channel=… polls=… rx-reads=… frames=… beacons=… \
  bcn-target=… probes=… probe-status=… probe-rsp=… probe-rsp-self=… data-target=… \
  deauth=… reason=… parse-err=… oversize=… ch-stable=0x1 rx=0x0 filter=0x0
K1 Wi-Fi GPL: resident window frames total=… mgmt=… ctrl=… data=… ext=… data-all=… \
  self-tx=… mgmt-other=… subtypes=… unclassified=…
K1 Wi-Fi GPL: RTL8852BS2 station resident window complete
```

park 步骤发现射频已经在目标信道上时，`before` 那一行打 `sweep=0x0`，后面直接是
`resident park complete`，不跑 sweep 也不写任何寄存器。

失败时不是这一行，而是 `resident window error=<errno>`，判据阶梯按「最能解释的原因排前面」：
进入快照解出来的驻留信道 ≠ 关联信道 → `-ECHRNG`（**在 3 秒窗口打开之前就判，run 36 之后
加的**）；退出快照读失败 → 它的 errno；有寄存器动了 → `-EIO`；接收路径出错 → 它的 errno；
过滤器写回出错 → 它的 errno；目标一条 Beacon 都没有 → `-ENODATA`；有 Beacon 但没有发给本机
的 Probe Response → `-ETIMEDOUT`；否则 `OK`。park 步骤自己的失败也分开报：它那次 sweep 的
errno，或者 sweep 跑完信道仍然不对时的 `-ECHRNG`。

**失败不撤回已经报出去的关联和握手。** 调用点是 `(void)`，完成标记只在通过时才打，所以
验收靠这一行的有无，而不靠这一步去否定上面那些已经有硬证据的结论。

#### 判据

runner 加了一个 flag，`--require-runtime-resident`，板上验收从 34 条变成 **35 条**
`--require-*`。它从 `resident window enter channel=` 那一行往后切，然后要求七件事同时成立
（后两条是 run 36 之后加的，flag 数不变）：

0. `RTL8852BS2 station resident park complete`，并且 `resident park after` 与
   `resident window parked=` 两行里 `parked=` 和 `target=` 数值相等——**判据里先有「在正确
   的信道上」，再谈收到了什么**；
1. ` bcn-target=` 非零——目标 AP 的 Beacon 在没有 sweep 的情况下进了本端的接收路径；
2. `resident probe tx sn=… status=0x0`——定向 Probe Request 交给硬件成功；
3. ` probe-rsp-self=` 非零——AP 的 Probe Response 回到了本机地址（发送真的出去了）；
4. ` ch-stable=0x1 rx=0x0 filter=0x0`——13 个信道寄存器一个没动，接收路径和过滤器写回都
   没报错；
5. `RTL8852BS2 station resident window complete`。

两种结果都推进：过了，下一个拦路虎就明确是发送描述符的安全字段和数据面；不过，
`resident channel moved …` 或者 `-ENODATA`/`-ETIMEDOUT` 直接点名是哪一层出的问题，
而不用再猜。

#### 板上结果一：run 36 证伪了「关联跑完射频就停在目标信道上」

run 36（`out/k1-serial/k1-wpa-20260830T221808Z.log`，35 条 flag 里缺 3 条）一路走到驻留
窗口，窗口自己的读数把故障定位得没有歧义：

```
3167 resident window enter channel=0x1 ssid-len=0x2 window-ms=0xbb8
3177 resident channel resident-enter … bb0734=0xd0000 … rf18-a=0x1c0d rf18-b=0x1c0d
3197 resident channel resident-exit  … 13 项与进入时逐字节相同
3199 resident window channel=0x1 polls=0xb30 rx-reads=0x29 frames=0xb beacons=0x0 \
     bcn-target=0x0 probes=0x4 probe-status=0x0 probe-rsp=0x0 probe-rsp-self=0x0 \
     data-target=0x0 deauth=0x0 reason=0x0 parse-err=0x0 oversize=0x0 \
     ch-stable=0x1 rx=0x0 filter=0x0
3200 resident window error=0x3d                  <- ENODATA
```

`ch-stable=0x1 rx=0x0 filter=0x0` 说明这一步自己什么都没做错：13 个寄存器一个没动，接收
路径没报错，过滤器写回没报错；4 帧 Probe Request 也都交出去了（`probe-status=0x0`）。真正
的读数是 `rf18-a/b=0x1c0d`——**射频在信道 13，关联在信道 1**。窗口在一个空信道上听了 3 秒，
`-ENODATA` 就是它唯一能给的答案。

被证伪的是「更正一」之后顺着写下来的那句默认假设：**park=1 的 sweep 之后本端要的信道就在
RF 里**——这句本身对，但那不是驻留窗口之前的最后一次 sweep。装钥匙之前，同一个函数还要跑
一次**不 park 的 1-13 确认 sweep**（它是「关联在整整一次全信道扫描之后还活着」这条证据），
run 36 自己的回读把顺序摆得很清楚：

```
2942 scan RF readback before … ch-reg=0x1c01 ch=0x1   <- 握手那次 park=1 的 sweep 留下的
3106 scan RF readback after  … ch-reg=0x1c0d ch=0xd   <- 确认 sweep 走完 1-13，停在 13
3154 assoc confirm bss=0x4 beacons=0x10 aid=0x1
3166 RTL8852BS2 station WPA2 keys installed
3167 resident window enter channel=0x1                <- 窗口从这里开始
```

**证伪它的是这个函数自己的确认 sweep，不是外部条件。** 「更正一」那条规律（每次 sweep 的
`before` 等于上一次的 `after`）在 run 36 里一次例外都没有，只是这次它指向的是信道 13。

#### 因此加的三件事

1. **窗口之前的 park 步骤。** 确认 sweep 保持不 park——它是关联存活的证据，收窄信道会削弱
   它。改成在窗口之前单独加一步：读一次两条路径的 RF `0x18`，已经在目标信道上就直接返回
   `RTL8852BS2 station resident park complete`，不在就 `park_arm(channel)` ＋ 一次
   parked sweep ＋ `park_disarm()`，然后再读一次核对。这一步顺带报自己那次 sweep 收到的
   目标 Beacon 数（`bcn-target=`），所以「射频听得见这个 AP」和「窗口什么都没听到」不会混
   成一个结论。将来若把确认 sweep 挪到窗口之后，这一步会自己变成一次无操作的回读。
2. **`-ECHRNG`（44）。** 窗口在打开 3 秒之前先比一次「解码出来的驻留信道」和「关联信道」，
   不一致直接 `-ECHRNG` 退出，判据阶梯因此多一个状态：**信道问题再也不会被报成
   `-ENODATA`**。RF `0x18` 低 8 位是信道号（`0x1c01`→1、`0x1c0d`→13），两条路径必须一致，
   否则解码结果记 0。BB `0x0734` 的 [23:16] 只打印不参与判定：原厂
   `halbb_8852b_api.c:1415` 用 `0x0ff0000` 掩码写 `halbb_ch_idx_encode()` 的结果，而这个
   函数体不在缓存的原厂子集里，run 36 在信道 13 上读到 `0x0d` 是一次观测，不是解码规则。
3. **帧分类。** run 36 的 `frames=0xb beacons=0x0 parse-err=0x0`（收到 11 帧、一个 Beacon
   都不是、也没有解析错误）当时无法进一步追。现在按 FC 的 type 位分四类计数，另记「A2 是
   本机」的自发帧、数据帧、管理帧里没被命名的子类型掩码、完全没分类的帧数，以及第一帧没分
   类帧的前 16 字节。顺带修掉一个死计数器：`k1_rtl8852bs_runtime_mgmt_parse()` 只在管理帧
   的分支之后才填 `bssid`/`bssid_valid`，所以数据帧的 `from_target` 永远为假、
   `data-target=` 永远是 0；现在 `from_target` 也接受 `addr2`/`addr3` 命中 BSSID。

#### 板上结果二：run 37 撞上 sweep 完成判定的一个竞态

run 37（`out/k1-serial/k1-wpa-20260830T224144Z.log`）没走到驻留窗口，但它把上面所有步骤
又跑了一遍**并且全过**，包括在另一个 BSSID 上完成的四次握手：

```
1643 auth target bssid=504f3be2e6d2 channel=0x1 beacons=0x6 ssid-len=0x2
2761 assoc request tx channel=0x1 bytes=0x46 ssid-len=0x2 cap=0x431 rsn=0x1 sn=0x7 status=0x0
2763 wpa msg2 tx bytes=0x99 sn=0x8 status=0x0
2765 wpa msg4 tx bytes=0x83 sn=0x9 status=0x0
2881 assoc exchange rsp=0x1 status=0x0 aid=0x1 auth-rsp=0x1 beacons=0x1d bss=0x3 \
     wpa=0x1 msg1=0x1 msg2=0x1 msg3=0x1 mic=0x1 msg4=0x1 bssid=504f3be2e6d2
```

`mic=0x1` 是 AP 第三帧的完整性校验通过，也就是密码学那一整条链在 run 36 之外又独立成立了
一次——而且这次的对端是**另一个 BSSID**：run 36 关联的是隐藏 SSID 的 `564f3be2e6d2`，
run 37 关联的是同一 ESS 里带 RSN 元素的 `504f3be2e6d2`（两者与 `6413abdbf628` 都在 run 37
的 BSS 表里）。目标是扫描表里第一个可用 BSS，所以两轮之间会变，而 park 步骤用的是关联那一
步自己的信道，跟着变。

失败在装钥匙之前的确认 sweep：

```
3105 passive scan walk enter-mask=0x1fff next=0x1ff9 fw-next=0x6 end=0x1
3106 passive scan-offload wait error=0x6e C2H=0xf done-ack=0x1 scan-events=0xe \
     enter-mask=0x1fff next=0x1ff9 fw-next=0x6 end=0x1 last=ch0xd reason=0x5
3158 passive scan-offload error=0x6e firmware-return=0x0
3159 station WPA2 error=0x6e                     <- ETIMEDOUT
```

**这一行的四个输入全部满足完成条件**：`enter-mask=0x1fff` 十三个信道全进过，`end=0x1` 见到
scan-end，`done-ack=0x1`，`next | fw-next = 0x1ff9 | 0x6 = 0x1fff` 十三个 dwell 全退掉。
run 36 的同一次 sweep 打的是逐字节相同的 walk 行，然后成功了。差别只在 C2H 的到达边界：

```
run 36  3062 C2H ch=0xd reason=0x3      3067 next-channel ch=0xd      3068 C2H ch=0xd reason=0x5
run 37  3065 C2H ch=0xd reason=0x3      3066 C2H ch=0xd reason=0x5    3068 next-channel ch=0xd
```

完成判定原来**只写在 C2H 分派的分支里**。run 37 的最后一次 channel-enter 和 scan-end 落在
同一个接收聚合里，所以判定是在信道 13 那一位还没置上时做的（`0xfff ≠ 0x1fff`）；退掉最后
一个 dwell 的是循环头上的 dwell 轮询，发生在下一轮，而**scan-end 之后不会再有 C2H**，于是
再也没有人回头看一眼，1000 个轮询预算烧完报 `-ETIMEDOUT`。run 36 只是两个事件恰好落在两次
读里。这是本端 sweep 等待循环里一个一直存在的竞态，run 37 是第一次撞上。

修法：把判定提出来变成 `k1_rtl8852bs_runtime_scanofld_walk_done()` ＋
`k1_rtl8852bs_runtime_scanofld_complete_check()`，**每一轮轮询都判两次**——dwell 轮询之后
（这时最后一个 dwell 刚被退掉，且此后 FIFO 空也能靠 drain 收尾，不会再烧预算）、帧处理循环
之后（覆盖 C2H 事件）。判定内容一字未改，parked sweep 仍然只要 done-ack ＋ scan-end。
另外 `match` 里记下第一次判定成立的轮询序号，`passive scan walk` 和超时行都打
`complete-at=`／`polls=`：**「走完了但 drain 没做完」和「根本没走完」以后是两个可区分的
读数**，而不是同一个 `-ETIMEDOUT`。

#### 板上结果三：run 38 把本节的判据跑齐了

串口 `out/k1-serial/k1-wpa-20260830T230957Z.log`，35 条 `--require-*` 全过，其中包含
`--require-runtime-resident`。关联仍然落在 `SB` 的 `504f3be2e6d2`（`aid=0x1 status=0x0
wpa=0x1 msg1=0x1 msg2=0x1 msg3=0x1 mic=0x1 msg4=0x1`），port init、四次握手、装钥匙都在
原位一次过。

先看竞态修没修掉。两趟 unparked sweep（`enter-mask=0x1fff`）这次都判成了完成：

```
passive scan walk enter-mask=0x1fff next=0x1ffe fw-next=0x1 end=0x1 complete-at=0x77f polls=0x7a0
passive scan walk enter-mask=0x1fff next=0x1ff1 fw-next=0xe end=0x1 complete-at=0x715 polls=0x734
```

`complete-at` 每次都比 `polls` 小一小段——判定成立之后还剩几十轮在做 drain，正是新读数要
区分的那两件事；`-ETIMEDOUT` 一次没有。

再看新加的 park 步骤。装完钥匙时射频确实停在确认 sweep 留下的信道 13 上，park 把它带回 1：

```
resident channel park-before … bb0734=0x000d0000 … rf18-a=0x1c0d rf18-b=0x1c0d
resident park before parked=0xd bb-ch=0xd target=0x1 sweep=0x1
resident park after  parked=0x1 bb-ch=0x1 target=0x1 sweep-err=0x0 bss=0x8 bcn-target=0x1e
RTL8852BS2 station resident park complete
```

`sweep=0x1` 是「需要一趟停驻 sweep」，`bcn-target=0x1e` 是这趟 sweep 自己收到的 30 帧目标
Beacon；两个信道读法（RF `0x18` 低八位、BB `0x0734` [23:16]）一致地从 13 变成 1。

然后是这一节真正要的那三秒。窗口自己不开 sweep，只把三个接收滤波器放宽再放回去：

```
resident window enter channel=0x1 ssid-len=0x2 window-ms=0xbb8
resident window parked=0x1 bb-ch=0x1 target=0x1
scan RX filter before ce20=0xf0170001 ce30=0x00000000
scan RX filter scan   ce20=0xf017000f ce30=0x55550055
resident probe tx sn=0xc bytes=0x2c status=0x0
scan RX filter after  ce20=0xf0170001 ce30=0x00000000
resident window channel=0x1 polls=0xd23 rx-reads=0x264 frames=0x6b beacons=0x5c
  bcn-target=0x2a probes=0x1 probe-status=0x0 probe-rsp=0x4 probe-rsp-self=0x1
  data-target=0x4 deauth=0x0 reason=0x0 parse-err=0x0 oversize=0x0 ch-stable=0x1
  rx=0x0 filter=0x0
resident window frames total=0x6b mgmt=0x65 ctrl=0x0 data=0x6 ext=0x0 data-all=0x6
  self-tx=0x0 mgmt-other=0x5 subtypes=0x10 unclassified=0x5
resident window unclassified head=40000000ffffffffffff02d1d4dc3116
```

读数逐条对上判据：3363 轮轮询里 612 次读到东西，107 帧，92 帧 Beacon——其中 42 帧来自关联
的那个 AP；一帧定向 Probe Request 发了出去（`status=0x0`），回来 4 帧 Probe Response，其中
1 帧的 A1 是本机（`probe-rsp-self=0x1`）；`ch-stable=0x1` 是进出两次采样的信道寄存器逐个
相等（`resident-enter` 与 `resident-exit` 都是 `rf18-a/b=0x1c01`、`bb0734=0x00010000`），
`filter=0x0` 是三个滤波器都放回了原值（`ce20`／`ce30` 前后一致），`deauth=0x0` 是 AP 没有
把这条关联踢掉，`parse-err=0x0 oversize=0x0` 是 107 帧全部解析成功。**「信道是本端驻留的」
这句话到这里才有证据**：没有任何 dwell 在跑，本端自己放宽滤波器就持续收到目标 Beacon，
并且在 dwell 之外完成了一次定向 Probe Request/Response。

`unclassified=0x5` 那 5 帧不是异常：头 16 字节 `40 00 0000 ffffffffffff 02d1d4dc3116` 是另
一台 station 广播的 Probe Request（`FC=0x0040`，A2 是一个本地管理位置 1 的随机 MAC），滤波器
放宽之后本来就该收到——它顺带说明收到的确实是空口上的第三方帧，不是本端自己的回环。

还有一条读数是写这一节时没预料到的，下一步会用上：`data=0x6`，其中 `data-target=0x4`——
关联的那个 AP 在这三秒里发了 4 帧数据帧过来（滤波器放宽之后能收到的，多半是它转发给整个
BSS 的组播）。本端只按帧头数了个数，**没有**去看硬件有没有用装进去的 GTK 把它们解开：
RX 描述符的 `HW_DEC`／`ICV_ERR`（DW3 BIT(2)／BIT(10)）和 `SEC_TYPE`（DW7 [20:17]）本端还
没解码。

#### 还没做的

**这仍然不是一条通的链路。** run 38 全过，证明的也只是「不跑扫描时收发循环还活着」：
发送描述符的安全字段仍然没填，本端仍然没有发出过一帧被 CCMP 保护的帧；窗口里收到的那 6 帧
数据帧只按帧头数了个数，硬件到底有没有用装进去的钥匙解开它们，本端还没读 RX 描述符的解密
状态位，所以也不算证据。`wlan0` 的行为一个字节没变，没有 DHCP、没有联网。密钥的三层处理照旧（profile 留空、构建脚本从 `~/.config/k1-wifi-psk.env`
读、`out/k1-wpa` 不发布），这一步新增的打印里没有任何由密钥派生的值。仍然是 RAM-only：
eMMC / SPI flash / eFuse / U-Boot 环境一个都没写。

#### 下一步

1. 解码 RX 描述符的解密状态位——`HW_DEC`（DW3 BIT(2)）、`ICV_ERR`（DW3 BIT(10)）、
   `SEC_TYPE`（DW7 [20:17]）、`SEC_CAM_IDX`（DW5 [7:0]），把驻留窗口里那几帧来自目标 AP
   的数据帧按这四个读数分类打出来。**已实现并已上板，见下一节增量 3i（前半），run 39 的读数就是
   想要的那一种。**这是**一帧都不用发**就能拿到的证据：硬件若报
   `HW_DEC=1 ICV_ERR=0 SEC_TYPE=6`，装进去的 GTK 就真的在解组播帧，`sec_ent_mode`、
   ADDR_CAM 的槽号、安全 CAM 的那三十二字节一次全被证明。注意 `SEC_TYPE` 在 DW7，
   只有长描述符（32 字节，`descriptor0` BIT(31)）才有，本端的解析器目前只留了
   `descriptor0`／`descriptor3`（增量 3i 的前半）。
2. 发送描述符的安全字段（`sec_type` / `sec_cam_idx`）——密钥在 CAM 里、槽也有了、信道是
   驻留的、port 是使能的，缺的只是发送路径去引用安全 CAM index（增量 3i 的后半）。
   字段与取值现在都有出处：WD info dword2（描述符偏移 `24+8`）的 `sec_type` [12:9]、
   `sec_hw_enc` BIT(8)、`sec_cam_idx` [7:0]（原厂 `trx_desc_8852b.c:295-297` ＋
   `txdesc.h:136-140`，与 mainline `RTW89_TXWD_INFO2_SEC_*` 逐位相同），`sec_type` 取的就是
   安全 CAM 那一项的 `type`（mainline `core.c` 的 `rtw89_core_tx_update_sec_key()` 与
   `cam.c:433` 用同一个 `enum rtw89_sec_key_type`），CCMP-128 = 6，正是本移植已经写进安全
   CAM 的那个值；`MAC_TXD_OFLD_HW_ENC_CCMP128 = 0x8` 属于另一张表，不要用。另外 8852B 的
   `hw_sec_hdr = false`（mainline `rtw8852b.c:1007`，`cam.c:523` 因此给密钥加
   `IEEE80211_KEY_FLAG_GENERATE_IV`）：**CCMP 头那 8 字节要主机自己拼进帧里，硬件只加密并在
   尾部追加 8 字节 MIC，描述符里的帧长不含这 8 字节**。
3. `rtw_hal_bb_dm_init` / `rtw_hal_rf_dm_init`（DACK/RCK/IQK/DPK/TSSI）那一批，以及把
   认证／关联响应的前 32 字节原样打到串口这条一直没补的证据。

### 增量 3i（前半）：读 RX 描述符的解密状态位——一帧不发，就能说清装进去的 GTK 有没有在解密

（**run 39 已上板：装进去的 GTK 确实在硬件里解 AP 的组播，读数见下面「上板读数」**）

run 38 的驻留窗口报了 `data=0x6`、其中 `data-target=0x4`：关联的那个 AP 在那三秒里往它自己
的 BSS 里发了 4 帧数据帧，本端收到了，但只按帧头数了个数。而装进硬件的 TK 与 GTK 到那一步
为止的全部证据，仍然只是「固件那四条命令都 ack 了」——硬件到底有没有拿这两把钥匙做事，一个
字节的读数都没有。

答案就写在这些帧自己的接收描述符里，而且读它**一帧都不用发**：AP 转发给整个 BSS 的组播帧，
按 IEEE 802.11 clause 12 是用组密钥保护的，也就是本端刚装进去的那把 GTK。所以那 4 帧的描述
符是这个问题最直接的读数——不需要本端先具备发送保护帧的能力（那是增量 3i 的后半），也不需要
再跑一次握手。

#### 描述符里的字段，以及出处

先把出处列全，因为这一步的结论全部建立在「这些位就是这个意思」上面：

- DW3：`A1_MATCH` BIT(0)、`SW_DEC` BIT(1)、`HW_DEC` BIT(2)、`CRC32_ERR` BIT(9)、
  `ICV_ERR` BIT(10)、`WITH_LLC` BIT(25)。原厂 `rtw89_txrx.h:168-221`，mainline
  `rtw89_core_rx_parse_rxdesc_v0()`（`core.c:4133-4159`）逐位相同。其中 BIT(9)／BIT(10)
  本移植早就在解了，另外四位是这一步补上的。
- DW5：`SEC_CAM_IDX` [7:0]、`ADDR_CAM` [15:8]、`MACID` [23:16]、`ADDR_CAM_VLD` BIT(28)。
- DW7：`SEC_TYPE` [20:17]，取值是 `mac_ax_enc_alg`——和安全 CAM 那一项自己的 `type` 字段
  同一套编码，所以 CCMP-128 读回来应当是 6，正是本移植写进安全 CAM 的那个值。
- 「解开了」这个判断照抄 mainline 自己的式子（`core.c:4399-4401`）：`hw_dec` 成立、
  `sw_dec` 不成立、`icv_err` 不成立。三个条件缺一不可：`sw_dec` 的意思是硬件把帧原样交给
  软件去解，那恰恰说明它没解。
- mainline 不设任何「IV 或 MIC 已被剥掉」的标志，所以**一帧被硬件解开的帧仍然带着自己那
  8 字节 CCMP 头和尾部的 8 字节 MIC**：明文（LLC/SNAP 头 `aa aa 03 00 00 00`）是从
  802.11 头 + 8 开始的，不是从 802.11 头开始的。
- DW5 与 DW7 只存在于长描述符（32 字节，由 DW0 BIT(31) 宣告）里；短描述符（16 字节）没有
  这两个 dword，读它们会读到别的帧的字节。

#### 改了什么

1. `struct k1_rtl8852bs_rx_frame_s`（`chip/k1/k1_rtl8852bs_gpl.h`）多了 `descriptor5`、
   `descriptor7` 两个原始 dword，`descriptor_long` 说明这两个字段到底存不存在（这样
   「字段缺失」永远不会被读成「字段为零」），以及解好的 `a1_match`／`sw_dec`／`hw_dec`／
   `with_llc`／`sec_type`／`sec_cam_index`／`addr_cam_index`／`mac_id`／`addr_cam_valid`。
2. `K1_RTL8852BS_RXDESC_*` 补齐上面列的那些位与位段，宏就放在原有的 `CRC_ERROR`／
   `ICV_ERROR` 旁边，注释里写清原厂与 mainline 的出处。
3. `k1_rtl8852bs_runtime_rx_parse()` 在 `descriptor_length >= …_LONG_SIZE` 时才去读
   `+20`（DW5）和 `+28`（DW7）——长度检查本来就已经保证了长描述符的 32 字节整个落在这次
   传输里，所以这里不需要再加边界判断，只需要区分描述符的两种形态。
4. 新增 `k1_rtl8852bs_runtime_resident_observe_security()`，在驻留窗口的接收循环里
   **放在现有的 `!crc_error && !icv_error` 门之前**调用。这是有意的：一帧被保护的帧
   ICV 校验失败，本身就是关于那把钥匙的读数（硬件试了，算出来的码和 AP 的不一样），
   把它当坏帧丢掉恰好丢掉了想要的证据。只统计数据帧——本次关联交换的管理帧按定义就是不
   保护的，控制帧没有载荷可保护。
5. 另配一个 `k1_rtl8852bs_runtime_resident_header_length()`：24 字节起步，两个方向位都置
   位就多 6 字节的第四地址，QoS 子类型多 2 字节，再带 order 位多 4 字节 HT control。明文
   起点要靠它算，不能假定 24。
6. 三条打印，加首帧的 head dump（16 进制，`k1_rtl8852bs_scanofld_log_bytes()`）：

   ```
   resident window data sec total= target= prot= group= a1-match= hw-dec= sw-dec= icv= crc= dec=
   resident window data sec llc-iv= llc-plain= short= desc-long= desc-short= sec-type-mask= cam-mask=
   resident window data sec first len= hdr= prot= dw3= dw5= dw7= sec-type= cam= addr-cam= macid= cam-vld= llc=
   resident window data sec first head=<32 字节>
   ```

   `dec` 就是上面那个 mainline 式子的计数。`llc-iv`／`llc-plain` 是**不看描述符**的独立旁
   证：解开的帧 LLC/SNAP 在头 + 8，没解开的帧那两个位置都是密文，所以哪个位置有
   `aa aa 03 00 00 00` 这件事本身就能回答同一个问题。`short` 是「帧太短，两个位置都读不
   了」的计数，免得两个零被读成「看过了，都没有」。两个 mask 各是一位一个取值（cipher 与
   密钥槽），它们只可能由长描述符填上，所以旁边就是两种描述符形态的计数。首帧样本优先留
   「来自目标 AP 且被保护」的那一帧，没有这样的帧才退回留第一帧数据帧。

#### 上板要看什么

这一步没有唯一正确的读数，但每种读数的含义是确定的：

- `hw-dec=0x4 sw-dec=0x0 icv=0x0 dec=0x4 sec-type-mask=0x40 cam-mask=0x2 llc-iv=0x4
  llc-plain=0x0`——想要的那一种。`sec-type-mask=0x40` 即只出现过 `sec_type=6`
  （CCMP-128），`cam-mask=0x2` 即只用过安全 CAM 第 1 项，也就是 GTK 那一项（TK 在第 0 项，
  只有单播帧才会用到它）。这一组读数一次证明：GTK 的那 32 字节、`sec_ent_mode` 的取值、
  ADDR_CAM 里回填的密钥槽号、以及 4 帧组播的解密，全都对。
- `hw-dec=0x0 sw-dec=0x4`——硬件把帧原样交出来了，说明它没把这些帧和装进去的密钥对上。
  该查 ADDR_CAM 里回填的槽号和组密钥那一项的 `type`／`ext_key`，不是查密钥本身的字节。
- `hw-dec=0x4 icv=0x4 dec=0x0`——硬件用了钥匙，但算出来的完整性码和 AP 的不一致：这才是
  「GTK 的字节错了」或「AES key unwrap 解错了」的读数。
- `prot=0x0`——这几帧根本没被保护（例如 AP 发的是不保护的组播管理类流量）。那这一轮什么也
  没证明，需要另一个窗口。
- `total=0x0`——三秒里 AP 一帧组播都没发。这不是失败，只是没赶上。
- `a1-match` 不用来判定任何事：组播帧的 A1 是组地址而不是本端地址，这个数只是把读数留下。

**故意不加 `--require-*` 开关。** 三秒的窗口里 AP 有没有发组播完全由 AP 决定，run 38 里恰
好有 4 帧，下一次可能是 0 帧。把它写成判据会让 smoke 变成偶发失败的那种测试，而本移植一直
的规矩是判据只放确定性的东西。读数照打，结论由人读——这一节列的五种读数就是读法。

#### 上板读数（run 39）

run 39 打回来的四条：

```
resident window data sec total=0x2 target=0x2 prot=0x2 group=0x2 a1-match=0x0 hw-dec=0x2 sw-dec=0x0 icv=0x0 crc=0x0 dec=0x2
resident window data sec llc-iv=0x2 llc-plain=0x0 short=0x0 desc-long=0x2 desc-short=0x0 sec-type-mask=0x40 cam-mask=0x2
resident window data sec first len=0x1a0 hdr=0x18 prot=0x1 dw3=0x00200004 dw5=0x10000001 dw7=0x000c7b79 sec-type=0x6 cam=0x1 addr-cam=0x0 macid=0x0 cam-vld=0x1 llc=0x0
resident window data sec first head=0842000001005e7f0001504f3be2e6d224a3f050081f506662a900604f000000
```

这就是上一节列的五种读数里的第一种，只是这次的窗口里 AP 发了 2 帧组播而不是 run 38 的 4 帧：

- `total=0x2 target=0x2 prot=0x2 group=0x2`——两帧数据帧全部来自目标 BSS、全部置了
  Protected 位、全部是组播。
- `hw-dec=0x2 sw-dec=0x0 icv=0x0 crc=0x0 dec=0x2`——两帧都是**硬件**解的，硬件一次都没有
  把帧原样交回主机，完整性码一次都没有算错。`dec` 是 mainline 那个式子
  （`hw_dec && !(sw_dec || icv_err)`）的计数，它和 `hw-dec` 相等。
- `sec-type-mask=0x40`——出现过的 cipher 只有第 6 号一个，即 CCMP-128，正是本移植写进安全
  CAM 的那个值。
- `cam-mask=0x2 first cam=0x1 cam-vld=0x1`——用到的安全 CAM 项只有第 1 项一个，也就是 GTK
  那一项（TK 在第 0 项，只有单播帧才会用到）。ADDR_CAM 命中的是第 0 项
  （`addr-cam=0x0 macid=0x0`），也就是关联时建的那一项。
- `llc-iv=0x2 llc-plain=0x0 short=0x0`——**不看描述符**的那条独立旁证同样成立：两帧的
  `aa aa 03 00 00 00` 都出现在 802.11 头 + 8 处，即 CCMP 头之后，没有一帧的明文 LLC 直接
  贴在头后面。描述符的位和帧的字节各自说了一遍同一件事。
- `desc-long=0x2 desc-short=0x0`——两帧都是 32 字节长描述符，所以 DW5／DW7 是真读到的字段
  而不是被当成零的缺省值。
- 首帧样本自证：`head` 以 `0842` 开头（数据帧、Protected、FromDS），A1 是
  `01:00:5e:7f:00:01`（IPv4 组播），A2 是 `50:4f:3b:e2:e6:d2`（AP 自己），`len=0x1a0`
  的 416 字节里 `hdr=0x18` 是 24 字节头。这是 AP 往 BSS 里转发的真实下行组播，不是本端的
  回环。

一次读数同时证明了四件此前只有「固件 ack 了」这一种证据的事：AES key unwrap 解出来的 GTK
那 16 字节是对的、安全 CAM 第 1 项的 32 字节写对了、ADDR_CAM 里回填的 `sec_ent_mode` 与组
密钥槽号（槽 2 → CAM 1）对得上、以及硬件的安全引擎确实在按这套配置工作。`a1-match=0x0`
如上一节所说不用来判定任何事：组播帧的 A1 是组地址。

#### 构建

- 带密钥：`3b54762da7587ad8f90d30b7b421833422a901e3371d7e7e5581251a7c35b2fd`
  （`--clean` 与增量两次构建同一个哈希）
- `--no-key`：`db88a807143d07d29cc92fd367a597cc1d602b96dd82048db6784534d9a3ab82`

顺带修掉了 `tools/build_k1_wpa.sh` 的一个真问题，它差点让上面这两个哈希写错：底层的
`build.sh` 只在构建目录还没有 `.config` 时才跑配置步骤，所以在同一个构建目录里从生成的
profile 切到提交的 profile（也就是加 `--no-key`）时，配置根本不会重新生成——`--no-key`
会把**带密钥的**镜像增量重建一遍，然后把它的哈希当成 no-key 的哈希报出来，恰好是这个开关
存在的意义要排除的那种假结果。现在脚本把「上次是用哪个 defconfig 配的」记在
`${BUILD_DIR}/.k1-wpa-config-source` 里，来源一变就强制 `--clean`。已经两个方向都验过：
切换时打印 `Reconfigure: …`、哈希随之改变；不切换时不打印、哈希不变。

#### 还没做的

**这仍然不是一条通的链路。** 接收侧的读数已经拿到（上面那四条），但发送侧的安全字段仍然没
填，本端仍然没有发出过一帧被 CCMP 保护的帧（增量 3i 的后半）；这一步只是让接收侧有能力说出
硬件对收到的保护帧做了什么。`wlan0` 的行为一个字节没变，没有 DHCP、没有联网。密钥的三层
处理照旧（profile 留空、构建脚本从 `~/.config/k1-wifi-psk.env` 读、`out/k1-wpa` 不发布），
这一步新增的打印里没有任何由密钥派生的值——打的是描述符的位、cipher 编号、CAM 槽号和一帧
密文的前 32 字节。仍然是 RAM-only：eMMC / SPI flash / eFuse / U-Boot 环境一个都没写。

### 增量 3i（后半）：发送侧的安全字段与 CCMP 头——只在内存里，一帧不发

（**run 40 已上板：两行读数与写死的字面量逐位相同，读数见下面「上板读数」**）

run 39 已经证明硬件拿着装进去的 GTK 在解 AP 的组播帧。反过来那一半——本端发一帧被 CCMP 保护
的帧——缺的东西只有两样：发送描述符里引用安全 CAM 的那一个 dword，和 802.11 头之后那 8 字节
CCMP 头。这一步把这两样都实现出来并逐字段验证，但**不发送任何帧**：全部是内存操作，期望值写
成字面量，一个错位的移位不可能靠自己和自己一致而混过去。

#### 出处（先原厂，mainline 只用来对照）

- WD info dword2，描述符偏移 `24 + 8`：`sec_cam_idx` [7:0]、`sec_hw_enc` BIT(8)、
  `sec_type` [12:9]。原厂 `trx_desc_8852b.c:293-299` 组这个 dword，位段来自
  `txdesc.h:131-140` 的 `AX_TXD_SEC_CAM_IDX_SH 0 / MSK 0xff`、`AX_TXD_SEC_HW_ENC BIT(8)`、
  `AX_TXD_SECTYPE_SH 9 / MSK 0xf`，与 mainline 的 `RTW89_TXWD_INFO2_SEC_*` 逐位相同。该
  dword 的其余字段（lifetime selector [15:13]、A-MPDU density [20:18]）本端保持为零。
- `sec_cam_idx` 不是自己编的号，而是 ADDR_CAM 那一项对应密钥槽里存的值：原厂
  `security_cam.c:476` 是 `*sec_cam_idx = role->info.a_info.sec_ent[key_index]`。本移植
  TK 用槽 0 → 安全 CAM 第 0 项，GTK 用槽 2 → 第 1 项，正是 run 39 的接收描述符回读出来的
  `cam=0x1`。发送侧和接收侧因此用的是同一个编号体系，这一点已经被硬件读数校对过一次。
- `sec_type` 用 `mac_ax_enc_alg` 的编号，CCMP-128 = 6，就是本移植写进安全 CAM 的那个值。
  `MAC_TXD_OFLD_HW_ENC_CCMP128 = 0x8` 属于固件 offload 的另一张表，不要用。
- 8852B 的 `hw_sec_hdr = false`（mainline `rtw8852b.c:1007`，`cam.c:523` 因此给密钥加上
  `IEEE80211_KEY_FLAG_GENERATE_IV`）：**CCMP 头那 8 字节由主机拼进帧里**，硬件只做加密并在
  尾部追加 8 字节 MIC，描述符里的帧长不含这 8 字节 MIC。

#### 做了什么

1. `K1_RTL8852BS_MGMT_TXI_SEC_*` 与 `K1_RTL8852BS_CCMP_*` 两组宏，注释里写清上面每一条出处。
2. `k1_rtl8852bs_runtime_ccmp_header_build()`：按 802.11-2016 §12.5.3.2 摆
   PN0、PN1、Rsvd、`KeyID|ExtIV`、PN2..PN5。这个头里的 PN **不是**连续的六字节——低两字节
   在最前，中间隔着一个保留字节和一个 `KeyID<<6 | 0x20` 字节，高四字节在后，所以它值得单独
   一个函数和单独的断言。拒绝 PN 0（CCMP 的 PN 从 1 起算）、超过 2^48−1 的 PN、大于 3 的
   key id、不足 8 字节的缓冲区和 NULL。
3. `k1_rtl8852bs_runtime_mgmt_tx_build()` 多一个 `security` 参数：为 NULL 时 dword2 写零，
   非 NULL 时写 `(sec_type << 9) | BIT(8) | sec_cam_idx`，并在写完后按位读回校验。现有两处
   调用（管理帧发送）都传 NULL——本移植至今发出去的每一帧管理帧都是不保护的，这个 dword 必须
   保持为零。
4. 新诊断 `k1_rtl8852bs_fwdl_runtime_tx_security_diagnostic()`，挂在已有的
   `CONFIG_K1_RTL8852BS2_RUNTIME_DATA_TX_DIAGNOSTIC` 下（同样是纯内存操作，不值得再加一个
   Kconfig 开关），打印 `runtime TX security …` 与 `runtime TX security ccmp=` 两行，成功后
   打 `RTL8852BS2 runtime TX security fields complete`。
5. `tools/run_k1_wireless_smoke.py` 加 `--require-runtime-tx-security` 判据，
   `tools/run_k1_scanofld_active.sh` 里也加上——它是纯内存断言，不像组播那样看 AP 的心情，
   所以这一条**可以**做判据。

#### 断言（期望值都是字面量）

| 输入 | 期望 |
| --- | --- |
| CCMP 头，PN 1，key 0 | `01 00 00 20 00 00 00 00` |
| CCMP 头，PN `0x0000fedcba98`，key 2 | `98 ba 00 a0 dc fe 00 00` |
| CCMP 头，PN 0 ／ PN `2^48` ／ key 4 ／ 7 字节缓冲 | 各返回 `-EINVAL`，且不写缓冲区 |
| 描述符，`{CCMP128, 安全 CAM 0}`（TK） | dword2 = `0x00000d00` |
| 描述符，`{CCMP128, 安全 CAM 1}`（GTK） | dword2 = `0x00000d01` |
| 描述符，`security = NULL` | dword2 = `0` |
| 描述符，`sec_type = 0` ／ `sec_type = 16` | 各返回 `-EINVAL` |

`0xd00` 就是 `6 << 9 | BIT(8)`：cipher 6 落在 [12:9]、硬件加密位 8、CAM index 在低字节。
`sec_type = 0` 被拒是有意的——「不加密」这件事要用 `security = NULL` 表达，不能用一个零 cipher
混在保护路径里；`sec_type = 16` 被拒是因为它会溢出 [12:9] 去改掉旁边的 lifetime selector。

#### 上板读数（run 40）

`out/k1-serial/k1-wpa-20260831T010437Z.log`，36 条 `--require-*` 判据全过，
`PASS: K1 wireless RAM image reached NSH`：

```
K1 Wi-Fi GPL: runtime TX security info2-tk=0x0000000000000d00 info2-gtk=0x0000000000000d01 info2-plain=0x0000000000000000 sec-type=0x0000000000000006 hdr=0x0000000000000008 mic=0x0000000000000008 len=0x0000000000000040
K1 Wi-Fi GPL: runtime TX security ccmp=98ba00a0dcfe0000
K1 Wi-Fi GPL: RTL8852BS2 runtime TX security fields complete
```

- `info2-tk=0xd00`、`info2-gtk=0xd01`、`info2-plain=0x0`：描述符 dword2 的三种形态和上面
  那张表里的字面量逐位相同。`0xd00 = 6 << 9 | BIT(8) | 0`，`0xd01` 只差低字节的 CAM index，
  不保护的帧写零——也就是说 cipher 落在 [12:9]、硬件加密位在 8、CAM index 在低字节这三件事
  在真机上编出来的确实是这三个数，不是主机端算错又自己对上。
- `sec-type=0x6`：写进描述符的 cipher 编号是 `mac_ax_enc_alg` 的 CCMP-128，与 run 39 从
  **接收**描述符 DW7 读出来的 `sec-type=0x6` 是同一个编号；`cam=0x1` 也和 run 39 的接收读数
  对上。发送侧引用安全 CAM 的方式因此不只是「按原厂抄的」，而是被硬件自己的回读校对过。
- `hdr=0x8`、`mic=0x8`、`len=0x40`：CCMP 头 8 字节由主机拼、MIC 8 字节由硬件追加、描述符里
  的帧长 `0x40 = 64 = 24 + 8 + 32` **不含** MIC——8852B 的 `hw_sec_hdr = false` 这条约定在
  代码里落成了这三个数。
- `ccmp=98ba00a0dcfe0000`：PN `0x0000fedcba98`、key id 2 的 CCMP 头。低两字节 `98 ba` 在
  最前，然后是保留字节 `00` 和 `KeyID<<6 | ExtIV = 2<<6 | 0x20 = 0xa0`，最后才是高四字节
  `dc fe 00 00`。这一行就是「PN 不是连续六字节」这件事的现场证据。

同一次 run 的驻留窗口顺手又给了一组接收侧读数，和 run 39 的四帧样本不同：
`total=0x4 prot=0x4 group=0x4 hw-dec=0x3 sw-dec=0x1 icv=0x0 crc=0x0`、
`sec-type-mask=0x41 cam-mask=0x3`。四帧保护组播里有三帧硬件解了、一帧被交上来没解
（`sw-dec`，`sec-type=0`／`cam=0`），说明这一帧的密钥硬件手里没有——附近另一个 BSS 的组播落进
了嗅探模式的收包口。这不是回归：`sec-type-mask` 里除了 CCMP-128 的 bit 6 只多了「无加密」的
bit 0，被解的那三帧仍然是 `cam=0x1`（GTK 那一项）。

#### 还没做的

**这一步一帧都没发，所以它还不是「本端能发保护帧」的证据。** 缺的是数据队列那一路的发送：把
CCMP 头拼进帧、把帧长按「不含 MIC」算好、走数据 DMA 通道提交，并观察 AP 是否回应（增量 3j，
候选首帧是 DHCP Discover，因为驻留窗口已经在数 `data_frames_to_self`）。PN 也还只是函数参数，
没有每密钥的单调计数器。`wlan0` 的行为一个字节没变，没有 DHCP、没有联网。密钥的三层处理照旧
（profile 留空、构建脚本从 `~/.config/k1-wifi-psk.env` 读、`out/k1-wpa` 不发布），这一步新增
的打印里没有任何由密钥派生的值——打的是 cipher 编号、CAM 槽号，和一个由字面 PN 拼出来的 CCMP
头。仍然是 RAM-only：eMMC / SPI flash / eFuse / U-Boot 环境一个都没写。

#### 构建

- 带密钥：`e2c099e601754510a068a3c5077525c4c1264d84780cd7851272e571c45f5e73`
- `--no-key`：`43aa3c70e9fd3a681a467de74c3ef68d10d5e7336e672fa4bbf0ef9c87a8683a`
  （切回带密钥后哈希回到上面那个，配置来源切换的强制 `--clean` 仍然生效）

### 增量 3j：真的往数据队列发一帧被 CCMP 保护的帧

（**尚未上板**：run 41 待跑。本节所有期望值都是离线的字面量，上板读数一栏留空，别当成硬件结论）

增量 3i 把发送侧的安全字段和 CCMP 头做完了，但一帧没发。这一步补上「发」：把 CCMP 头拼进一帧
到-DS 的数据帧、按「帧长不含 MIC」算好长度、走 band 0 的 BE 数据队列提交，并在驻留窗口里等
AP 的回应。

#### 出处（先原厂，mainline 只用来对照）

- **描述符 48 字节，不是 40。** `mac_txdesc_len_8852b()` 只要 `wdinfo_en` 就加
  `WD_INFO_LEN`，而原厂核心对**每一帧**都置 `mdata->wdinfo_en = 1`，所以 WD BODY 24 ＋
  WD INFO 24 = 48。本移植管理帧那一路用的也是 48，这里没有例外。
- **`AX_TXD_HDR_LLC_LEN` 写 20（半字节单位），不是 12。** 原厂 `get_hdr_with_llc()`
  （`trx_desc.c:53-83`）算的是 `mac_hdr_len + (with_llc ? 8 : 0) + (vlantag ? 4 : 0) +
  sec_hdr_len` 再 `/= 2`；数据帧原厂核心置 `with_llc = 1`、`sec_hdr_len = iv_len`，于是
  (24 + 8 + 8) / 2 = 20，字段位置 `_SH 11 _MSK 0x1f`。mainline 写的是
  `ieee80211_hdrlen(fc) >> 1` = 12。两个值都不影响本移植：这个字段是给 TX checksum offload
  和硬件头转换用的 L3 偏移，两样本移植都没开（`mac_tcpip_chksum_ofd()` 没调过、
  `hw_hdr_conv = 0`）。按「先原厂」的规矩写原厂的 20，并把这段理由写在宏的注释里。
- **`AX_TXD_BK` BIT(13) 要置。** 原厂恰好在 `ampdu_en == FALSE` 时置 `bk = 1`；本移植不聚合，
  所以 `AGG_EN` BIT(12) 保持零、BK 置一。
- **band 0 的 BE 队列**：`qsel = 0`、`ch_dma = 0`（ACH0），调度器使能位是 `R_AX_CTN_TXEN`
  的 BIT(0)（`B_AX_CTN_TXEN_BE_0`）。本移植此前只开过 MGQ `0x100` 和 CPUMGQ `0x400`，
  数据队列的那一位从来没开过——不开的话描述符再对也不会有帧出去。
- **速率**沿用管理帧那一路的 `USERATE_SEL | DATARATE=CCK1 | DISDATAFB`：能不能协商到更高的
  速率是另一件事，这一步要的是「发得出去」。
- **`pktlen` 含主机拼的 8 字节 CCMP 头、不含硬件追加的 8 字节 MIC**（8852B
  `hw_sec_hdr = false`，见增量 3i）。

#### 为什么首帧选 DHCP Discover

因为它的回应是**不可伪造的证据**。一个 `op = 2` 的 BOOTP 回应，`xid` 等于我们随机出来的那个、
`chaddr` 等于本端 eFuse MAC——这种帧只可能在「AP 用我们派生出来的密钥解开了我们这一帧、并把
它转给了它背后的 DHCP 服务器」之后才存在。相比之下「发出去没报错」只证明硬件收下了描述符。
驻留窗口本来就在数 `data_frames_to_self`，接收侧不用改。

#### 做了什么

1. 一组数据描述符与 DHCP／IPv4／BOOTP 的宏，注释里逐条写上面的出处。
2. `g_k1_rtl8852bs_tx_packet_number` ＋ `..._tx_packet_number_next()`：PN 从 1 起算、单调加一，
   装新密钥时（`..._wpa_arm()`）归零。3i 里 PN 还只是个函数参数。
3. `..._runtime_inet_sum()` / `..._inet_fold()`：RFC 1071 的和与折叠。折叠返回的是**反码**，
   所以把一个正确的头连同它自己的校验和再加一遍必然折成 0——这是一条移位错不可能同时满足的
   离线判据。
4. `..._runtime_dhcp_discover_build()`：FC `0x4108`（到-DS、Protected）、A1 = BSSID、
   A2 = 本端、A3 = 广播、seq、主机拼的 CCMP 头（key id 0）、LLC/SNAP `aa aa 03 00 00 00 08 00`、
   IPv4（0.0.0.0 → 255.255.255.255，TTL 64，proto 17）、UDP 68 → 67、236 字节 BOOTP ＋
   22 字节选项，共 **326** 字节。A1 是 AP 的单播地址，所以**不置** BMC 位，AP 会 ACK、硬件会重传。
5. `..._runtime_dhcp_reply_match()`：先试 `header_length + 8` 再试 `header_length` 两个偏移找
   LLC/SNAP——解密后的帧**保留** CCMP 头，明文帧没有——因此它从不解析密文；然后依次校
   IPv4 版本 4、`ihl * 4 >= 20`、proto 17、无分片、端口 67 → 68、`op = 2`、四字节 `xid`、
   `chaddr` 等于本端 MAC，最后把 `yiaddr` 交出去。
6. `..._runtime_data_secure_tx_build()`：48 字节描述符，`hdr_llc` 按上面那条算，
   `body3` 置 BK，`info2` 复用 3i 的 `(sec_type << 9) | BIT(8) | sec_cam_idx`（`security`
   为 NULL 时写零），十二个 dword 全部按位读回。
7. 新诊断 `k1_rtl8852bs_fwdl_runtime_data_secure_tx_diagnostic()`，仍然挂在已有的
   `CONFIG_K1_RTL8852BS2_RUNTIME_DATA_TX_DIAGNOSTIC` 下（纯内存），断言见下表。
8. 驻留窗口里的发送半：`..._runtime_sch_tx_en_data()` 打开 `CTN_TXEN` 的 BE 位并回读，
   `..._runtime_resident_data_tx()` 取一个新 seq 与新 PN、建帧建描述符、查 TXPG_WP 资源
   （不足回 `-ENOSPC`，不硬发）、`k1_sdio_wifi_write()` 提交、只有写成功才记计数。窗口在
   「已装 TK ＋ 已收到目标 AP 的 Beacon ＋ 还没收到 DHCP 回应」时最多发 2 次，重传**沿用第一次
   的 xid**（真实 DHCP 客户端就是这么做的，两次里哪一次被回应都认得出来）。新增两行打印
   `resident data tx sn=… bytes=… xid=… pn=… status=…` 与
   `resident window data tx sent=… status=… tk=… dhcp-reply=… offer-ip=…`。
9. `tools/run_k1_wireless_smoke.py` 加 `--require-runtime-data-secure-tx`（离线模型那行 ＋
   驻留窗口切片里的 `status=0x0` ＋ 汇总行 `sent≥1 tk=1`），`tools/run_k1_wpa.sh` 一并加上，
   判据数 36 → **37**。

#### 断言（期望值都是 python 模型离线算出来的字面量）

| 输入 | 期望 |
| --- | --- |
| DHCP Discover 总长 | `326`（`0x146`） |
| IP total length ／ UDP length | `286`（`0x11e`）／ `266`（`0x10a`） |
| IP 校验和 ／ UDP 校验和 | `0x79d0` ／ `0x5bdd`，且各自连校验和再加一遍折成 `0` |
| 帧头前 32 字节（self `02:11:22:33:44:55`，bssid `06:aa:bb:cc:dd:ee`，seq `0x123`，PN 1） | `08 41 00 00 06 aa bb cc dd ee 02 11 22 33 44 55 ff ff ff ff ff ff 30 12 01 00 00 20 00 00 00 00` |
| IP／UDP 头 28 字节 | `45 00 01 1e 00 00 00 00 40 11 79 d0 00 00 00 00 ff ff ff ff 00 44 00 43 01 0a 5b dd` |
| 描述符（保护） | `body0=0x0040a400 body2=0x146 body3=0x2123 info0=0x40000400 info1=0 info2=0xd00` |
| 描述符（不保护） | `body0=0x00408400`，`info2=0` |
| 布局 | `fifo_address=0x1002f`、`transfer_length=0x178`、8 个 PLE page、1 个 WDE page |
| 描述符负例 | header 25／header 22／frame 32／seq `0x1000`／缓冲少一字节／`sec_type=0`／`sec_type=16` 各回 `-EINVAL` |
| 建帧负例 | 缓冲 `326 - 1` 回 `-EINVAL` |
| PN 计数器 | 连续两次返回 1、2 |
| 合成回应（FC `0x4208`、192.168.1.1 → 255.255.255.255、67 → 68、`yiaddr = 192.168.1.123`、`chaddr` = 本端） | 在 `header_length` 24 与 40 两个位置都匹配，`offer-ip = 0xc0a8017b` |
| 匹配器负例 | xid 错一位／chaddr 错／长度截断／我们自己的 Discover／`op = 1`／源端口是客户端口 各不匹配 |

`body0 = 0x0040a400`：`hdr_llc = 20` 落在 [15:11]（`20 << 11 = 0xa000`）、`ch_dma = 0`、
STF_MODE 与 WDINFO_EN 在低位；`body3 = 0x2123` 是 `seq 0x123 | BK`；`0x146` 就是 326。
`transfer_length = 0x178 = 376`：374 向上取到 8 字节整数倍。合成回应放在 `header_length` 40
也匹配，是为了证明「明文帧没有 CCMP 头」那条回退路径也走得通，不是只在解密后的偏移上巧合。

#### 上板读数

留空——run 41 还没跑。上板后要看的是三件事：`resident data scheduler TX enable` 的
`after=` 里 BE 位真的置上、`resident data tx … status=0x0`（队列收下了这一帧）、以及
`resident window data tx … dhcp-reply=`。

#### 还没做的

**「队列收下了」不等于「AP 解开了」。** `--require-runtime-data-secure-tx` 只卡到
「离线模型全对 ＋ 数据队列收下了一帧、且当时确实装着成对密钥」；`dhcp-reply=` 是**报告而不是
判据**，因为一台背后没有 DHCP 服务器的 AP 会把这一帧解得好好的却永远不回。等哪一次 run 真的
拿到回应，那一行才是「本端发的保护帧被 AP 解开了」的证据，届时再决定要不要升成判据。
`wlan0` 的行为一个字节没变，没有 DHCP 客户端、没有联网——这一帧是驻留窗口自己拼出来的，不走
网络栈。密钥的三层处理照旧（profile 留空、构建脚本从 `~/.config/k1-wifi-psk.env` 读、
`out/k1-wpa` 不发布）；新增的打印里没有任何由密钥派生的值——打的是描述符的位、帧长、seq、
一个随机 xid、一个 CCMP PN 和（如果有的话）AP 提供的 IP，这些都不是凭证。仍然是 RAM-only：
eMMC / SPI flash / eFuse / U-Boot 环境一个都没写。

#### 构建

- 带密钥：`9833e8cdfe16ccc5fceb9c89fa8db03630192b0aec36b40c289afe245b4d0fcf`
  （`--clean` 与切回来的增量构建同一个哈希）
- `--no-key`：`095e1663c421d9caf5de62944881e2886c05e5e54cfb1c6b944f253796f0610f`

顺带记一个环境坑：这次第一次构建整棵树都报 `unrecognized opcode 'csrr…', extension 'zicsr'
required`。原因不是工具链，是**上一次用 ninja 单独编一个文件时触发了一次半途失败的 cmake
重配**（`ccache` 不在那个 shell 的 PATH 上），留下的 `build.ninja` 里 `-march` 退成了
`rv64imac`——F/D 和 zicsr 全丢了。`--clean` 一次就好了，两次带密钥构建哈希相同。教训是：不要
再用 ninja 直接驱动单个对象规则；要快速语法检查就从 `compile_commands.json` 里取出命令行、
去掉 ccache 前缀、加 `-fsyntax-only` 自己跑，这样不会碰构建目录的状态。

### 工具：为什么按了 RST 也常常停不进 U-Boot——0 秒 autoboot ＋ 主机读数滞后

这一段不是移植进度，是把一个从很早就在偶发、一直被当成「手速问题」的东西查清楚了，值得记下来
因为它决定了每次上板要按几次 RST。

这块板的 U-Boot 环境是从 SPI flash 读的，里面 `bootdelay = 0`。**每一份**串口日志里都是
`Autoboot in 0 seconds`——包括那些成功停下来的。也就是说它并不给出一个可以瞄准的窗口：它只在
「看的那一瞬间输入缓冲里已经有字节」时才放弃 autoboot。成功的日志的特征是紧接着下一行就是
`=> `，而且那个中止字节被吃掉、没有回显。

第二半是主机侧的：CH340 在板子复位时会重新枚举（`ttyUSB1 → ttyUSB0`），内核随后一次性交付一
大串缓冲下来的输出，所以**主机读到的位置可能落后板子几百毫秒**。任何「看到某个 marker 就发一
个字节」的做法，即使那个 marker 看起来很早，字节也可能是在 autoboot 检查之后才到——run 39b 就
是这样失败的：日志里 SPL banner、DDR 那几行、`re-opened /dev/ttyUSB0`、板上时间 2.610 的
`In:    serial`，然后写了一个 `s`，板子仍然在 3.002 跑了 `Try to boot from mmc2`。

所以修的方向不是「更准的时机」，而是**不要有时机**：`halt_at_uboot_from_serial()` 现在从看到
SPL banner 起持续写 `s`（最多 20 秒），直到读到 `=>` 为止；写失败按 `OSError`／`XmodemError`
吞掉，因为 USB 节点会在复位中途消失，下一轮轮询会顺着稳定的 `/dev/serial/by-id` 链接重开。读
到提示符之后不能马上发命令——还在飞行中的 `s` 会变成命令的前缀——所以它先排空、发 `\x15\r`
清行，确认提示符后面真的什么都没有，最多试三次。

另外，窗口一旦错过就不再是致命错误：手动复位下看到 `Try to boot` 只意味着这一次没赶上，工具
丢掉这次尝试、重置状态继续听，操作员**再按一次 RST** 就开出一个新窗口，不必重启工具。改完
run 39c 第一次按就停住了。
