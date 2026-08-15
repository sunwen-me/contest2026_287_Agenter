# VelaROS ROS-only 固件体积基线

版本：2026-08-02

## 当前 Fast DDS 静态 profile 结果

当前 `goldfish-arm64-v8a-ap-velaros` 产品配置已启用
`CONFIG_FASTDDS_VELAROS_STATIC_PROFILE`。在相同发布 defconfig 下，profile 前后
Map 对比为：整机 Flash 25,672,616 B -> 23,143,224 B，减少 2,529,392 B
（9.85%）；Fast DDS 链接 Flash 17,248,198 B -> 14,769,205 B，减少
2,478,993 B（14.37%）；静态 RAM 减少 43,936 B（6.32%）。该组 Map 测量对应
profile 后 ELF `46b1cc34cd756db2a9ea89c7caa9b29621171e1a706cbb0b79499c95a2482aeb`。
加入静态 rclcpp/Action SDK 后的历史发布 ELF 为
`16fc2e7b4e11793a797173a85b406e8a9498f1254077633a7adf7bd4b53e5498`。加入
`Twist`、`MoveRelative` 和 uORB 产品节点、移除 Release Fibonacci 后的历史 ELF
为 `2c70d60f84cccbc54313dc13f0fbdb0337583c9ba7954aa6bcdc0ed9141d2995`。当前再加入
固定共享缓冲 backend 后，ELF 为
`82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`；Map 装载
Flash 为 23,152,776 B（22.08 MiB），静态 RAM 为 728,376 B（711.30 KiB）。
本节不把不同功能版本的总差异冒充 Fast DDS profile 或单个 Action 类型的收益。

## Fast DDS 原生 SHM/DataSharing 删除结果

在当前产品闭包上只改变原生 SHM/DataSharing 的编译边界，前后使用相同 defconfig、
工具链和 `-O3`。删除前 ELF SHA256 为
`7eed1e06de7c506ce1fa21069a4f1b3110e7e5a9c30e409a3c95b2416954f5a4`，删除后、加入
固定共享缓冲 backend 前为 `2c70d60f84cccbc54313dc13f0fbdb0337583c9ba7954aa6bcdc0ed9141d2995`。
精确 section 结果如下：

| 指标 | 删除前 | 删除后 | 变化 |
|---|---:|---:|---:|
| Flash | 23,485,960 B | 23,125,496 B | -360,464 B（-352.02 KiB，-1.535%） |
| 静态 RAM | 663,640 B | 661,752 B | -1,888 B（-1.84 KiB，-0.284%） |
| `.text` | 20,719,912 B | 20,376,888 B | -343,024 B |
| `.rodata` | 2,495,768 B | 2,479,416 B | -16,352 B |
| `.data` | 270,280 B | 269,192 B | -1,088 B |
| `.bss` | 393,360 B | 392,560 B | -800 B |

Fast DDS 编译单元从 187 降到 183，最终归档已链接对象从 170 降到 166。Map
归属分析显示 `libfastdds.a` 约减少 362.29 KiB Flash 和 772 B RAM；全 ELF section
才是上表的烧录/静态 RAM 权威口径。LVGL、curl、mbedTLS、媒体、数据库和 openVela
调试设施没有参与本轮变化。

删除的是 Fast DDS 原生 SHM transport、DataSharing listener/notification/payload
pool 和 watchdog 路径。板内普通消息已有 uORB 替代；大消息的 openVela 固定池、
uORB descriptor 与 ROSIDL 静态 backend 随后已实现。该后续功能的独立体积增量见
下节，且不改变这里原生 SHM 删除前后的历史比较口径。

## 固定共享缓冲 backend 增量

在上述删除原生 SHM 后的产品闭包上，仅增加 4×16 KiB 固定 pool、有界 lease、
uORB descriptor、ROSIDL 静态 backend 和远端 CPU/CDR fallback。前后使用同一
release defconfig、工具链和 `-O3`：

