# 2026-08-16 比赛里程碑证据

本记录对应当前 `feat/k1-velaros-milestone-20260816` 提交候选，基线为
`openvela/dev-ai-contest-2026` 的 `e8ebc57`。所有命令均从比赛仓根目录执行。

## K1 BSP

```text
tools/ci_k1.sh --jobs 8
PASS

hardware_bringup ELF SHA256
6803721e2a2e16bee0444914813f891e097be780a250b2d3397fc22be1fe5a1a
```

本里程碑包含 MUSE Pi Pro 的 GPIO/PLIC、EMAC、I2C、SPI、framebuffer、SDIO/eMMC、
USB host/device/probe、watchdog 和无线首阶段 bring-up 源码、profile、上板工具与操作记录。
实板已有的证据范围以 `docs/K1_*_REAL_BOARD_*.md`、`docs/K1_ETHERNET_DESIGN.md` 和
`docs/K1_USB_DEVICE_REAL_BOARD_20260815.md` 为准。

## VelaROS

```text
OPENVELA_ROOT=/home/sw/Dev/k1-workspace \
  tools/restore_velaros_dds_sources.sh --check
PASS

OPENVELA_ROOT=/home/sw/Dev/k1-workspace \
  tools/restore_velaros_ros2_sources.sh --check
PASS

tools/build_velaros_dds_sim.sh --clean --jobs 8
PASS
ELF SHA256 a0067b8fbc66aa48e2eb11032d3c2b07daeb29a9f71c919da1b50782318dcb73

python3 tools/check_velaros_dds_sim.py --timeout 150
PASS: DDS 3/3, rcl/rmw lifecycle, rclcpp Action, uORB/KVDB/Binder,
fixed buffer backend, LIO/navigation and AMP prototype

tools/build_velaros_release_sim.sh --no-codegen --jobs 8
PASS
ELF SHA256 dce9545a0b57dc7c5172304d19e51f8dc97897ee4883b572b1bea7961fc1571e

python3 tools/check_velaros_ros2_host.py --robot-only --output \
  /home/sw/Dev/k1-workspace/cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-velaros
PASS: ROS 2 host /cmd_vel, MoveRelative success/cancel and guest cleanup

python3 tools/check_velaros_ros2_host.py
PASS: two-way Topic 3/3, SetBool Service, two-way Fibonacci Action success/cancel
```

发布 profile 门禁确认只保留 SIMPLE discovery、UDPv4 和产品静态闭包，排除 TypeLookup、
DDS-RPC、Discovery Server/Client/database、static EDP、TCP、UDPv6 和 Fast DDS 原生
SHM/DataSharing。

## 边界

K1 实板外设结论不能由上述 goldfish 模拟器验收替代。PWM 波形、USB Device/Fastboot
持久烧录闭环、Wi-Fi/蓝牙用户面连通性，以及 K1 上的 VelaROS/Fast DDS 跨设备和长稳，
仍需按各自上板文档补充实测日志后才能标为完成。AI Coding 日志由比赛工作区的采集器在
会话结束后导出，本里程碑不伪造或替代该日志。
