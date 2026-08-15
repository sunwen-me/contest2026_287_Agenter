# VelaROS ROS 2 / DDS 版本与 ABI 锁定

版本：2026-08-02

适用范围：VelaROS v0.1、openvela goldfish 可行性验证、K1 后续移植

## 结论

VelaROS v0.1 采用 **ROS 2 Lyrical + Fast DDS 3.6.1**，不再使用早期方案中的
Humble/Fast DDS 2.x 暂定组合。选择依据不是“旧版本更容易移植”，而是当前开发机
已安装的最新稳定 ROS 2 发行版 Lyrical，其默认 Fast DDS 包恰好是 3.6.1；目标端
和 Linux 验证端可以共用同一组公开 API、RTPS/CDR 行为和问题修复基线。

不选择 ROS 2 Rolling 作为首版基线。Rolling 持续滚动更新，`rcl`、`rclcpp`、
`rmw_fastrtps` 和生成类型支持可能在项目周期内改变，不适合作为比赛交付的 ABI
冻结点。项目可以定期评估升级，但每次升级必须整体更新版本锁并重新通过 simulator
与 Linux 对端互操作测试。

## 锁定矩阵

| 组件 | 锁定版本 | 精确来源 / revision | 用途 |
|---|---:|---|---|
| ROS 2 | Lyrical | 2026-05-22 release | Linux 构建与互操作基线 |
| `rclcpp` | 32.0.0 | `c80310e420185110bb8876a67223c14a02feee59` | 已接入的静态源码级 RAII 子集语义基线；非完整上游 ABI |
| `rcl` / `rcl_action` | 10.4.4 | `da0b5e70247ef1fb0425bf35417d4e9fdf11efb9` | context/node、Topic/Service、有界 Action、timer/wait set 已目标编译、运行和回收 |
| `rmw` | 7.10.1 | `69abaef996bdbe39488839bf230553af527a1d33` | 已目标编译的公共 RMW 接口 |
| `rmw_dds_common` | 6.0.0 | `2f00e6e9208e925d61330ea92f857b2e67d595ab` | 已目标编译的 GraphCache |
| `rmw_fastrtps_cpp` | 9.4.8 | `198a8b57386144896e5270a2a51bc4306be33967` | 已目标编译/链接；生命周期及 Linux Lyrical 双向 pub/sub PASS |
| `rosidl_typesupport_fastrtps` | 3.9.5 | `fa6cab41fd21f034fc735337b4f3fe9d83829a49` | 已目标编译的图消息静态类型支持 |
| Fast DDS | 3.6.1 | `4e81e8b71bcd6e7c5213c000503cba8e49d6022a` | 目标端 DDS/RTPS 实现 |
| Fast-CDR | 2.3.6 | `d7219391608ae8cd7971e761206dc020098bacb3` | CDR 序列化 |
| foonathan_memory | 0.7.4 | `79d054caaa491d9b6ed7cc65a3a84b495578e6c1` | Fast DDS 内存资源 |
| Asio | 1.34.2 | `ed6aa8a13d51dfc6c00ae453fc9fb7df5d6ea963` | UDP 异步网络 |
| TinyXML2 | 9.0.0 | openvela `external/tinyxml2` | XML profile 解析 |
| `example_interfaces` | 0.14.1 | `e4f46c5762e433a72b2496bb23b0375ecfc06d1e` | 静态 Fibonacci Action IDL 基线 |
| C++ ABI | libc++ / C++20 | libc++abi `3d27ac1876a09b61b5f6516b5e19d361571f6823`，openvela AArch64 GCC 13.4.0 | 目标端静态链接 ABI |

DDS 和 ROS 2 包的完整版本、deb 生成器版本与机器可读 revision 分别位于
[`tools/velaros-dds-sources.lock`](../tools/velaros-dds-sources.lock) 和
[`tools/velaros-ros2-sources.lock`](../tools/velaros-ros2-sources.lock)。

## ABI 边界

