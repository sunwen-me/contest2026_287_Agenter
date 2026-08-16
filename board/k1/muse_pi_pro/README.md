# MUSE Pi Pro (SpacemiT K1) board support

本目录是 K1 新硬件适配的板级支持包，映射到：

```text
vendor/spacemit/boards/k1/muse_pi_pro
```

当前包含：

- 板级 Kconfig 和 CMake 入口；
- 已确认的 UART、PLIC、CLINT 和 RAM staging 参数；
- S-mode 最小 NSH defconfig；
- 不修改 U-Boot 硬件状态的安全板级初始化；
- 基于 `CONFIG_RAM_START` 的 RV64 链接脚本。

配套 `vendor/spacemit/chips/k1` 当前已包含：

- 单 hart RV64 S-mode 入口；
- SBI TIME timer；
- 保留 U-Boot 配置、只轮询 RBR/LSR/THR 的 `/dev/console`。
- scheduler/syslog 就绪前可用的启动标记和同步异常寄存器日志。
- 默认关闭、可选启用的 hart 0 S-mode PLIC context 1；
- 默认关闭、依赖 PLIC 的 K1 GPIO source 58 边沿中断路径。
- 默认关闭、完全轮询的 K1 I2C2 lower-half；`configs/i2c` 使用板载 EEPROM
  `0x50` 做只读验证，详情见 `docs/K1_I2C_BRINGUP.md`。
- 默认关闭、完全轮询的 K1 SPI3 lower-half；`configs/spi3_loopback` 仅用于
  40Pin Pin 19 (MOSI) 到 Pin 21 (MISO) 的临时回环验证，不接触 QSPI 启动 Flash，
  详情见 `docs/K1_SPI3_BRINGUP.md`。
- 默认关闭的 K1 PWM11 lower-half；`configs/pwm11` 将 40Pin Pin 3 配置为
  `/dev/pwm11`，用于低频占空比测量，详情见 `docs/K1_PWM11_BRINGUP.md`。
- 默认关闭的 K1 on-chip watchdog lower-half；`configs/watchdog` 仅运行有界的
  启动、喂狗、停止 smoke，不测试到期复位，详情见
  `docs/K1_WATCHDOG_BRINGUP.md`。
- 默认关闭的 K1 SDH2 eMMC lower-half；`configs/emmc` 注册 `/dev/mmcsd0`，
  已在实板完成初始化和单块/CMD18 多块只读读取验证，驱动当前不执行写入，详情见
  `docs/K1_EMMC_BRINGUP.md`。
- 默认关闭的 RTL8852BS2 无线首轮迁移；`configs/wireless` 完成 Wi-Fi SDIO 卡
  枚举及 Bluetooth UART2 H5 `SYNC -> CONFIG` 诊断，不注册网络或 Bluetooth
  设备，详情见 `docs/K1_WIRELESS_BRINGUP.md`。
- `configs/hardware_bringup` 将 GPIO/PLIC、EMAC0、I2C2、SPI3 和 watchdog
  合并到同一张验证镜像；PWM11 仍保留在独立配置中，不属于该镜像。
- `configs/display_fb` 注册继承自 U-Boot 的 `/dev/fb0` 并包含 `fb` 色块测试；它
  不重配 DPU 或面板，已在实板通过 framebuffer 注册、映射和色块更新测试；实际
  面板显示仍取决于 U-Boot 是否探测到 HDMI/MIPI 面板，详情见
  `docs/K1_USB_DISPLAY_BRINGUP.md`。
- `configs/usb_probe` 先安全使能 USB30 时钟/reset 和 USB2 PHY，再采集 K1 DWC3、USB PHY
  和 xHCI 参数；已在实板完成只读采集，不注册 USB Host 或 Device，详情见
  `docs/K1_USB_DISPLAY_BRINGUP.md`。
- `configs/usb_host` 已在实板启动 DWC3/xHCI Host、PLIC source 125 和两个根端口，
  并用 Netac XS510 完成 UAS alt 1、stream 1、初始 SCSI 和 `/dev/sda` 注册验证；
  详情见 `docs/K1_USB_DISPLAY_BRINGUP.md`。
- `configs/usb_host_glue` 完成 USB30 时钟/复位、USB2 PHY 和 DWC3 Host 角色初始化，
  但不启动 xHCI、不枚举设备；GPIO79/127/123 的 VBUS/Hub 状态继续继承 U-Boot，
  详情见 `docs/K1_USB_DISPLAY_BRINGUP.md`。

官方板卡接口、Type-C 烧录模式、40Pin UART 线序和首板接线见：
[`docs/K1_MUSE_PI_PRO_OFFICIAL_HARDWARE.md`](../../../docs/K1_MUSE_PI_PRO_OFFICIAL_HARDWARE.md)。

比赛仓根目录还提供：

- `tools/build_k1.sh`：临时映射 vendor 目录并执行干净构建；
- `tools/check_k1_elf.sh`：验证 ELF、Kconfig 和 UART 安全约束；
- `tools/package_k1_bringup.sh`：生成非破坏性的 U-Boot 手工首启包；
- `docs/K1_UBOOT_BRINGUP.md`：首启、诊断和恢复手册。

当前尚未包含：

- SMP 的 SBI HSM/IPI 接线；
- 可整盘写入的 SD 镜像生成器；
- PWM11 Pin 3 波形实测。

基础 NSH、GPIO/PLIC、Ethernet、I2C2、SPI3、watchdog 和 eMMC 只读路径已经完成
实板验证；PWM11 波形、SMP/AMP 和完整可写存储流程仍不在当前默认承诺内。不要为
扩大功能而临时套用 QEMU 的 PLIC 或通用 16550 初始化路径，它们会掩盖真实的中断
和 UART 约束。
