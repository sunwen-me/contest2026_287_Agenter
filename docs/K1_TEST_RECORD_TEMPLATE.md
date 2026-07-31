# MUSE Pi Pro K1 首启验收记录

## 1. 基本信息

| 项目 | 记录 |
|---|---|
| 日期 | |
| 操作者 | |
| 板卡版本 | |
| 内存容量 | |
| 供电方式 | |
| USB-UART 型号 | |
| U-Boot `version` | |
| OpenSBI 版本 | |
| SD 卡/启动介质 | |
| ELF SHA256 | |

## 2. U-Boot 环境

粘贴以下命令的完整输出：

```text
version
bdinfo
printenv bootcmd
mmc list
part list mmc 0
help bootelf
help wdt
```

```text
<粘贴日志>
```

实际加载命令：

```text
<填写>
```

`${filesize}`：

```text
<填写>
```

## 3. 启动阶段

| 检查点 | 结果 | 证据/备注 |
|---|---|---|
| `bootelf -p` 接受 ELF | PASS/FAIL | |
| `K1: entry` | PASS/FAIL | |
| hart ID 为 0 | PASS/FAIL | |
| DTB 指针有效 | PASS/FAIL | |
| 初始 sstatus/satp/stvec 已记录 | PASS/FAIL | |
| `K1: bss-clear` | PASS/FAIL | |
| `K1: s-mode bare` | PASS/FAIL | |
| `K1: nx_start` | PASS/FAIL | |
| NuttX banner | PASS/FAIL | |
| NSH prompt | PASS/FAIL | |

完整串口日志文件名：

```text
<填写>
```

串口日志 SHA256 / JSON 元数据文件：

```text
<填写>
```

## 4. 异常寄存器

没有异常时填写“无”。发生异常时原样记录：

```text
scause  =
sepc    =
stval   =
sstatus =
satp    =
sp      =
```

`addr2line` 结果：

```text
<填写>
```

## 5. NSH 与 Timer

粘贴 `help`、`uname -a`、`ps`、`free`、`uptime` 输出：

```text
<粘贴日志>
```

| 检查项 | 结果 | 证据/备注 |
|---|---|---|
| UART 输出连续 | PASS/FAIL | |
| UART 输入可交互 | PASS/FAIL | |
| `uptime` 持续推进 | PASS/FAIL | |
| 连续运行 10 分钟 | PASS/FAIL | |
| 无重复 trap | PASS/FAIL | |
| 无 watchdog 重启 | PASS/FAIL | |

## 6. 结论

- 本轮结论：
- 首个失败检查点：
- 下一步最小修改：
- 是否允许进入 PLIC 阶段：是/否
