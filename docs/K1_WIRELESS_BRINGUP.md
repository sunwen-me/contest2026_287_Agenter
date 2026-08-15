# K1 RTL8852BS2 Wireless Bring-up

更新时间：2026-08-16（Asia/Shanghai）

本文件记录 MUSE Pi Pro 板载 RTL8852BS2 的 openvela 首轮迁移边界。它是
硬件供电、pinmux、SDIO 识别和 Bluetooth H4 传输的实现说明，不是“Wi-Fi 或蓝牙
已经可用”的宣称。

## 1. 硬件事实

| 功能 | K1 资源 | Linux DTS 依据 |
| --- | --- | --- |
| Wi-Fi | SDH1 `0xd4280800`，GPIO15--20，4-bit 1.8 V SDIO | `k1-x_MUSE-Pi-Pro.dts`、`k1-x_pinctrl.dtsi` |
| Bluetooth | UART2 `0xd4017100`，GPIO21--24，4-wire flow control，IRQ 44 | `k1-x_MUSE-Pi-Pro.dts`、`k1-x_pinctrl.dtsi` |
| RF 电源 | GPIO67，高有效 | `rf-pwrseq/pwr-gpios` |
| Wi-Fi REG_ON | GPIO116，高有效 | `wlan-pwrseq/regon-gpios` |
| Wi-Fi wake | GPIO66，输入 | `wlan-pwrseq` pinctrl |
| Bluetooth RESET_N | GPIO63，高有效 | `bt-pwrseq/reset-gpios` |

GPIO64、GPIO65 不在该板 DTS 的无线电源序列中，不能作为无线控制线使用。

## 2. 已实现

- `chip/k1/k1_sdio.c`：SDH1 实例、4-bit SDIO capability，以及无副作用的
  CMD5 OCR 探测；
- `board/k1/muse_pi_pro/src/k1_wireless.c`：GPIO15--24 pinmux、无线控制线
  时序、SDH1 探测、UART2 H4 注册；
- `chip/k1/k1_bt_uart.c`：UART2 的单实例 H4 lower-half，使用 NuttX
  `btuart_register()`，可在上层打开时注册 `/dev/ttyHCI0`；
- `board/k1/muse_pi_pro/configs/wireless/defconfig`：独立试验配置，默认
  `nsh` 配置不变。

上电时序固定为：`BT_RESET_N=0`、`WLAN_REG_ON=0`、`RF_PWR=0`，等待 10 ms，
再依次置 `RF_PWR=1`（10 ms）、`WLAN_REG_ON=1`（30 ms）、
`BT_RESET_N=1`（50 ms）。GPIO66 仅配置为输入，不驱动。
Wi-Fi CMD5 和 Bluetooth H4 会独立尝试；前者失败不会阻断后者，以便从单次串口
日志区分 SDIO 与 UART2 问题。板级初始化仍会返回第一项失败码。

## 3. 明确未实现

- RTL8852BS2 Wi-Fi MAC、SDIO function 初始化、Realtek Wi-Fi firmware 下载和
  `netdev` 注册；
- RTL8852BS2 Bluetooth vendor firmware 下载、HCI Reset/版本查询及扫描验证；
- Wi-Fi/蓝牙低功耗唤醒、SDIO 中断、吞吐与长期稳定性验证；
- 蓝牙 UART2 IER 的实板风险确认。

因此，CMD5 成功只证明 SDIO 从设备响应；`/dev/ttyHCI0` 出现只证明 H4 传输已
注册。两者均不代表联网、关联 AP、扫描设备或数据传输通过。

## 4. UART 风险隔离

历史实板记录表明 UART0 在 S-mode 写 IER 存在 APB 挂死风险。无线配置采取以下
隔离措施：

- 从不修改 `k1_console.c`，UART0 继续使用 U-Boot 继承的 polling console；
- UART2 在独立 `wireless` 配置中才启用，依赖已验证的 K1 PLIC；
- H4 endpoint 未被上层打开前 UART2 IER 保持关闭；
- UART2 TX 等待有 100 ms 上限，控制器未就绪或 CTS 持续阻塞时返回错误，不无限
  卡死。

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
不得执行 `saveenv`、`mmc write`、`mmc erase` 或 FDL/fastboot 写入。首轮串口
验收应依次确认：

1. 正常进入 NSH，UART0 日志未退化；
2. `K1 RTL8852BS2 bring-up failed` 没有出现，或记录其确切错误码；
3. CMD5 返回成功且 OCR 值被记录；
4. `/dev/ttyHCI0` 存在，打开/关闭不导致异常；
5. 只有在 UART2 H4 收发和厂商初始化已有独立日志后，才继续实现 Realtek 固件
   和网络功能。
