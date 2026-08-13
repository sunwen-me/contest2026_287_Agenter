# K1 MUSE Pi Pro 40Pin GPIO

更新时间：2026-08-13（Asia/Shanghai）

## 结论

MUSE Pi Pro 的 40Pin GPIO 信号已接入 NuttX GPIO 上层，注册为
`/dev/gpio0` 到 `/dev/gpio25`。为保持第一轮实板验证兼容，
`/dev/gpio0` 仍然是已验证的 GPIO49 / 40Pin Pin 22：

```text
/dev/gpio0 -> GPIO49 -> 40Pin Pin 22
```

GPIO49 驱动已在真实 MUSE Pi Pro 上验证输出低、高电平，万用表相对 Pin 6
（GND）分别测得约 0 V 和 3.3 V。新增 GPIO 已完成源码、Linux 主线
pinmux 和官方原理图交叉核对，仍需逐个在实板上做电压验证。

## 硬件位置和安全边界

Pin 22 是 40Pin 排针从 Pin 1 端开始数的右侧偶数列第 11 个位置，左侧相邻
针脚为 Pin 21。Pin 6 是 GND。

GPIO 电平为 3.3 V。测量时黑表笔接 Pin 6，红表笔接 Pin 22。不要把 USB-TTL
的 VCC 接到板子上；USB-TTL 只接 UART0 的 GND、RX、TX。

## 40Pin 映射

下面的 minor 是设备节点中的编号，即 `minor=0` 对应 `/dev/gpio0`。
GPIO49 特意放在第一项，以保留已有测试接口；其余项按物理排针顺序排列。

| 设备节点 | 40Pin | K1 GPIO | MFPR 偏移 | GPIO bank/bit | GPIO mux | CPU pad |
|---|---:|---:|---:|---|---:|---|
| `/dev/gpio0` | 22 | 49 | `0x0c8` | 1 / 17 | 0 | external/3.3V |
| `/dev/gpio1` | 3 | 41 | `0x0a8` | 1 / 9 | 0 | 1.8V |
| `/dev/gpio2` | 5 | 40 | `0x0a4` | 1 / 8 | 0 | 1.8V |
| `/dev/gpio3` | 7 | 70 | `0x11c` | 2 / 6 | 1 | 1.8V |
| `/dev/gpio4` | 11 | 71 | `0x120` | 2 / 7 | 1 | 1.8V |
| `/dev/gpio5` | 12 | 74 | `0x12c` | 2 / 10 | 0 | 1.8V |
| `/dev/gpio6` | 13 | 72 | `0x124` | 2 / 8 | 1 | 1.8V |
| `/dev/gpio7` | 15 | 73 | `0x128` | 2 / 9 | 1 | 1.8V |
| `/dev/gpio8` | 16 | 91 | `0x200` | 2 / 27 | 0 | 1.8V |
| `/dev/gpio9` | 18 | 92 | `0x204` | 2 / 28 | 0 | 1.8V |
| `/dev/gpio10` | 19 | 77 | `0x138` | 2 / 13 | 0 | external/3.3V |
| `/dev/gpio11` | 21 | 78 | `0x13c` | 2 / 14 | 0 | external/3.3V |
| `/dev/gpio12` | 23 | 75 | `0x130` | 2 / 11 | 0 | external/3.3V |
| `/dev/gpio13` | 24 | 76 | `0x134` | 2 / 12 | 0 | external/3.3V |
| `/dev/gpio14` | 26 | 50 | `0x0cc` | 1 / 18 | 0 | external/3.3V |
| `/dev/gpio15` | 27 | 39 | `0x0a0` | 1 / 7 | 0 | 1.8V |
| `/dev/gpio16` | 28 | 38 | `0x09c` | 1 / 6 | 0 | 1.8V |
| `/dev/gpio17` | 29 | 51 | `0x0d0` | 1 / 19 | 0 | external/3.3V |
| `/dev/gpio18` | 31 | 52 | `0x0d4` | 1 / 20 | 0 | external/3.3V |
| `/dev/gpio19` | 32 | 34 | `0x08c` | 1 / 2 | 0 | 1.8V |
| `/dev/gpio20` | 33 | 47 | `0x0c0` | 1 / 15 | 0 | external/3.3V |
| `/dev/gpio21` | 35 | 48 | `0x0c4` | 1 / 16 | 0 | external/3.3V |
| `/dev/gpio22` | 36 | 35 | `0x090` | 1 / 3 | 0 | 1.8V |
| `/dev/gpio23` | 37 | 33 | `0x088` | 1 / 1 | 0 | 1.8V |
| `/dev/gpio24` | 38 | 46 | `0x0bc` | 1 / 14 | 0 | 1.8V |
| `/dev/gpio25` | 40 | 37 | `0x098` | 1 / 5 | 0 | 1.8V |

