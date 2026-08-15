# K1 主机侧验收工具

## 一键回归

仅检查比赛仓源码，不需要 openvela 工作区或工具链：

```bash
tools/ci_k1.sh --static-only
```

执行静态检查、干净构建、ELF 验收和上板包更新：

```bash
tools/ci_k1.sh --jobs 8
```

完整模式需要比赛仓位于已同步的 openvela 工作区中，并使用工作区内置
`riscv-none-elf` 工具链。托管 GitHub Actions 只运行 static-only；完整交叉构建
由本地 CI 完成。

## 串口抓取

`capture_k1_serial.sh` 是只读抓取器：它会保存板端输出，但不会把你在终端输入的
`help` 发给板子。需要交互时使用下面的双向控制台：

```bash
tools/console_k1_serial.sh --device /dev/ttyUSB0
```

控制台会根据提示符自动转换回车：U-Boot 使用 `CR`，NuttX 的 NSH 使用 `LF`。
因此在看到 `nsh>` 后直接按 Enter 即可执行命令。按 `Ctrl-D` 退出主机控制台，
`Ctrl-C` 会发送到板子。

确认 USB-UART 设备后，在上电前启动：

```bash
tools/capture_k1_serial.sh \
  --device /dev/ttyUSB0 \
  --duration 600
```

默认参数是 115200 8N1，日志写到 `out/k1-serial/`。按 Ctrl-C 可提前停止。工具
保留原始串口字节，不在日志正文插入时间戳，同时生成同名 `.json`，记录设备、
波特率、开始/结束时间、字节数和 SHA256。

指定日志位置：

```bash
tools/capture_k1_serial.sh \
  --device /dev/ttyUSB0 \
  --output /tmp/k1-first-boot.log
```

如果遇到权限错误，应将当前用户加入系统约定的串口设备组或临时使用已有的设备
访问规则；不要修改设备节点权限后把该操作写入自动化脚本。

## Fastboot 持久化闭环

`k1_fastboot_flash.py` 默认只做 manifest 校验和设备探测。真正写入必须同时给出
`--execute` 和 `--confirm K1-FASTBOOT-WRITE`，并且提供 USB-TTL 串口设备；工具会
在发出 `reboot` 前启动串口抓取，写入完成后检查 manifest 中的启动标记并保存 JSON
证据。没有确认过镜像格式、`/dev/<target>` 设备节点和可恢复镜像时，不要执行写入。

```bash
python3 tools/k1_fastboot_flash.py /absolute/path/write-manifest.json --probe
python3 tools/k1_fastboot_flash.py /absolute/path/write-manifest.json \
  --execute --confirm K1-FASTBOOT-WRITE --serial-device /dev/ttyUSB0
```

这条工具链只实现 NuttX Fastboot 的安全主机闭环，不实现也不模拟 K1 BootROM 的
Titan/FDL 协议。官方 FDL 仍需真实 VID/PID、协议版本、镜像容器和恢复证据后另行
接入。

## 异常解析

使用产生该日志的同一份 ELF：

```bash
tools/decode_k1_trap.sh \
  --log out/k1-serial/k1-first-boot.log \
  --elf out/k1-bringup/nuttx \
  --output out/k1-serial/k1-first-boot-traps.md
```

解析器提取每个 `K1 EXCEPTION` 块，解释同步 `scause`，并用工作区
`riscv-none-elf-addr2line` 对 `sepc` 做函数和源码定位。ELF SHA256 必须与
`nuttx.sha256` 一致，否则符号结果不可作为验收证据。

上板包也包含两个可独立运行的 Python 工具：

```bash
python3 capture_k1_serial.py \
  --device /dev/ttyUSB0 --output k1.log

python3 decode_k1_trap.py \
  --log k1.log \
  --elf nuttx \
  --addr2line /path/to/riscv-none-elf-addr2line
```

两者只依赖 Python 标准库。