“ABI 锁定”不表示把 Ubuntu 的 ROS 2 `.so` 直接复制到 openvela。目标端仍由
openvela 工具链和 libc++ 全量静态编译。这里冻结的是三类边界：

1. `rmw_fastrtps_cpp` 所调用的 Fast DDS/Fast-CDR C++ API 版本；
2. ROSIDL 预生成消息与 Fast RTPS type support 的生成器版本；
3. Linux 对端与 openvela 目标端的 DDS wire protocol、类型名、type hash 和
   CDR 序列化行为。

因此 ROS 2 核心包、消息生成器、RMW、Fast DDS 和 Fast-CDR 必须作为一个版本组
升级，不能只替换其中一个库。

## openvela 适配范围

Fast DDS 目标端首版启用：

- UDPv4、RTPS participant discovery 和 endpoint discovery；
- 静态生成的类型支持；
- Reliable / Best Effort、Keep Last、Volatile 等首版 QoS；
- pthread、monotonic clock、socket、`poll`、可配置 multicast/unicast 和 libc++；
- Fast DDS 3.6.1 官方 HelloWorld publisher/subscriber。

首版关闭：

- Shared Memory transport、Security、SQLite persistence；
- TCP/IPv6、自定义动态插件加载；
- host 风格 `libpthread`、`librt`、`dlopen()`；
- 目标端的 `colcon`、Python、ROSIDL 生成器和 `ros2` CLI。

NuttX 差异集中在
[`tools/patches/fastdds-3.6.1-openvela.patch`](../tools/patches/fastdds-3.6.1-openvela.patch)，
包括 configure-time atomic/pthread 探测、NuttX mmap 虚拟内存支持以及关闭
不适用的 Boost.Interprocess SHM 路径。补丁还阻止 Fast DDS/Fast-CDR 在作为
openvela 子目录构建时把全局 `CMAKE_BUILD_TYPE` 强制改成 Release，避免
`-DNDEBUG` 泄漏到 NuttX、ADB、QuickJS 等无关目标。

ROS 2 语义与 openVela 现有 task/work queue、uORB、syslog、KVDB、Binder 和
RPMsg 的复用边界见
[`VELAROS_OPENVELA_INTEGRATION.md`](VELAROS_OPENVELA_INTEGRATION.md)；该门禁
用于防止后续把桌面 ROS 2 的调度、板内总线、日志和配置基础设施重复移植一遍。

运行阶段还固化了以下 NuttX 差异：

- Fast DDS 内部接收、内建控制、定时事件和发现服务线程显式使用 64 KiB 栈，
  避免上游默认 8 KiB 在 NuttX 上溢出；TypeLookup 若启用也使用 64 KiB，当前
  VelaROS 静态类型 profile 则通过 `registration_only` 不启动远端类型查询；
- simulator HelloWorld 使用 `127.0.0.1` initial peers 和固定 participant ID，
  以 loopback 单播完成确定性 discovery；这不是删除 DDS multicast，而是把
  simulator 验收从宿主网络环境中隔离；
- Linux ROS 2 互操作使用固定 participant ID、QEMU UDP 端口重定向和 `/32`
  external locator；host/guest 同时设置 `fastdds.max_message_size=1200`，防止
  SEDP endpoint discovery 形成超过 guest MTU 的单个 UDP datagram；
- NuttX UDP 接收路径在 `recvfrom()` 前使用 100 ms `poll()`。原因是跨线程
  `shutdown()/close()` 不保证唤醒阻塞接收；有限等待使资源回收线程能观察停止
  标志并被可靠 join；
- NuttX UDP 输入通道按“停止接收、join、关闭 socket、删除资源”的顺序回收，
  避免 ASIO socket close 与接收线程访问 socket 之间的竞态；
- NuttX 不需要 Linux `epoll` 才能运行 DDS。本基线依赖的是 UDP socket 和
  NuttX 已提供的 `poll`；K1 实板 multicast 能否工作仍取决于后续 EMAC、网卡
  multicast filter 和 NuttX 网络配置；
