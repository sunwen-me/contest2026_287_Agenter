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
  `velaros_ros2_talker` / `velaros_ros2_listener`。

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

只校验或强制重建生成消息源码：

```bash
tools/generate_velaros_ros2_interfaces.sh --check
tools/generate_velaros_ros2_interfaces.sh --force
tools/generate_velaros_std_msgs.sh
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

它通过模拟器控制台建立 `17410 -> 7410`、`17411 -> 7411` 两条 UDP
重定向，先验证 openvela talker 到宿主 listener，再验证宿主 talker 到
openvela listener。执行前必须先用包含 QEMU external locator 修改的源码
重建固件。

2026-07-31 已完成 ROS 2 核心、`rmw_dds_common` GraphCache/生成 Fast RTPS
类型支持和 DDS 数据面的合并自动验收，并确认最终 ELF/构建元数据没有宿主
ROS 路径或共享库污染。包含 `rcl` publisher/subscription 和两个
`std_msgs/String` 命令的最近一次已通过构建 ELF SHA256 为
`d0531983a42e81058d5ad5f238af35b2d63cf2eb3a203bac63e839ecdb5b0287`。
QEMU external locator 修改发生在该次构建之后，双向互操作状态仍为待重编译
和待运行验收。
