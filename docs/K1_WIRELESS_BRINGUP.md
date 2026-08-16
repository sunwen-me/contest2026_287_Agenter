# K1 RTL8852BS2 Wireless Bring-up

更新时间：2026-08-16（Asia/Shanghai）

本文件记录 MUSE Pi Pro 板载 RTL8852BS2 的 openvela 首轮迁移边界。它只覆盖
无线供电、pinmux、Wi-Fi SDIO 卡枚举和 Bluetooth H5 链路诊断；不宣称 Wi-Fi 或
蓝牙已经可用。

## 1. 硬件与协议事实

| 功能 | K1 资源 | 依据 |
| --- | --- | --- |
| Wi-Fi | SDH1 `0xd4280800`，GPIO15--20，4-bit 1.8 V SDIO | `spacemit-com/linux-6.6` 的 `k1-x_MUSE-Pi-Pro.dts` 与 `k1-x_pinctrl.dtsi` |
| Bluetooth | UART2 `0xd4017100`，GPIO21--24 | 同一 DTS 的 `&uart2` / `pinctrl_uart2` |
| Bluetooth 协议 | H5（3-wire），115200 8E1，关闭 RTS/CTS | `spacemit-com/buildroot-ext` 的 `board/spacemit/k1/plt_overlay/etc/init.d/S40hci`：`rtk_hciattach -n -s 115200 ttyS2 rtk_h5`；`spacemit-com/rtk_hciattach` |
| RF 电源 | GPIO67，高有效 | `rf-pwrseq/pwr-gpios` |
| Wi-Fi REG_ON | GPIO116，高有效 | `wlan-pwrseq/regon-gpios` |
| Wi-Fi wake | GPIO66，输入 | `wlan-pwrseq` pinctrl |
| Bluetooth RESET_N | GPIO63，高有效 | `bt-pwrseq/reset-gpios` |

GPIO64、GPIO65 不在该板 DTS 的无线电源序列中，不能作为无线控制线使用。

官方 `rtk_hciattach` 的 H5 补丁表将 RTL8852BS 映射到
`rtl8852bs_fw` 与 `rtl8852bs_config`。这两个厂商文件不在本仓、也不能由相近型号
固件替代。

## 2. 已实现

- `chip/k1/k1_sdio.c`：SDH1 实例、4-bit SDIO capability，以及标准
  CMD0/CMD5/CMD3/CMD7 卡选择、CCCR/FBR 的 CMD52 识别读取；
- `board/k1/muse_pi_pro/src/k1_wireless.c`：GPIO15--24 pinmux、无线控制线
  时序、SDH1 探测和 Bluetooth RESET_N 释放；
- `chip/k1/k1_bt_uart.c`：UART2 的 H5 诊断器。它按官方 attach 参数配置
  115200 8E1、关闭 RTS/CTS，轮询完成 H5 `SYNC -> CONFIG`，按协商结果校验 CRC、
  确认可靠事件，并发送标准 HCI `Read Local Version Information`；
- `board/k1/muse_pi_pro/configs/wireless/defconfig`：独立试验配置，默认
  `nsh` 配置不变。

上电时序固定为：`BT_RESET_N=0`、`WLAN_REG_ON=0`、`RF_PWR=0`，等待 10 ms，
再依次置 `RF_PWR=1`（10 ms）、`WLAN_REG_ON=1`（30 ms）、
`BT_RESET_N=1`（50 ms）。GPIO66 仅配置为输入，不驱动。

Wi-Fi 卡枚举会把 CCCR 的 Bus Interface Control 设为 4-bit，并将主机切换为
4-bit 模式；随后只读 CCCR revision、SD revision、IOEN、IORDY、Bus Interface、
Card Capability 和已声明 function 的 FBR interface code。它不写 IOEN，因此
不会启用任何 WLAN function，也不会触碰 Realtek vendor register 或固件。

Bluetooth H5 诊断使用官方 attach 的 10 次、每次 500 ms 的同步窗口。只有收到并
验证 `SYNC` 响应后才发送 `CONFIG`；配置响应声明 DIC 时，后续帧会使用并验证
16-bit CRC。收到控制器主动 `SYNC` 或 `CONFIG` 请求时会回对应的 H5 控制帧。
协商完成后只发送标准 HCI `Read Local Version Information`（opcode `0x1001`），并
确认可靠的 Command Complete 事件；它不发送 HCI Reset 或任何 Realtek 厂商命令。

## 3. 明确未实现

- RTL8852BS2 Wi-Fi SDIO function 初始化、Realtek Wi-Fi firmware 下载、MAC 和
  `netdev` 注册；
- RTL8852BS2 Bluetooth 厂商 firmware/config 下载、通用 H5 可靠帧传输、HCI Reset、
  Bluetooth stack 注册及扫描验证；
- Wi-Fi/蓝牙低功耗唤醒、SDIO 中断、吞吐与长期稳定性验证；
- H5 链路诊断的实板验收。

因此，CCCR/FBR 可读只证明 SDIO 卡选择、4-bit 总线设置和标准 function 描述符
可用；`K1 Bluetooth: H5 local version ...` 只证明 UART2、电源时序、H5 基础协商
和一次标准 HCI 事件往返在该次启动中成功。两者都不代表联网、关联 AP、可扫描设备
或数据传输。

`/dev/ttyHCI0` 不会由本配置创建。NuttX 现有 `btuart_register()` 上半层只解析
H4，不能直接接收 H5 字节流；在 H5 可靠传输和厂商启动序列完成前注册它会产生错误
的设备可用性表象。

## 4. UART 风险隔离

历史实板记录表明 UART0 在 S-mode 写 IER 存在 APB 挂死风险。无线配置采取以下
隔离措施：

- 从不修改 `k1_console.c`，UART0 继续使用 U-Boot 继承的 polling console；
- UART2 仅在独立 `wireless` 配置中配置，且 IER 始终为零，不接入 PLIC；
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
