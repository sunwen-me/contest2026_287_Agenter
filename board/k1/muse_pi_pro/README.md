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
- GPIO 中断和 PLIC 的最终实板验收。

本配置已完成编译链接验证，但尚未完成 K1 实板 NSH 验证。不要为扩大功能而临时
套用 QEMU 的 PLIC 或通用 16550 初始化路径，它们会掩盖真实的中断和 UART 约束。