Pin 8 (`UART0_TX`) and Pin 10 (`UART0_RX`) are deliberately not registered as
GPIO devices because they are the debug console. Pin 1/17 are 3.3V, Pin 2/4
are 5V, and Pin 6/9/14/20/25/30/34/39 are GND; they are not GPIO devices.

## 驱动范围

当前驱动是板级 GPIO lower-half，每个物理 GPIO 是一个独立的 GPIO 设备：

- `GPIO_INPUT_PIN`：浮空输入；
- `GPIO_INPUT_PIN_PULLUP`：内部上拉输入；
- `GPIO_INPUT_PIN_PULLDOWN`：内部下拉输入；
- `GPIO_OUTPUT_PIN`：推挽输出；
- `GPIO_OUTPUT_PIN_OPENDRAIN`：开漏输出，高电平释放、低电平主动拉低；
- 在同时启用 `CONFIG_K1_PLIC=y` 和 `CONFIG_K1_GPIO_IRQ=y` 时，支持
  `GPIO_INTERRUPT_PIN`、`GPIO_INTERRUPT_RISING_PIN`、
  `GPIO_INTERRUPT_FALLING_PIN` 和 `GPIO_INTERRUPT_BOTH_PIN`；
- GPIO 中断共享 PLIC source 58，K1 GPIO 的四个 32-bit bank 共用一个板级 ISR，
  ISR 读取并写 1 清除 `GEDR` 后再分发到对应 GPIO callback；
- GPIO 硬件去抖、有效高/低电平中断和 wakeup 中断仍返回 `-ENOTSUP`；
- `GPIOC_SETMASK` 的 `true` 表示屏蔽。硬件 `GAPMASK` 与此相反：位为 0 表示屏蔽，
  位为 1 表示允许中断。

寄存器定义位于：

```text
chip/k1/hardware/k1_gpio.h
```

实现位于：

```text
board/k1/muse_pi_pro/src/k1_gpio.c
```

驱动只操作上述 GPIO 对应的 GPIO bank、MFPR pinmux、外部 IO 电源域，以及
GPIO/AIB 的时钟复位寄存器；不会改写 UART0 的 APBC 寄存器。1.8V CPU pad
经 MUSE Pi Pro 原理图中的电平转换器连接到 40Pin 的 3.3V 侧；external pad
则由驱动显式选择 3.3V 电源域。

K1 的外部 IO 电源域是受保护寄存器。驱动遵循 Linux mainline
`drivers/pinctrl/spacemit/pinctrl-k1.c` 的序列，在每次写入前向 APBC
`ASFar=0x50` / `ASSar=0x54` 写入 `0xbaba` / `0xeb10`，然后将 MFPR
`+0x800+0x10`（GPIO47..52）和 `+0x800+0x0c`（GPIO75..80）的 `V18EN`
清零，明确选择 MUSE Pi Pro 的 3.3V 外部 IO 电源。

## 构建配置

`board/k1/muse_pi_pro/configs/nsh/defconfig` 启用：

```text
CONFIG_DEV_GPIO=y
CONFIG_EXAMPLES_GPIO=y
CONFIG_EXAMPLES_GPIO_STACKSIZE=8192
```

GPIO 外部中断是单独的实验性选项，不进入默认 NSH 配置：

```text
CONFIG_K1_PLIC=y
CONFIG_K1_GPIO_IRQ=y
```

这两个选项只应在已经确认 PLIC DTS、并准备做单根 GPIO 边沿测试的临时构建中启用。

仓库提供了独立的实验配置，不会修改默认 `nsh/defconfig`：

```bash
tools/build_k1.sh \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/gpio_irq \
  --build-dir cmake_out/muse_pi_pro_gpio_irq \
  --package-dir out/k1-gpio-irq \
  --clean --package --experimental-irq
```