- 缺失 TypeObject 查询改用 `find()` 返回 `RETCODE_NO_DATA`，不再依赖异常路径；
- NuttX 示例不捕获用于 task-group 子任务清理的 SIGTERM，防止
  `CONFIG_GROUP_KILL_CHILDREN_TIMEOUT_MS=-1` 下退出阶段循环等待。

libc++abi 的任务组 TLS 修复单独保存在
[`tools/patches/libcxxabi-nuttx-task-group-tls.patch`](../tools/patches/libcxxabi-nuttx-task-group-tls.patch)。
NuttX 的 DDS 应用会创建多个 task group；补丁为每个 task group 保存独立的
pthread exception key，解决跨组调用 `__cxa_get_globals()` 时
`std::__libcpp_tls_set` 失败。

## 可重复恢复与构建

从比赛仓执行：

```bash
tools/restore_velaros_dds_sources.sh --check
tools/restore_velaros_ros2_sources.sh --check
tools/generate_velaros_ros2_interfaces.sh --check
tools/build_velaros_dds_sim.sh --clean --jobs 8
tools/check_velaros_dds_sim.py
```

若依赖目录缺失或 revision 不匹配：

```bash
tools/restore_velaros_dds_sources.sh --replace
tools/build_velaros_dds_sim.sh --clean --jobs 8
```

恢复脚本会把被替换的旧源码移动到 `/tmp/velaros-dds-source-backup-*`，随后从精确
Git commit 导出源码并应用 openvela 补丁。构建脚本显式清除 ROS、ament 和 colcon
环境变量，因此用户 shell 中是否执行过 `source /opt/ros/lyrical/setup.bash`
不会影响目标端 CMake 查找结果。

也可以用一个命令先构建再验收：

```bash
tools/check_velaros_dds_sim.py --build
```

截至 2026-08-02 的自动验收结果为：

- participant/endpoint discovery：PASS；
- ROS 2 核心 `rcutils` allocator 和 `rmw` node-name smoke：PASS；
- `rmw_dds_common` 生成 Fast RTPS 类型支持和 GraphCache 更新：PASS；
- `rmw_fastrtps_cpp` context 初始化、Fast DDS node 创建和 ROS graph 查询：PASS；
- `rmw_fastrtps_cpp` graph listener、graph reader/writer 回收：PASS；
- `rmw_fastrtps_cpp` participant、context 完整回收：PASS；
- `rcl` context、Fast DDS node 创建、shutdown 和完整回收：PASS；
- `rcl` steady clock、50 ms timer、`rcl_wait()` 唤醒、callback 和完整回收：PASS；
- VelaROS 单线程 executor 复用一个 wait set，3 次 timer callback 与 3 次
  subscription callback、完整回收：PASS；
- VelaROS 静态 rclcpp RAII Topic 3/3、SetBool Service/Client 1/1、Timer 3 次，
  executor、实体、Node、Context 完整回收：PASS；
- `rcutils` → NuttX syslog adapter 输出 2 条、无截断或非法级别：PASS；
- 白名单 uORB → ROS 2 `sensor_temp` 3 条、ROS 2 → uORB 持久控制设定值
  3 条、完整回收且无 bridge 线程：PASS；
- openvela Binder 服务 `openvela.velaros.runtime` 发现、跨 task AIDL 调用、
  单 task `poll()` 分派和干净停止：PASS；
- KVDB 持久配置写入、跨 task 读取和恢复，ROS talker/listener 使用配置中的
  Domain/participant：PASS；
- 3 条样本发送、3 条样本接收：PASS；
- 64 KiB DDS 内部线程栈：PASS；
- Publisher/Subscriber 退出和后台资源回收：PASS；
- `ps` 无 `DDSHelloWorldExample`、Publisher 或 Subscriber 残留：PASS；
- 最终 ELF 和构建元数据无 `/opt/ros` 或宿主 ROS 共享库污染：PASS；
- Linux ROS 2 Lyrical 与 openvela simulator 无 Agent 双向
  `std_msgs/String` 互操作，guest -> host 与 host -> guest 均 3 发 3 收：PASS；
