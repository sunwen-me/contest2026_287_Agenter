# goldfish Fast DDS 验证配置

此配置从 openvela `goldfish-arm64-v8a-ap` 派生，只用于验证 VelaROS 的原生
Fast DDS 基线，不是 K1 板级配置。

新增能力：

- Fast DDS 3.6.1、Fast-CDR 2.3.6、foonathan_memory 0.7.4；
- libc++ exceptions、RTTI、wide character 和 C++20；
- pinned Asio headers、TinyXML2、匿名 mmap；
- `DDSHelloWorldExample`，应用任务栈 2 MiB；
- Fast DDS 内部线程显式 64 KiB 栈；
- loopback 单播 initial peers 和可正常回收的 NuttX UDP 接收线程。
- ROS 2 Lyrical 核心接口、`rmw_dds_common`、3 个预生成图消息及其 Fast RTPS
  C++ 类型支持；
- `velaros_core_smoke` 与 `velaros_rmw_dds_smoke`，分别验证公共 RMW 接口和
  GraphCache/生成类型支持。
- `rcl 10.4.4` publisher/subscription、`std_msgs/String` 5.9.2，以及
  `velaros_talker` / `velaros_listener`。
- `rcl` steady clock、timer 和 wait set 的触发与完整回收验收。
- 有界、无内部线程的 VelaROS 单线程 executor；复用一个 `rcl_wait_set` 调度
  subscription、timer、client 和 service，不复制 NuttX 调度器或 libuv event loop。
- 标准 `std_srvs/SetBool` 请求/响应；目标端删去 service event introspection 和
  type-description graph，`/velaros/runtime/set_bridge` 直接复用 openVela KVDB。
- 标准五通道 `example_interfaces/Fibonacci` Action；默认 2 个 Goal、32 项序列，
  Action client/server 与 Topic/Service 共用有界 executor 和 wait set。
- 静态 `rclcpp_action` Client/Server GoalHandle 子集；成功、取消、Feedback、Result
  由同一 caller-owned executor 驱动，不创建每 Goal 线程。
- `rcutils` 日志到 NuttX syslog 的固定缓冲薄适配，不引入第二套日志后端。
- 白名单 uORB ↔ ROS 2 bridge：复用现有 `sensor_temp`，控制设定值复用 uORB
  持久队列；转换和 pump 均由调用方/executor 驱动，不创建 bridge 线程。
- openvela KVDB 本地配置：持久保存 DDS Domain/participant、bridge 开关和
  heartbeat 周期；ROS 2 talker/listener 实际使用前两项，不引入 YAML parser。
- Binder 控制面：`velarosd` 注册 `openvela.velaros.runtime`，`velarosctl`
  跨 task 查询/修改配置并请求停止；服务端单 task `poll()` 分派，不启用通用
  Binder examples 或 Binder thread pool。

Fast DDS 以 header-only 方式使用锁定的 Asio 源码。这里特意不启用
`CONFIG_LIB_ASIO`，因为它会额外编译 openvela 的 separate-compilation/SSL
Asio 目标；Fast DDS 不链接也不需要该目标。

专用配置关闭 `AUDIOUTILS_SPEEXDSP`、`AUDIOUTILS_ALSA_LIB`、`LIB_FFMPEG`、
`MEDIA` 和 `QUICKAPP`。
当前比赛 manifest 没有同步 SpeexDSP 的实际源码，而默认 FFmpeg 配置又启用了
ALSA filters；media server 的插件也直接依赖 FFmpeg 的 `libavutil`。这些
多媒体组件与 DDS 验证无关，关闭它们可以避免把缺失的可选音频依赖误判为 DDS
移植问题。比赛 manifest 中 QuickApp 的 `libgui_wrapper.a` 与
`libquickapp.a` 也是未下载的 Git LFS 指针；DDS HelloWorld 不需要 QuickApp，
所以专用配置不链接这两个约 350 MiB 的专有预编译库。

构建：

```bash
tools/build_velaros_dds_sim.sh --clean --jobs 8
```

推荐直接运行一键验收：

```bash
tools/check_velaros_dds_sim.py
```

