# K1 GPIO49 / 40Pin Pin 22

更新时间：2026-08-13（Asia/Shanghai）

## 结论

MUSE Pi Pro 的 40Pin **Pin 22** 已接入 NuttX GPIO 上层，映射为：

```text
/dev/gpio0 -> GPIO49 -> 40Pin Pin 22
```

驱动已在真实 MUSE Pi Pro 上验证输出低、高电平，万用表相对 Pin 6（GND）
分别测得约 0 V 和 3.3 V。

## 硬件位置和安全边界

Pin 22 是 40Pin 排针从 Pin 1 端开始数的右侧偶数列第 11 个位置，左侧相邻
针脚为 Pin 21。Pin 6 是 GND。

GPIO 电平为 3.3 V。测量时黑表笔接 Pin 6，红表笔接 Pin 22。不要把 USB-TTL
的 VCC 接到板子上；USB-TTL 只接 UART0 的 GND、RX、TX。

## 驱动范围

当前驱动是单实例、板级 GPIO lower-half，注册为 `/dev/gpio0`：

- `GPIO_INPUT_PIN`：浮空输入；
- `GPIO_INPUT_PIN_PULLUP`：内部上拉输入；
- `GPIO_INPUT_PIN_PULLDOWN`：内部下拉输入；
- `GPIO_OUTPUT_PIN`：推挽输出；
- `GPIO_OUTPUT_PIN_OPENDRAIN`：开漏输出，高电平释放、低电平主动拉低；
- GPIO 中断、去抖和中断屏蔽：当前返回 `-ENOTSUP`，因为本轮没有启用 K1
  GPIO/PLIC 中断链路。

寄存器定义位于：

```text
chip/k1/hardware/k1_gpio.h
```

实现位于：

```text
board/k1/muse_pi_pro/src/k1_gpio.c
```

驱动只操作 GPIO49 对应的 GPIO bank、MFPR pinmux，以及 GPIO/AIB 的时钟复位
寄存器；不会改写 UART0 的 APBC 寄存器。

## 构建配置

`board/k1/muse_pi_pro/configs/nsh/defconfig` 启用：

```text
CONFIG_DEV_GPIO=y
CONFIG_EXAMPLES_GPIO=y
CONFIG_EXAMPLES_GPIO_STACKSIZE=8192
```

构建入口：

```bash
K1_PACKAGE_DIR=/home/sw/Dev/k1-workspace/out/k1-bringup-gpio-v2 \
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

## 已知限制

1. 当前只提供 GPIO49 一个物理 pin，后续增加 40Pin 其他 GPIO 时应逐一确认
   DTS/pinmux、GPIO bank、时钟复位和实际板级复用；
2. 当前仍是 NuttX RAM 临时启动，按 RST 或重新上电会回到原厂 Linux；
3. 当前没有实现 GPIO 外部中断、PLIC 路由和硬件去抖；
4. `CONFIG_GPIO_LOWER_HALF` 与当前板级 lower-half 互斥，避免重复注册或出现
   未定义的 `k1_gpio_initialize()`。