- Linux ROS 2 Lyrical 与 openvela simulator 双向 `std_srvs/SetBool` 2/2，以及
  Fibonacci Action 两向 `SUCCEEDED(4)`/`CANCELED(5)`：PASS；
- DDS-only 完整验收 ELF SHA256：
  `61b99d51509012477bcf018ab89187ae1c4478d9df62d64853303e5f48d2193e`。
- 当前 RMW 集成候选 ELF SHA256：
  `186bb0de97ce925849d108c1b94686f0589b347eff57d02217a80d8a272a2f6a`
  （构建和 RMW 生命周期总验收 PASS）。
- 当前 `rcl` 最小客户端层候选 ELF SHA256：
  `982e15964bbe443b779568c3cde4daafd917545fb6f784ce5750c9bdf63bf181`
  （`rcl`、RMW 和 DDS 合并验收 PASS）。
- 当前包含单线程 executor 与双向主机互操作修复的最终 ELF SHA256：
  `19a4fcbcb720bd78fa6692dceb2f8328ac28deda0558bc086260d41ded8eefda`
  （核心、RMW、`rcl`、DDS 与 Linux Lyrical 双向验收均 PASS）。
- 当前包含 syslog/uORB 融合层的最终 ELF SHA256：
  `1d34f688b490bddfa868f54ecc520e6a92b6712ae2ef19672e0abaf81824b5bd`
  （653958336 bytes；核心、融合 smoke 与 Linux Lyrical 双向验收均 PASS）。
- 当前包含 uORB/syslog/KVDB/Binder 四平面融合的最终 ELF SHA256：
  `7a44d3f0b4a08beb6e43dd756103d605d53bf729d88a4e6edfb45a5db5d7f839`
  （650175048 bytes；核心、控制面、DDS 与 Linux Lyrical 双向验收均 PASS；删除
  通用 Binder examples 后比上一基线减少 3783288 bytes）。
- 当前包含有界 Action 的开发 ELF SHA256：
  `ed9b3928f3cf1f37fbfaf547698ecf55ad8a814624e20fe39416f8c08f3ce3a7`
  （Topic、Service、Action、产品 Twist/MoveRelative 与任务回收均 PASS）。
- 当前 ROS-only 发布 ELF SHA256：
  `82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`
  （完整 openVela 基线保存审计和 Fast DDS 静态 profile 门禁 PASS；Linux
  Lyrical → VelaROS Twist/uORB、MoveRelative 成功/取消与任务回收 PASS；
  固定共享缓冲 backend 已链接；Fibonacci 编译对象和 ELF 符号为 0）。

核心验收、guest 互操作与 host 节点记录分别写入工作区
`cmake_out/velaros-dds-sim-runtime.log`、
`cmake_out/velaros-ros2-host-runtime.log` 和
`cmake_out/velaros-ros2-host-node.log`。模拟器在 Ctrl-A x 关闭后其外层
launcher 可能以 139 退出；验收脚本只在 guest 侧全部断言通过后主动关闭模拟器，
不把这一已知 launcher teardown 行为误判为 DDS 失败。

## 升级门槛

升级到更高 ROS 2/Fast DDS 版本前，至少满足：

1. 新 ROS 2 发行版已经稳定发布，而非只使用 Rolling 的瞬时 revision；
2. `rmw_fastrtps_cpp` 声明支持目标 Fast DDS/Fast-CDR 组合；
3. openvela 完成干净全量编译和 simulator DDS HelloWorld；
4. Linux ROS 2 对端完成双向 pub/sub、发现、QoS 和长时间运行测试；
5. 更新 source lock、许可证清单、补丁和二进制体积/内存基线；
6. K1 实板重新验证网络、线程栈、时钟和异常日志。