| 指标 | backend 前 | backend 后 | 变化 |
|---|---:|---:|---:|
| Flash | 23,125,496 B | 23,152,808 B | +27,312 B（+26.67 KiB，+0.118%） |
| 静态 RAM | 661,752 B | 728,376 B | +66,624 B（+65.06 KiB，+10.068%） |
| `.text` | 20,376,888 B | 20,401,720 B | +24,832 B |
| `.rodata` | 2,479,416 B | 2,481,080 B | +1,664 B |
| `.data` | 269,192 B | 270,008 B | +816 B |
| `.bss` | 392,560 B | 458,368 B | +65,808 B |

该 backend 前后表对应命名纠正前的可重复测量快照，backend 后 ELF 为
`747199e229e1ad8b6a9a2c0626a473602caa85bc12f2e080f3bd3f654eee91f4`。后续命名
纠正只让 `.rodata` 再减少 32 B，不改变共享池、代码路径或静态 RAM 结论。

64 KiB RAM 是产品配置显式预留的 payload 容量，不是运行时隐藏 heap。剩余约
1.06 KiB RAM 是 slot/lease 状态、mutex、backend 状态与链接增量。`codesize` Map
归属分析识别新增 `libvelaros_buffer_pool.a` 和 `libvelaros_buffer_backend.a`，其
Flash 增量加上 RMW loader/uORB 的少量变化与全 ELF section 结果一致。

backend 前后 ELF/Map 快照为 `cmake_out/velaros-before-buffer-backend.{elf,map}`
和当前 release 输出。功能、所有权与硬件限制见
[`VELAROS_BUFFER_BACKEND.md`](VELAROS_BUFFER_BACKEND.md)。

准确的源码排除项、保留边界和完整验收见
[`VELAROS_FASTDDS_STATIC_PROFILE.md`](VELAROS_FASTDDS_STATIC_PROFILE.md)。下文保留
的是更早的“开发测试入口 -> ROS-only 发布入口”比较，用于说明没有删除 openVela
平台模块；它不是当前 Fast DDS profile 的 before/after 数据。

## 比较口径

使用同一工具链、`-O3` 和完整 openVela goldfish 配置比较两份 AArch64 Map：

- 开发配置 `goldfish-arm64-v8a-ap-fastdds`，ELF SHA256
  `59c39443cd489be43f1dfab2ed3f87b910e846e28d60e8b3b17f8054cfc05678`；
- 静态 profile 引入前的 ROS-only 发布基线，ELF SHA256
  `794d193d0192d734022595c985972c15e2867e2d4dfdbfde9286da4d0bab09a9`。

两份生成后的 `.config` 逐行比较，差异只包含 DDS HelloWorld 和 VelaROS
core/RMW/rcl/executor/Topic/Action/openVela-integration 测试入口。LVGL、媒体、
网络、安全库、数据库、平台调试与测试能力均保持相同。原始 ELF 包含调试信息，
不能代表烧录体积，以下数字均来自链接 Map 的装载 section/符号。

| 指标 | 开发配置 | ROS-only 发布配置 | 变化 |
|---|---:|---:|---:|
| Flash | 24.43 MiB | 23.80 MiB | -637.79 KiB（-2.5%） |
| 静态 RAM | 391.12 KiB | 384.14 KiB | -6.98 KiB（-1.8%） |
| `.text` | 21.91 MiB | 21.35 MiB | -577.96 KiB（-2.6%） |
| `.rodata` | 2.49 MiB | 2.44 MiB | -54.71 KiB（-2.1%） |
| `.data` | 26.94 KiB | 21.82 KiB | -5.12 KiB（-19.0%） |
| `.bss` | 364.18 KiB | 362.32 KiB | -1.86 KiB（-0.5%） |

精确 Flash 差值为 653,094 bytes，静态 RAM 差值为 7,147 bytes。之前
24.43 MiB → 9.93 MiB（-59.4%）的结果混入了 openVela 平台模块裁剪，已经撤回，
不再作为 VelaROS 的优化成绩。

## 明确保留的 openVela 模块

下列模块在两份 Map 中装载大小完全相同：

