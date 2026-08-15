# K1 片上看门狗 Bring-up

## 范围与状态

本文只覆盖 SpacemiT K1 的片上看门狗，寄存器块地址为 `0xd4080000`。驱动只在
专用验证配置 `board/k1/muse_pi_pro/configs/watchdog` 中启用，常规 NSH 配置保持
关闭。

`k1_wdt_smoke` 仅验证启动、三次显式喂狗、状态读取和停止，**绝不故意测试超时复
位**。下文已记录主机侧构建和 ELF 验证；实板验收仍必须取得完整串口证据。

## 硬件依据

| 项目 | 值 | 交叉核对来源 |
|---|---:|---|
| WDT 寄存器块 | `0xd4080000` | K1 U-Boot DTS 的 `watchdog@D4080000` |
| 启动闩锁 | `0xd4051020`，bit 4 | K1 U-Boot DTS 的第二个 `reg` 项和 watchdog 驱动 |
| 时钟/复位寄存器 | `0xd4050200` | K1 U-Boot CCU 和 reset 驱动 |
| 时钟 gate | WDTPCR bit 0、bit 1 置位 | K1 U-Boot CCU 的 `wdt_clk` gate |
| 解除复位 | WDTPCR bit 2 清零 | K1 U-Boot `RESET_WDT` 表 |
| 受保护写解锁 | 向 `+0xb0` 写 `0xbaba`，再向 `+0xb4` 写 `0xeb10` | K1 U-Boot watchdog 驱动 |
| 控制寄存器 | enable `+0xb8`、timeout `+0xbc`、status `+0xc0`、reset `+0xc8` | K1 U-Boot watchdog 驱动 |
| 超时换算 | `counter = ceil(timeout_ms * 256 / 1000)` | 厂商换算加安全向上量化 |

驱动只使用复位模式。请求的超时会按控制器 3.90625 ms 粒度向上量化，因此硬件不会
早于 NuttX watchdog API 报告的超时值复位。

## 构建

在比赛仓根目录执行：

```bash
tools/build_k1.sh --clean \
  --config vendor/spacemit/boards/k1/muse_pi_pro/configs/watchdog \
  --build-dir /home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_watchdog \
  --jobs 8
```

产物为 `/home/sw/Dev/k1-workspace/cmake_out/muse_pi_pro_watchdog/nuttx`。
构建通过只能证明源码接入和链接正确，不能证明实板 MMIO 时序正确。

### 主机构建记录

2026-08-14，上述命令成功完成，主机侧证据如下：

| 检查项 | 结果 |
|---|---|
| 构建结果 | `#### build completed successfully (7 seconds) ####` |
| ELF 目标 | ELF64 little-endian RISC-V，entry `0x11000000` |
| 看门狗配置 | `K1_WATCHDOG=y`、`WATCHDOG=y`、`K1_WATCHDOG_SMOKE=y`，`WATCHDOG_AUTOMONITOR` 关闭 |
| 已链接符号 | `k1_wdt_initialize`、`k1_wdt_start`、`k1_wdt_stop`、`k1_wdt_keepalive`、`k1_wdt_settimeout`、`k1_wdt_smoke_main` |
| ELF SHA256 | `ad5ce47bc269b265a887bb01b5ed2f5c056580df160ea4760612b6dfe9ec96a7` |

该结果仅为 **host-build verified**。完成下节串口记录前，不能将其表述为看门狗
实板验证通过。

### 实板验证记录

2026-08-14 已在 MUSE Pi Pro 上完成安全 smoke。U-Boot 中的 `PMIC_WDT` 与
`watchdog@D4080000` 都在加载前依次停止；NuttX 的 `k1_wdt_smoke` 成功启动、三次
喂狗并停止硬件 watchdog，随后 `uptime` 返回新的 `nsh>`。完整命令回显、加载地址和
镜像 SHA256 见 `docs/K1_WATCHDOG_REAL_BOARD_20260814.md`。

这项实板结果不包括故意等待到期复位，不能扩展解释为所有 watchdog 行为均已验证。

## 实板步骤

1. 连接已验证的 3.3 V USB-TTL 串口，并抓取完整会话日志。
2. 抢到 U-Boot 后逐条执行下列命令，每条都等待出现 `=>`：

   ```text
   wdt dev PMIC_WDT
   wdt stop
   wdt dev watchdog@D4080000
   wdt stop
   ```

   PMIC 看门狗与 K1 片上看门狗彼此独立，NuttX K1 驱动不能停止 PMIC 看门狗。
3. 按既有 U-Boot wrapper 流程加载 watchdog 专用 RAM payload，等待出现 `nsh>`。
4. 仅执行：

   ```text
   k1_wdt_smoke
   ```

5. 日志中必须有三条 active 状态、一条 inactive 状态，以及最终结果：

   ```text
   k1_wdt_smoke: PASS watchdog started, pinged, and stopped
   ```

6. 保存串口日志和 ELF SHA256，并填入测试记录。

## 安全边界

- 不运行通用 `wdog` 示例，不故意等待到期。
- 不启用 `CONFIG_WATCHDOG_AUTOMONITOR`。K1 驱动明确为手工控制模型，Kconfig
  会拒绝该组合。
- 不在 U-Boot 中用原始 MMIO 猜测地址停看门狗。使用
  `K1_REAL_BOARD_HANDOFF.md` 规定的 `wdt dev` 和 `wdt stop`。
- 若 smoke 在 `WDIOC_START` 后失败，命令会在返回前发送 `WDIOC_STOP`。若串口
  无响应，按 RST 重启板子并回到已知可用的原厂 Linux 启动路径。

## 源码位置

- lower-half：`chip/k1/k1_wdt.c`、`chip/k1/hardware/k1_wdt.h`
- 板级注册：`board/k1/muse_pi_pro/src/k1_boot.c`
- 验证配置：`board/k1/muse_pi_pro/configs/watchdog/defconfig`
- 有界 smoke 命令：`middleware/k1_watchdog_smoke/k1_wdt_smoke.c`
- U-Boot 看门狗准备：`docs/K1_REAL_BOARD_HANDOFF.md`