手工验证 VelaROS executor 时运行：

```text
velaros_executor_smoke
```

通过标志是 timer callback 和 subscription callback 均为 3 次，并打印
`VelaROS minimal single-thread executor smoke: PASS`。

手工验证 openVela 融合层时运行：

```text
velaros_openvela_integration_smoke
```

通过标志是 syslog 2 条、uORB → ROS 3 条、ROS → uORB 3 条，并打印
`VelaROS openVela integration smoke: PASS`。

手工验证 openvela 原生配置和控制面：

```text
velarosd &
velarosctl status
velarosctl set domain 0
velarosctl set participant 0
velarosctl set bridge 1
velarosctl set heartbeat 1000
velarosctl smoke
```

`smoke` 会通过 Binder 发现服务、修改并从独立 task 读取 KVDB、恢复原配置，
最后请求服务退出。通过标志包括 `VelaROS Binder service discovery: PASS`、
`VelaROS KVDB cross-task config: PASS` 和 `VelaROS service lifecycle: PASS`。
goldfish 的 UnQLite 持久文件为 `/data/persist.db`；K1 必须在板级 defconfig 中
改成实际掉电保持分区，不得照搬模拟器路径。

只校验或强制重建生成消息源码：

```bash
tools/generate_velaros_ros2_interfaces.sh --check
tools/generate_velaros_ros2_interfaces.sh --force
tools/generate_velaros_std_msgs.sh --check
tools/generate_velaros_std_msgs.sh --force
tools/generate_velaros_std_srvs.sh --check
tools/generate_velaros_std_srvs.sh --force
tools/generate_velaros_action.sh --check
tools/generate_velaros_action.sh --force
```

需要先重新构建时：

```bash
tools/check_velaros_dds_sim.py --build
```

手工验收时，在同一个 NSH 中将 subscriber 和 publisher 都作为后台任务启动：

```text
DDSHelloWorldExample subscriber --samples 3 &
DDSHelloWorldExample publisher --samples 3 &
```

验收要求是 participant/endpoint 成功匹配，publisher 发送 3 条样本，
subscriber 收到相同 index/message，打印 `Publisher unmatched.`，等待 3 秒后
`ps` 中不再出现 `DDSHelloWorldExample`、Publisher 或 Subscriber。

与宿主 `/opt/ros/lyrical` 的双向验收入口为：

```bash
tools/check_velaros_ros2_host.py
```

它通过模拟器控制台为 guest participant 0/1 建立四条 UDP 重定向，依次验证
双向 Topic、SetBool Service，以及双方分别作为 Fibonacci Action client/server
时的成功和取消路径。执行前必须先用包含 QEMU external locator 修改的源码重建固件。

2026-08-02 已完成 ROS 2 核心、`rmw_dds_common` GraphCache/生成 Fast RTPS
类型支持和 DDS 数据面的合并自动验收，并确认最终 ELF/构建元数据没有宿主
ROS 路径或共享库污染。包含 `rcl` publisher/subscription、两个
`std_msgs/String` 命令、timer/wait set、executor、syslog/uORB 融合层、
KVDB/Binder 控制面和 QEMU external locator/MTU/type propagation 修复的最终
ELF SHA256 为
`59c39443cd489be43f1dfab2ed3f87b910e846e28d60e8b3b17f8054cfc05678`。
`std_msgs/String` 与 `std_msgs/Float64` 的 20 个生成文件
输入指纹为 `1049233872f2919089483e7ff4cdc7909764529ae1cacb2a2a7de75863fccf56`。
Linux ROS 2 Lyrical 与 openvela simulator 已完成无 Agent 双向验收，guest 到
host 和 host 到 guest 均 3 发 3 收，主机到目标端 SetBool 2/2 请求响应；两向
Fibonacci Action 的成功/取消路径也均 PASS 并正常清理任务；executor smoke 的 timer
和 subscription callback 也均为 3 次并正常退出；openVela 融合 smoke 的两条
桥接路径均为 3 次并正常回收；Binder 服务发现、KVDB 跨 task 配置、单 task
`poll()` 分派和服务停止均 PASS。
