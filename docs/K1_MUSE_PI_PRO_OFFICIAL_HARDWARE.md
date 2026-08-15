# MUSE Pi Pro（SpacemiT K1）官方硬件与首板 Bring-up 指南

更新时间：2026-08-12

## 1. 依据与适用范围

本文档以 SpacemiT 官方《[K1 MUSE Pi Pro 用户使用指南](https://www.spacemit.com/community/document/info?lang=zh&nodepath=hardware/eco/k1_muse_pi_pro/pi_pro_user_guide.md)》为硬件接口和操作事实来源，官方页面显示的最近更新时间为 2026-07-30。

项目软件启动链、K1 UART MMIO 地址、S-mode 和 U-Boot 手工加载流程仍以本仓库的 K1 文档和实板参考仓为依据。两类资料的边界如下：

| 内容 | 事实来源 |
|---|---|
| PWR Type-C 的供电/USB Device/烧录功能 | SpacemiT 官方用户指南 |
| FDL、PWR、RST 按键和 STAT 指示灯 | SpacemiT 官方用户指南 |
| 40Pin 调试串口脚位和 3.3V 电平 | SpacemiT 官方用户指南 |
| UEFI、SD/eMMC/SSD/USB 启动选择 | SpacemiT 官方用户指南 |
| K1 UART0 `0xd4017000`、115200 8N1、S-mode | 项目 K1 资料盘点和实板参考 |
| openvela/NuttX 临时启动 | 当前板实测使用 wrapper + U-Boot `go`；`bootelf -p` 会触发 U-Boot 异常 |

当前板已经完成 UART、NSH、timer、GPIO/PLIC 和 Ethernet 基线；后续结论仍需保留完整串口日志和对应镜像 SHA256。

## 2. 板端电源、USB 和按键

### 2.1 PWR Type-C 接口

官方指南将 PWR 接口定义为：

- USB Type-C；
- 支持 USB-PD 供电：5V/3A、9V/3A、12V/3A；
- 在烧录模式下，同时承担电源输入和 USB Device，与上位机连接后可被识别并进行烧录升级；
- 烧录必须使用 USB 数据线，纯充电线不能执行烧录；官方建议烧录时保证 USB 供电功率不低于 10W。

因此：

1. 正常上电可以使用 PWR Type-C；
2. 正常启动时，Type-C 不一定在主机上枚举出 USB 设备，这是正常的；
3. 只有按 FDL 进入烧录模式后，PWR Type-C 才应作为 USB Device 被 Titan/fastboot 识别；
4. USB-TTL 的 VCC 线不应接到板上，板子由 PWR Type-C 供电。

### 2.2 指示灯和按键

| 标识 | 官方含义 |
|---|---|
| STAT 熄灭 | 未连接电源或异常 |
| STAT 绿色常亮 | 系统启动完成、正常运行 |
| STAT 绿色常亮约 3 秒后闪烁 1 次 | Boot 异常 |
| STAT 绿色常亮约 3 秒后闪烁 2 次 | 外部 RAM/DDR 异常 |
| STAT 绿色常亮约 3 秒后闪烁 3 次 | Kernel 或烧写镜像异常 |
| STAT 绿色常亮约 3 秒后闪烁 4 次 | Grub/引导内容异常 |

| 按键 | 操作 |
|---|---|
| PWR | 关机状态按住约 1 秒后松开：开机；正常运行状态按住约 3 秒：强制下电关机 |
| RST | 短按：电源复位、系统强制重启 |
| FDL | 按住后插入电源或执行电源复位：进入固件烧录模式 |

首次调试建议用 **PWR Type-C + PWR/RST** 控制启动，不要在尚未确认烧录镜像和工具链之前按住 FDL。

## 3. USB-TTL 调试串口

### 3.1 板端脚位

官方指南把 40Pin 的 6、8、10 脚定义为调试串口：

| 板端脚位 | 信号 | 接 USB-TTL |
|---:|---|---|
| 6 | GND | GND |
| 8 | `UART0_TXD_3V3` | TTL `RXD` |
| 10 | `UART0_RXD_3V3` | TTL `TXD` |

接线图：

```text
MUSE Pi Pro pin 6  GND          -> USB-TTL GND
MUSE Pi Pro pin 8  UART0_TXD    -> USB-TTL RXD
MUSE Pi Pro pin 10 UART0_RXD    -> USB-TTL TXD
USB-TTL VCC/5V/3V3              -> 不接
```

官方指南明确 GPIO 电平域为 3.3V。USB-TTL 必须使用 3.3V 信号电平；不要根据红、黑、白、绿颜色猜 TX/RX，颜色只可作为线索，最终以转接器丝印或万用表确认。

### 3.2 串口参数

- 115200 baud；
- 8 data bits、无校验、1 stop bit（8N1）；
- Linux 下 CH340 通常显示为 `/dev/ttyUSB0`，实际设备名以系统枚举结果为准；
- 使用项目工具抓取原始日志：

```bash
tools/capture_k1_serial.sh \
  --device /dev/ttyUSB0 \
  --duration 600
```

必须在板子上电或按 RST 之前启动抓取，才能保留 BootROM/FSBL/OpenSBI/UEFI 或 U-Boot 的完整输出。

## 4. 两条不同的启动/烧录路径

### 4.1 官方系统安装或恢复：FDL + Type-C

官方用户指南记录的操作顺序：

**设备关机时：**

1. 按住 FDL 不松开；
2. 插入 PWR Type-C 数据线，连接上位机并给板子供电；
3. 松开 FDL；
4. 使用 Titan 或 fastboot 操作。

**设备已经由 Type-C 供电并运行时：**

1. 按住 FDL 不松开；
2. 短按 RST；
3. 松开 FDL；
4. 使用 Titan 或 fastboot 操作。

这条路径会改变板上固件或系统介质，属于烧录/恢复流程。除非已经确认镜像格式、目标分区和恢复方案，否则不要把它作为本项目第一次 openvela 验证的默认动作。

### 4.2 本项目 openvela 首板验证：串口 + SD/U-Boot

本项目当前设计的首板验证是非破坏性的手工路径：

```text
PWR Type-C 供电
    |
    +--> 40Pin UART0 -> USB-TTL -> 主机串口日志
    |
    +--> 原厂/可恢复启动介质
             |
             +--> 进入实际固件命令行
                     |
                     +--> 手工加载 openvela ELF
```

项目包中的 `bootelf -p` 在当前这块板的 U-Boot 上已经实测失败；当前使用 wrapper + `go` 的 RAM-only 路径。官方用户指南记录的是 UEFI 启动：上电约 3 秒内按 F2 可进入设置，Boot Manager 可选择 eMMC、SSD、USB 硬盘或 SD 卡；因此正式持久启动仍需单独设计，不能看到 Type-C 没有 USB 枚举就推断板子没有启动，也不能盲目输入 U-Boot 命令。

项目首轮原则：

- 保留一张能恢复原系统的 SD 卡；
- 不执行 `saveenv`、`mmc write` 或整盘写入；
- 先只读记录 `version`、`bdinfo`、存储设备、DTB 和实际启动提示；
- 只有确认出现 U-Boot 提示符后，才执行项目包内的 `bootelf -p` 流程；
- 只有确认官方 UEFI/启动介质适配方案后，才把项目 ELF 集成进自动启动项。

## 5. 首次上板操作清单

### 5.1 接线和供电

1. 板子下电；
2. USB-TTL 只接 pin 6/8/10，TTL VCC 悬空；
3. PWR Type-C 接可靠的 USB-PD 电源；
4. 上位机保留 `/dev/ttyUSB0` 串口设备；
5. 不按 FDL；
6. 启动串口抓取工具。

### 5.2 启动和观察

1. 若 STAT 熄灭，按 PWR 约 1 秒后松开；
2. 若板子已经运行，短按 RST；
3. 等待串口输出；
4. 记录 STAT 灯状态、启动固件版本、启动菜单或命令提示符；
5. 把完整日志与对应 ELF SHA256 一起保存。

### 5.3 没有串口输出时的排查顺序

1. 先确认 STAT 是否亮；
2. 确认 PWR Type-C 接口和电源线，而不是把 USB-TTL 的红线当电源；
3. 确认板端 pin 6/8/10，而不是只按线色接线；
4. 交换 USB-TTL 的 TXD/RXD，保持 GND 不变；
5. 确认 USB-TTL 为 3.3V 电平、串口为 115200 8N1；
6. 确认串口抓取程序启动后再按 RST；
7. 仍无输出时保存 STAT 状态和线缆/转接器型号，不要直接进入 FDL 或刷写。

## 6. 项目待实板验证项

官方指南解决了接口和操作方式，当前仍需补齐以下项目：

- 实际启动固件是 UEFI 还是项目所需的 U-Boot 命令环境；
- 实际 SD/MMC 编号、分区布局、DTB 路径和 `bootelf` 支持情况；
- 项目 ELF 的 `0x11000000` 装载窗口是否与当前固件的 reserved-memory 和 relocation 冲突；
- payload 窗口、U-Boot relocation 和 reserved-memory 的详细冲突核对；
- PWM11 Pin 3 波形；
- K1 Fast DDS 实板互操作、真实 IMU/LiDAR 和底盘接口。

## 7. 相关项目文档

- [`K1_BOOT_INVENTORY.md`](K1_BOOT_INVENTORY.md)：启动链事实、来源和待验证项；
- [`K1_UBOOT_BRINGUP.md`](K1_UBOOT_BRINGUP.md)：项目 U-Boot 手工首启；
- [`K1_HOST_TOOLING.md`](K1_HOST_TOOLING.md)：串口采集和异常解析；
- [`K1_DELIVERY_CHECKLIST.md`](K1_DELIVERY_CHECKLIST.md)：交付检查清单。
