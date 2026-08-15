# K1 片上看门狗实板验收（2026-08-14）

## 结论

MUSE Pi Pro 上的 K1 片上 watchdog lower-half 已完成首个安全实板验证。验证只覆盖
启动、三次显式喂狗、停止与 NSH 存活确认；**没有故意等待超时复位**。

| 检查项 | 结果 |
| --- | --- |
| U-Boot 停止 `PMIC_WDT` | PASS |
| U-Boot 停止 `watchdog@D4080000` | PASS |
| 读取 wrapper | PASS，54 bytes |
| 读取 watchdog payload | PASS，196056 bytes |
| RAM-only 进入 NuttX/NSH | PASS |
| `k1_wdt_smoke` | PASS，三次 active 状态后 inactive |
| 停止后 `uptime` | PASS，`00:03:04` 且返回 `nsh>` |

## 镜像与加载路径

| 项目 | 值 |
| --- | --- |
| ELF SHA256 | `ad5ce47bc269b265a887bb01b5ed2f5c056580df160ea4760612b6dfe9ec96a7` |
| flat payload SHA256 | `ac474d37f095d47a973a99de7651bb86ce667255aca0e3adb9c7bd99ca13c249` |
| wrapper SHA256 | `4b2d605d201c985c69d82c4c768a1f1b393a4dcc807d948f28e04da9e03e3d81` |
| 板端 payload | `/boot/musepi/contest-k1-watchdog-flat.bin` |
| U-Boot RAM 地址 | wrapper `0x12000000`，payload `0x11000000` |

板端 flat payload 和既有 wrapper 均在加载前用 `sha256sum` 与主机文件比对一致。运行
过程未执行 `saveenv`、`mmc write`、FDL 刷写或任何启动项修改；`ext4load` 仅从现有
bootfs 读取到 RAM。

## 串口证据

控制台会话摘录保存于：

```text
/home/sw/Dev/k1-workspace/out/k1-serial/k1-watchdog-smoke-20260814T042355Z.log
```

关键结果为：

```text
k1_wdt_smoke: PASS watchdog started, pinged, and stopped
00:03:04 up  0:03, load average: 0.00, 0.00, 0.00
nsh>
```

三条 `status flags=00000003` 表示 watchdog 活跃，停止后的
`status flags=00000002 timeout=10000 timeleft=0` 表示设备已 inactive。返回 NSH
及随后可运行的 `uptime` 证明 smoke 没有导致意外复位或卡死。

## 边界

本结果不验证到期复位路径、automonitor、PLIC 中断或跨复位持久性。到期复位会中断
当前 RAM-only 会话，且与本轮安全 smoke 的目标相反；继续保持
`CONFIG_WATCHDOG_AUTOMONITOR` 关闭。