| 模块 | 两份配置的 Map 装载大小 |
|---|---:|
| LVGL | 1,568,061 bytes |
| curl | 690,170 bytes |
| mbedTLS | 450,865 bytes |
| FreeType | 383,917 bytes |
| NuttX network | 322,347 bytes |
| drivers | 324,258 bytes |
| TurboJPEG | 208,584 bytes |
| PNG | 160,965 bytes |
| UnQLite | 140,784 bytes |
| ostest | 115,867 bytes |
| libuv | 55,676 bytes |

配置门禁还显式检查音视频、QuickJS、gtest/cxxtest、KASAN、allsyms、调试符号、
TCP、DNS、`popen()`、Binder、uORB、KVDB/UnQLite 等能力存在。Fast DDS 的运行时
IDL 外部预处理在 NuttX 上不调用 `popen()`，但 openVela 的 `SYSTEM_POPEN` 没有
因此被删除。

## 实际裁掉的 ROS/DDS 内容

发布 defconfig 只关闭以下测试/诊断入口：

- Fast DDS HelloWorld；
- VelaROS core、RMW DDS、RMW Fast RTPS、rcl 和 executor smoke；
- VelaROS Topic talker/listener、Action client/server 互通命令；
- openVela integration smoke；
- Fast DDS Statistics backend、内部开发诊断与旧日志宏。
- VelaROS 产品 profile 下的 Fast DDS 原生 SHM/DataSharing 实现；通用 Fast DDS
  开发配置仍保留上游源码作对照。

Map 中直接消失的可执行归档是 DDS/ROS 测试和互通程序。当前 Release 不再只是
“可供以后链接”的 SDK：它实际链接 `velaros_robot_node`、`geometry_msgs/Twist`、
`MoveRelative` typesupport、uORB bridge 和 rclcpp Action 子集。开发回归用
Fibonacci 的 functions、Fast RTPS type support、traits 和 smoke 命令不进入产品
编译图，ELF 符号审计也为 0。发布固件仍保留 `velaros_bridge_service`、KVDB/Binder
控制面和完整 openVela 平台基线，不提供 DDS HelloWorld 或 ROS 测试命令。

主要链接闭包变化如下：

| 模块 | 开发配置 | ROS-only 发布配置 | 变化 |
|---|---:|---:|---:|
| DDS HelloWorld | 350.97 KiB | 0 | -350.97 KiB |
| `rmw_fastrtps_cpp` | 573.81 KiB | 450.24 KiB | -123.58 KiB |
| `rcl_action` | 47.36 KiB | 5.44 KiB | -41.92 KiB |
| `rcl` | 99.23 KiB | 69.47 KiB | -29.76 KiB |
| `rmw_dds_common` | 91.93 KiB | 68.06 KiB | -23.86 KiB |
| `rmw_fastrtps_shared_cpp` | 317.54 KiB | 300.55 KiB | -16.99 KiB |

这些库的减小主要来自静态链接器不再为测试命令保留未使用路径，不表示从源码或
SDK 中删除 Topic、Service、Action、QoS 或 Discovery API。

## 验收边界

ROS-only 发布配置已经通过：

- 可重复 Release 构建及独立配置门禁；
- ROS-only 差异与 openVela 平台能力保存门禁；
- Fast DDS Statistics backend 对象排除检查；
- host ROS 2 Lyrical → guest `std_srvs/SetBool` 2/2 Service 通信；
- host ROS 2 Lyrical → guest `geometry_msgs/Twist` → openVela uORB；
- host `MoveRelative` 成功、Feedback、Result 和取消五通道通信；
- Fibonacci 编译对象和 ELF 符号均为 0；
- openVela UnQLite `/data/persist.db` 配置写入；
- 产品节点、Service 和 DDS 后台任务正常回收。

当前数字是 goldfish simulator 静态链接基线。K1 链接脚本、板级驱动、DMA
buffer 和网络资源会改变最终结果；取得板子后必须用 K1 ELF/Map 重跑，并补充
峰值堆、线程栈、CPU 和 30 分钟长稳数据。
