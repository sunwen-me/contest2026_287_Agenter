# K1 MUSE Pi Pro 实板稳定性验收

日期：2026-08-12（Asia/Shanghai）

## 结论

PASS。SSTC 版本 NuttX 在真实 MUSE Pi Pro 上保持双向 NSH 交互，连续观察窗口
覆盖 10 分钟，没有看到 U-Boot 重启、watchdog 重启或异常 trap。

启动阶段两个看门狗均逐个选择并停止成功：

```text
wdt dev PMIC_WDT           =>
wdt stop                   =>
wdt dev watchdog@D4080000  =>
wdt stop                   =>
```

## 交互式证据

同一 `/dev/ttyUSB0` 串口会话中的四次 `uptime` 均返回新的 `nsh>`：

```text
00:08:09 up  0:08, load average: 0.00, 0.00, 0.00
00:08:54 up  0:08, load average: 0.00, 0.00, 0.00
00:09:39 up  0:09, load average: 0.00, 0.00, 0.00
00:10:24 up  0:10, load average: 0.00, 0.00, 0.00
```

同时验证了 `help`、`uname -a`；`free`/`ps` 命令可执行，但当前实例未挂载
`/proc`，因此显示 procfs 提示。

## 边界

本次仍是 U-Boot 从 bootfs 加载到 RAM 后的临时启动，没有执行 `saveenv`、
`mmc write`、FDL 或覆盖默认 `bootcmd`。按 `RST` 或重新上电后会回到原厂 Linux。

下一阶段是核对 K1 GPIO 控制器和 40Pin pinmux，完成可观察的 GPIO 输出/输入 Demo；
在引脚和 pinmux 有资料或实测依据前，不直接猜 GPIO 编号或接线。
