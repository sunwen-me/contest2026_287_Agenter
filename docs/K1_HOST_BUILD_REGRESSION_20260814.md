# K1 主机构建回归记录（2026-08-14）

## 范围

本记录固定当前 K1 BSP 工作树的十二个专用配置的干净主机构建结果。每个配置均使用
独立的 `cmake_out/k1-regression-*` 输出目录、`--clean` 和 8 个并行任务构建，并由
`tools/check_k1_elf.sh` 验证 ELF 类型、入口地址、装载段权限、启动/NSH 符号、S-mode
timer 和 polling UART 约束。

这是源码、Kconfig、CMake 和最终链接的回归证据，**不是实板验收**。尤其不能据此声称
I2C EEPROM、SPI 回环、PWM 波形、watchdog MMIO 时序或 GPIO/PLIC 中断已在 MUSE Pi Pro
上验证。

## 结果

| 配置 | 专用能力 | 输出 ELF | SHA256 | 结果 |
| --- | --- | --- | --- | --- |
| `nsh` | 最小轮询 NSH | `cmake_out/k1-regression-nsh/nuttx` | `ed79c403907ae22f382f02ae88ff3e1752b18b7548cb6313d423031288625283` | PASS |
| `gpio_irq` | PLIC/GPIO IRQ 编译门槛 | `cmake_out/k1-regression-gpio_irq/nuttx` | `c2db7d8369d516df91a6bc316875940755140d6a9635acc2f8c8724a41ea737a` | PASS |
| `ethernet_polling` | EMAC0 polling 网络栈 | `cmake_out/k1-regression-ethernet_polling/nuttx` | `21a085401bebb4f8ceb1bb3ad0826fed354d4605f43a12c5845bcf6f1ba7f170` | PASS |
| `i2c` | I2C2 polling lower-half | `cmake_out/k1-regression-i2c/nuttx` | `9a882bfc0fc3b92cb3e2d0f041683573564a779be8bf755da7bbc3d1fd32d91c` | PASS |
| `spi3_loopback` | SPI3 polling lower-half | `cmake_out/k1-regression-spi3_loopback/nuttx` | `9d5d889039cc732879239bebe5ed457accb196f8d33453804fa8fa3816e2aef6` | PASS |
| `pwm11` | PWM11 lower-half | `cmake_out/k1-regression-pwm11/nuttx` | `928f3b0a24efe291f0b9d7d3e7479c30b81aa9d0b9682d830d757406edd0af03` | PASS |
| `watchdog` | K1 片上 watchdog lower-half 与有界 smoke | `cmake_out/k1-regression-watchdog/nuttx` | `6595abdd4039ff046049ae98e7fef52ad697a291b298ede77646114aafbd3ab3` | PASS |
| `hardware_bringup` | GPIO/PLIC、EMAC0、I2C2、SPI3、watchdog 合并验证 | `cmake_out/k1-regression-hardware_bringup/nuttx` | `cc58a0e1878dbb3ef8d8a18a2fce7eedeaa9f1a0300d979032be4374edc8d9d6` | PASS |
| `display_fb` | U-Boot framebuffer handoff、`/dev/fb0` 和 `fb` 测试 | `cmake_out/muse_pi_pro_display_fb/nuttx` | `f340fda36ab0120a37fd68ef6575467d3082382ae5dda7d9da6dd3321bbb7c20` | PASS |
| `usb_probe` | USB30 安全上电后的 DWC3/PHY/xHCI 寄存器探测 | `cmake_out/muse_pi_pro_usb_probe/nuttx` | 待本次修复重建 | NOT REBUILT |
| `usb_host_glue` | USB30 clock/reset、USB2 PHY 和 DWC3 Host 角色初始化 | `cmake_out/muse_pi_pro_usb_host_glue/nuttx` | `cd33907e34f434a7163efdcae96b3141e2c8cc7dc85679483c610e9cfdb8c13f` | PASS |
| `usb_host` | DWC3 Host、固定地址 xHCI、PLIC source 125、USB2817 Hub 和 MSC waiter | `cmake_out/muse_pi_pro_usb_host/nuttx` | `1dcfa0b61f1c281e755c617d1bf5fdfe577145e6ba5df9fdd1bdc8cbc66df456` | PASS |

`gpio_irq`、`hardware_bringup` 和 `usb_host` 构建额外使用 `--experimental-irq`，因为
它们明确打开 PLIC IRQ。GPIO IRQ 只由前两个配置启用；xHCI 使用独立的 PLIC source 125。

## 复现

在比赛仓根目录执行以下命令；将 `<profile>` 替换为
`nsh`、`gpio_irq`、`ethernet_polling`、`i2c`、`spi3_loopback`、`pwm11`、
`watchdog`、`hardware_bringup`、`display_fb`、`usb_probe` 或
`usb_host_glue`、`usb_host`：

```bash
tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/<profile> \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/k1-regression-<profile> \
  --jobs 8
```

GPIO IRQ 配置：

```bash
tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/gpio_irq \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/k1-regression-gpio_irq \
  --experimental-irq \
  --jobs 8
```

`hardware_bringup` 也启用了 GPIO/PLIC，因此构建时需要同样的
`--experimental-irq` 门槛：

```bash
tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/hardware_bringup \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/k1-regression-hardware_bringup \
  --experimental-irq \
  --jobs 8
```

显示和 USB profile：

```bash
tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/display_fb \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_display_fb \
  --jobs 8

tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/usb_probe \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_usb_probe \
  --jobs 8

tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/usb_host_glue \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_usb_host_glue \
  --jobs 8

tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/usb_host \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_usb_host \
  --experimental-irq \
  --jobs 8
```

watchdog 专用构建和实板安全步骤见 `docs/K1_WATCHDOG_BRINGUP.md`。其 smoke 只验证
启动、三次喂狗和停止；不允许故意等待 watchdog 到期复位。

## 实板状态

- I2C2：已完成板载 EEPROM `0x50` 连续只读验证，证据见
  `docs/K1_I2C_REAL_BOARD_20260814.md`；
- SPI3：已完成 Pin 19 -> Pin 21 的 800 kHz 模式 0 回环，证据见
  `docs/K1_SPI3_REAL_BOARD_20260814.md`；
- PWM11：暂缓，仅在获得示波器或逻辑分析仪后测量 Pin 3 的 1 kHz、50% 波形；
- watchdog：安全实板 smoke 已通过，证据见
  `docs/K1_WATCHDOG_REAL_BOARD_20260814.md`；不测试到期复位；
- GPIO IRQ：已完成实际 DTS 复核和 Pin 22 -> Pin 33 可控外部 IRQ 的
  PLIC claim/complete 验证，证据见 `docs/K1_PLIC_REAL_BOARD_20260814.md` 和
  `docs/K1_GPIO_BRINGUP.md`；
- USB Host：已完成 USB2817 外置 Hub 的 xHCI 多 slot 实现及主机构建。当前 flat
  包 SHA256 为 `6e6bb9f514c9ddb6eed40d8753efe030704bf60e01d9f1dbe836fcce964ef28a`；
  raw PLIC source 125 已在板级转换为 NuttX IRQ。尚未进行实板 Hub 描述符、下游设备
  或 MSC 验收。

各项接线、安全边界和串口验收标准分别见对应的 `K1_*_BRINGUP.md` 文档与
`docs/K1_DELIVERY_CHECKLIST.md`。