`--experimental-irq` 是刻意要求的安全闸门：没有它，ELF 检查器会拒绝带
`CONFIG_K1_PLIC=y` 的镜像；默认 `nsh` 构建仍按首板基线检查 PLIC 必须关闭。

构建入口：

```bash
K1_PACKAGE_DIR=/home/sw/Dev/k1-workspace/out/k1-gpio-migrate-v2 \
/home/sw/Dev/k1-workspace/contest2026_287_Agenter/tools/build_k1.sh \
  --clean --package
```

这是 U-Boot RAM 临时启动包，不会自动刷写 eMMC 或修改 U-Boot 环境。

## NSH 验证

进入板端 `nsh>` 后执行：

```text
ls /dev/gpio0
gpio -t 3 -o 0 /dev/gpio0
gpio -t 3 -o 1 /dev/gpio0
```

例如测试物理 Pin 33（GPIO47，对应 `/dev/gpio20`）：

```text
gpio -t 3 -o 0 /dev/gpio20
gpio -t 3 -o 1 /dev/gpio20
```

测量时黑表笔接同一排针的 GND（例如 Pin 6），红表笔接目标 GPIO。
不要把 USB-TTL 的 VCC 接到板子上。

预期关键输出：

```text
/dev/gpio0
Verify:        Value=0
Verify:        Value=1
```

实板验证结果：

| 操作 | NSH 结果 | Pin 22 对 Pin 6 实测 |
|---|---|---:|
| 输出低 | `Verify: Value=0` | 约 0 V |
| 输出高 | `Verify: Value=1` | 约 3.3 V |

## GPIO 外部中断验证

下面的测试需要一根跳线，把 40Pin Pin 22（GPIO49，`/dev/gpio0`）接到
Pin 33（GPIO47，`/dev/gpio20`）。两根信号都在 3.3V 侧；接线前先确认输出端为低，
不要把两个 GPIO 同时配置为输出。还应连接同一排针上的 GND（例如 Pin 6）。

仅在临时 IRQ 构建中执行：

```text
gpio -t 3 -o 0 /dev/gpio0
gpio -t 8 /dev/gpio20
```

第二条命令会等待 GPIO47 的上升沿。保持它运行，在另一个 NSH 串口会话中执行：

```text
gpio -t 3 -o 1 /dev/gpio0
```

预期等待会结束并打印输入值变为 1。当前 IRQ 代码尚未在实板上完成最终验收。

## 参考资料与已知限制

映射依据：

- [SpacemiT K1 MUSE Pi Pro 用户使用指南](https://www.spacemit.com/community/document/info?lang=zh&nodepath=hardware/eco/k1_muse_pi_pro/pi_pro_user_guide.md)
- [MUSE Pi/MUSE Pi Pro 扩展 IO 定义](https://www.spacemit.com/community/document/info?lang=zh&nodepath=software/SDK/bianbu/user_guide/LXQt/MUSEPi_and_MUSEPiPro_expansion_IO_pinout.md)
- [MUSE Pi Pro GPIO 引脚图](https://cdn-resource.spacemit.com/software/SDK/ros/docs-ros/zh/k1/03_Basic_applications/3.3_Pin_Applications/images/pi-pro-pins-2.jpg)
- Linux mainline `drivers/gpio/gpio-spacemit-k1.c`
- Linux mainline `drivers/pinctrl/spacemit/pinctrl-k1.c`
- Linux mainline `arch/riscv/boot/dts/spacemit/k1.dtsi`
- 项目使用的 `MUSEPi Pro_schematic-V1.1-20250619.pdf`

1. 新增 GPIO 已完成资料核对和 3.3V IO 电源域初始化，但除 GPIO49/Pin 22 外还没有逐个完成实板电压
   验证；首次测试建议一次只接一个目标脚，并避开 Pin 8/10；
2. 当前仍是 NuttX RAM 临时启动，按 RST 或重新上电会回到原厂 Linux；
3. GPIO 外部中断和 PLIC 路由已经实现为默认关闭的实验性路径，尚未完成实板边沿验收；
   硬件去抖、level/wakeup 中断仍未实现；
4. `CONFIG_GPIO_LOWER_HALF` 与当前板级 lower-half 互斥，避免重复注册；
5. GPIO70--73 在 K1 pinctrl 中使用 GPIO mux 1，这是 Linux mainline 对
   PRI_JTAG 复用脚的 GPIO 功能定义，不应擅自改成 mux 0。
