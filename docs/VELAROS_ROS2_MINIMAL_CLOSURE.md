# VelaROS ROS 2 Lyrical 最小依赖闭包

版本：2026-08-02

范围：目标端七批 `ROS 2 runtime + rmw_dds_common + rmw_fastrtps_cpp + rcl + Topic + Service + Action`

## 结论

目标端使用锁定的 ROS 2 Lyrical 源码交叉编译，不链接
`/opt/ros/lyrical` 中的 amd64 库。`/opt/ros/lyrical` 只用于核对发行包版本、
运行主机侧 ROSIDL 生成器，以及后续 Linux 互操作。

根据本机 Lyrical `package.xml`，第一批真实运行时闭包为：

```text
rcutils 7.1.1
   |
   +------------------------------+
   |                              |
rosidl_buffer 5.2.1       rosidl_typesupport_interface 5.2.1
   |                              |
   +----------> rosidl_runtime_c 5.2.1
                       |
                       +--> rosidl_runtime_cpp 5.2.1 (header-only)
                       |
                       +--> rosidl_dynamic_typesupport 0.4.1
                                      |
                                      +--> rmw 7.10.1
```

`rosidl_buffer` 与 `rosidl_typesupport_interface` 都来自 `rosidl 5.2.1`
仓库。`rmw 7.10.1` 对 `rosidl_dynamic_typesupport` 的依赖不能从旧版 ROS 2
经验中省略。

## 第一批目标端组件

| 包 | 类型 | 目标端用途 |
|---|---|---|
| `rcutils` | C 静态库 | allocator、error、logging、time、字符串和基础容器 |
| `rosidl_buffer` | C++ 静态库 | ROSIDL buffer C helper |
| `rosidl_typesupport_interface` | 头文件 | type support 标识符和映射接口 |
| `rosidl_runtime_c` | C 静态库 | 字符串、sequence、type hash 和 type description |
| `rosidl_runtime_cpp` | 头文件 | C++ runtime 容器和 traits |
| `rosidl_dynamic_typesupport` | C 静态库 | `rmw` 公共动态类型接口依赖 |
| `rmw` | C 静态库 | RMW 公共类型、QoS、验证和选项接口 |

`rmw` 本身只是公共接口层，不包含 Fast DDS 实现。第三批已经加入真正创建
DomainParticipant 的 `rmw_fastrtps_cpp`，其当前运行边界见下文。

第二批在此基础上加入：

```text
rcpputils 2.14.5
       |
       +--> rmw_dds_common 6.0.0
       |        |
       |        +--> Gid / NodeEntitiesInfo / ParticipantEntitiesInfo
       |             generated C/C++ sources
       |
       +--> rosidl_dynamic_typesupport_fastrtps 0.5.1

rosidl_typesupport_fastrtps 3.9.5
       |
       +--> rmw_dds_common generated Fast RTPS C++ typesupport
                    |
                    +--> Fast-CDR 2.3.6
```

该切片已经能在目标端运行 `rmw_dds_common::GraphCache`，并验证 3 个图消息
的静态 Fast RTPS 类型支持。

## 第二批目标端组件

| 包 | 类型 | 目标端用途 |
|---|---|---|
| `rcpputils` | C++ 静态库 | `rmw_dds_common` 的文件、环境、线程名等基础支持 |
| `rosidl_dynamic_typesupport_fastrtps` | C++ 静态库 | Fast DDS 动态类型后端与标识符 |
| `rmw_dds_common` | C++ 静态库 | context、GID、QoS、GraphCache 和时间转换 |
| `rmw_dds_common` 生成消息 | C/C++ 生成源码 | ROS graph 的 3 个内部消息 |
| Fast RTPS C++ 类型支持 | C++ 生成源码 | 3 个内部消息的 Fast-CDR 序列化边界 |

## 第三批目标端组件

第三批继续加入：

```text
rmw_fastrtps_cpp 9.4.8
       |
       +--> rmw_fastrtps_shared_cpp 9.4.8
       +--> rmw_security_common
       +--> rosidl_typesupport_introspection_c/cpp
       +--> rosidl_buffer_backend_registry
       +--> tracetools target stub
       +--> rmw_dds_common generated Fast RTPS typesupport
       +--> Fast DDS 3.6.1 / Fast-CDR 2.3.6
```

上述库已由 openvela AArch64 工具链静态编译并链接进固件。simulator 已验证
`rmw_init()`、Fast DDS participant/node 创建和 `rmw_get_node_names()`；
graph listener、graph reader、graph writer、participant 和 context 均能完成
回收，第三批 RMW 生命周期状态为 **PASS**。

## 第四批最小 rcl 客户端层

第四批加入 `rcl 10.4.4` 的 context、init options、node、guard condition、
steady clock、timer、wait set、security 和 enclave validation 核心源码，
并以静态库连接现有
`rmw_fastrtps_cpp`。simulator 已验证：

```text
rcl_init_options_init
  -> rcl_init
  -> rcl_node_init
  -> Fast DDS participant/node
  -> steady clock + 50 ms timer
  -> rcl_wait + timer callback
  -> wait set/timer/clock fini
  -> rcl_node_fini
  -> rcl_shutdown
  -> rcl_context_fini
```

为保持第一版闭包可审计，当前明确不支持非空命令行参数、全局 remap、YAML
参数、rosout、Type Description service/type cache 和动态日志插件。空参数和
无 remap 路径由 NuttX 最小适配层实现。

## 第五批 std_msgs 发布/订阅层

第五批加入 `rcl` 的 publisher/subscription 源码和锁定的
`common_interfaces 5.9.2`。主机 Lyrical 生成器只导出
`std_msgs/msg/String` 和 `std_msgs/msg/Float64` 所需的 20 个 C/Fast RTPS C
文件，目标端直接使用 `rosidl_typesupport_fastrtps_c` 符号，不加载动态类型
支持库。String 用于标准主机互操作，Float64 用于首批 uORB 桥接机制验证。

截至 2026-08-02 已完成 AArch64 交叉编译，固件中新增：

```text
velaros_talker [count] [participant-id]
velaros_listener [count] [participant-id]
```

两者使用 `/velaros/chatter`、`std_msgs/String`、`rcl`、
`rmw_fastrtps_cpp` 和 Fast DDS UDP。QEMU 主机互操作使用固定端口：

```text
host 17410/udp -> guest 7410/udp  (RTPS metatraffic)
host 17411/udp -> guest 7411/udp  (RTPS user data)
guest -> host 10.0.2.2:7410/7411
```

主机端配置和双向验收脚本分别为
`tools/velaros_host_fastdds.xml`、`tools/velaros_host_node.py` 和
`tools/check_velaros_ros2_host.py`。QEMU 外部 locator、1200-byte RTPS 报文上限
和 `registration_only` 静态类型策略已固化进
`rmw-fastrtps-9.4.8-openvela.patch`。2026-08-02 重编译后，guest 到 host 与
host 到 guest 均完成 3 发 3 收，双方节点正常退出，双向运行验收为 PASS。

## 第六批 Client/Service 请求响应层

第六批加入 `rcl` client/service、VelaROS executor 的 bounded client/service 槽，
以及锁定 Lyrical `std_srvs/SetBool`。生成器导出 15 个 C/C++ 请求/响应及 Fast
RTPS type-support 所需文件，并删除 ServiceEventInfo、`SetBool_Event`、service
introspection 与 type-description graph；准确 TypeHash 作为小型静态 C 源码保存。

目标端服务 `/velaros/runtime/set_bridge` 使用一个调用方 task、一个 `rcl_wait_set`
和一个 service 槽处理请求，成功后通过现有 openVela KVDB 修改 bridge 状态。
2026-08-02 主机 ROS 2 Lyrical 连续两次调用均收到标准 SetBool 响应，目标端处理
2/2 且任务正常退出。该结果证明 ROS 2 标准 Request/Response 数据面，不代表
远程 Parameters 或全部 service introspection 已移植。

## 第七批有界 Action 层

第七批加入锁定 `rcl_action 10.4.4`、`example_interfaces 0.14.1` 及 Fibonacci
所需的 `action_msgs`、`builtin_interfaces`、`unique_identifier_msgs`。目标端仍
使用 ROS 2 标准 Action 五通道：

```text
SendGoal service   GetResult service   CancelGoal service
Feedback topic     Status topic
```

主机 Lyrical 生成器导出并裁剪为 58 个 C/Fast RTPS C 文件；ServiceEvent、运行时
Type Description 和目标端生成器不进入固件。`rosidl_action_type_support_t` 在目标端
由五个静态 Fast RTPS C handle 与锁定 TypeHash 组合，不提供动态类型加载。

VelaROS executor 在原有 wait set 中为每个 Action client 预留 2 个 subscription 和
3 个 client，为每个 Action server 预留 1 个 timer 和 3 个 service。默认资源上限
是 2 个并发 Goal、每个 Fibonacci 序列 32 项；无界 Goal/Result 缓存、每 Goal
线程和通用动态多线程 executor 均未引入。

goldfish simulator 与 ROS 2 Lyrical 已完成两向 Fibonacci 验收：VelaROS 和主机
分别担任 client/server，双方均覆盖 `SUCCEEDED(4)` 与 `CANCELED(5)`，反馈、状态、
取消响应和结果均收到，任务退出后 `ps` 无残留。该结论证明选定静态 Action 类型的
完整通信语义，不代表任意 Action 类型可在运行时装载。

## 第八批静态 rclcpp RAII 层

第八批恢复并锁定 `rclcpp 32.0.0` 源码作为 Lyrical API 语义基线，但不编译完整
上游库。VelaROS 提供 `rclcpp/rclcpp.hpp` 的源码级静态子集：`Context`、`Node`、
`QoS`、`Publisher`、`Subscription`、`Client`、`Service`、`WallTimer` 和
`SingleThreadedExecutor`。所有实体直接拥有现有 `rcl` handle；executor 复用
VelaROS C wait set，容量由产品节点显式给出，且不创建工作线程。

为此 std_msgs 和 std_srvs 生成闭包增加预生成 Fast RTPS C++ type-support。SetBool
仍裁掉 ServiceEvent 与 Type Description graph，但 C/C++ handle 都保留锁定的
Service/Request/Response TypeHash，满足 `rmw_fastrtps_cpp` 的 Service QoS 类型
一致性检查。

goldfish simulator 已验证 C++ Topic 3 发 3 回调、SetBool 1 次请求/服务回调/响应
回调、Timer 3 次和完整 RAII 回收。该 profile 不是完整 `librclcpp` ABI，也不包含
参数、rosout、组件加载、callback group、动态多线程 executor 或 intra-process
manager。详细接口与边界见
[`VELAROS_RCLCPP_STATIC_PROFILE.md`](VELAROS_RCLCPP_STATIC_PROFILE.md)。

## 第九批静态 rclcpp Action RAII 层

第九批在已有有界 C Action 与同一 wait set 上增加
`rclcpp_action/rclcpp_action.hpp` 源码级子集：Client/Server、两类 GoalHandle、
Goal/Feedback/Result/Cancel 回调，以及 succeed/abort/canceled。标准五通道和 Linux
ROS 2 wire 语义不变；server 默认 2 个 Goal 槽、client 1 个活动 Goal、Fibonacci
sequence 32 项，所有 Goal 执行由 caller-owned executor 分步推进。

goldfish simulator 已通过一次 `SUCCEEDED` 和一次收到 Feedback 后的
`CANCELED`，Goal callback 2 次、Feedback callback 7 次、Cancel callback 1 次，
任务回收无残留。完整上游 ABI、future、shared goal ownership、每 Goal 线程、
无界缓存和运行时 Action 类型加载仍明确排除。详细范围见
[`VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md`](VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md)。

## 主机侧依赖

以下依赖用于源码发布、生成或测试，不进入 openvela 固件：

- `ament_cmake*`、`colcon`；
- Python、`rosidl_cli`、`rosidl_parser`、`rosidl_pycommon`；
- `rosidl_generator_c/cpp` 和类型描述生成器；
- gtest/gmock、launch testing、lint、performance test；
- ROS 2 CLI 和 ament resource index。

消息生成采用 host/target 分离：

```text
ROS 2 Lyrical host
  -> rosidl generators
  -> 固定版本的 generated C/C++ sources
  -> 保存生成器版本和输入 IDL

openvela target
  -> Kconfig + CMake
  -> 编译 generated sources 和锁定的 runtime/RMW/Fast DDS
  -> 全静态链接
```

## 下一层闭包

`rcl` Topic、Client/Service、首批用户消息、最小单线程 executor、静态 rclcpp
RAII、syslog adapter、一个传感器/控制 Topic 的白名单 uORB bridge、KVDB 本地
运行配置和 Binder 控制面已经接入。后续不按桌面 ROS 2 包清单机械平移，而按
openVela 能力复用门禁加入：

1. Demo 确实需要远程参数时，再加入必要的 `rcl_interfaces`，薄映射到已经落地的
   KVDB 本地配置，不把 YAML 作为首版前置依赖；
2. 根据实机 Demo 把 Float64 验证映射替换为有语义的传感器/执行器白名单消息；
3. 按机器人 Demo 的真实接口增加下一批静态 Action traits 和生成类型，继续复用
   已完成的有界 C++ RAII，不移植 `rclcpp_action` 的通用动态 executor；
4. K1 实板网络和 Linux ROS 2 对端验收。

精简高级通信保留/裁剪矩阵及 dev/release 双配置见
[`VELAROS_COMMUNICATION_PROFILE.md`](VELAROS_COMMUNICATION_PROFILE.md)。

详细的复用、适配和禁止重复移植边界见
[`VELAROS_OPENVELA_INTEGRATION.md`](VELAROS_OPENVELA_INTEGRATION.md)。

Fast DDS 3.6.1、Fast-CDR 2.3.6 已独立完成 openvela simulator 验收。

## openvela 构建方式

比赛仓通过 manifest 将
`middleware/velaros` 映射到 `external/velaros`。上游源码恢复到：

```text
external/velaros_ros2_sources/
  rcutils/
  rosidl/
  rosidl_dynamic_typesupport/
  rmw/
  rcpputils/
  rmw_dds_common/
  rosidl_typesupport_fastrtps/
  rosidl_dynamic_typesupport_fastrtps/
  rmw_fastrtps/
  rcl/
  ...
```

源码恢复与校验：

```bash
tools/restore_velaros_ros2_sources.sh --check
```

缺失时：

```bash
tools/restore_velaros_ros2_sources.sh
```

`rmw_dds_common` 的 3 个消息在 host 侧使用锁定的 Lyrical ROSIDL 生成器，
只导出源码和头文件：

```bash
tools/generate_velaros_ros2_interfaces.sh --check
```

生成物位于
`external/velaros_ros2_generated/rmw_dds_common`，共 71 个
`.c/.cpp/.h/.hpp` 文件。生成输入指纹为：

```text
d9b07b0e490b513a4ba25852afd9f9bb1c29555ecdd1d1e55e437512eaa1b155
```

目录中不允许出现目标文件、静态/动态库、`/opt/ros` 路径或临时生成目录。
`tools/build_velaros_dds_sim.sh` 默认先做缓存校验/生成；`--no-codegen`
仅供已确认生成物完整时跳过。

`std_msgs/String` 和 `std_msgs/Float64` 使用独立的可重复生成入口：

```bash
tools/generate_velaros_std_msgs.sh
```

生成物位于 `external/velaros_ros2_generated/std_msgs`，共 20 个文件；
输入指纹为
`1049233872f2919089483e7ff4cdc7909764529ae1cacb2a2a7de75863fccf56`。

构建配置启用：

```text
CONFIG_VELAROS_CORE=y
CONFIG_VELAROS_CORE_SMOKE=y
CONFIG_VELAROS_RMW_DDS_COMMON=y
CONFIG_VELAROS_RMW_DDS_COMMON_SMOKE=y
CONFIG_VELAROS_RMW_FASTRTPS_CPP=y
CONFIG_VELAROS_RMW_FASTRTPS_SMOKE=y
CONFIG_VELAROS_RCL=y
CONFIG_VELAROS_RCL_SMOKE=y
CONFIG_VELAROS_SERVICES=y
CONFIG_VELAROS_INTEROP=y
CONFIG_VELAROS_EXECUTOR=y
CONFIG_VELAROS_SERVICE_GATEWAY=y
CONFIG_VELAROS_EXECUTOR_SMOKE=y
CONFIG_VELAROS_SYSLOG_ADAPTER=y
CONFIG_VELAROS_UORB_BRIDGE=y
CONFIG_VELAROS_OPENVELA_INTEGRATION_SMOKE=y
CONFIG_VELAROS_PLATFORM_CONFIG=y
CONFIG_VELAROS_RUNTIME_SERVICE=y
```

构建完成后将生成十个 NSH 命令：

```text
velaros_core_smoke
velaros_rmw_dds_smoke
velaros_rmw_fastrtps_smoke
velaros_rcl_smoke
velaros_talker
velaros_listener
velaros_executor_smoke
velaros_openvela_integration_smoke
velarosd
velarosctl
```

第一个检查 target-side `rcutils` allocator、`rmw_validate_node_name()` 和
Fast DDS 动态类型支持标识符；第二个检查生成 Fast RTPS 类型支持的序列化边界
以及 `GraphCache` 的 participant/node 更新；第三个检查 RMW context、Fast DDS
node、ROS graph 查询和完整清理；第四个检查 `rcl` context/node 的创建与
完整回收；executor 命令验证一个有界、无内部线程的 executor 使用同一 wait set
分派 3 次 timer callback 和 3 次 subscription callback；融合命令验证 syslog
adapter 2 条日志，以及 uORB → ROS、ROS → uORB 各 3 条。它们均不加载主机 ROS
库。`velarosd` 注册 Binder 服务并初始化 KVDB，`velarosctl` 跨 task 查询/修改
配置和停止服务；Domain/participant 配置被两个实际互操作节点读取。

## 交叉编译记录

截至 2026-08-02 的实际 AArch64 交叉编译结果：

1. ROS 2 源码已由 AArch64 openvela GCC 13.4.0 实际编译，编译命令中没有
   `/opt/ros/lyrical` include 或 library；
2. openvela 全局 `-Werror -Wundef -Wshadow` 与 Lyrical 源码存在警告策略差异，
   已在 VelaROS target 范围内加入
   `-Wno-undef/-Wno-shadow/-Wno-strict-prototypes`，未关闭全局错误检查；
3. NuttX 不提供 glibc 的 `program_invocation_name`，但提供 `getprogname()`，
   已加入 NuttX 分支；
4. NuttX 的 `strcasecmp/strncasecmp` 声明位于 POSIX `<strings.h>`，已加入
   平台 include；
5. `librcutils.a`、`librosidl_buffer.a`、`librosidl_runtime_c.a`、
   `librosidl_dynamic_typesupport.a` 和 `librmw.a` 已实际生成；
6. 全量构建在 `velaros_core_smoke.c` 编译处发现应用目标没有继承上述局部警告
   策略，已为该应用显式传入同一组 `COMPILE_FLAGS`；
7. Fast DDS 上游将 `${CMAKE_DL_LIBS}` 传播到最终链接。NuttX 没有独立
   `libdl.a`，因此在 Fast DDS NuttX 包装层同时清空普通变量和 cache，避免
   主机平台默认值 `dl` 泄漏到目标链接；
8. 最终固件链接成功，`velaros_core_smoke` 在 simulator 中验证
   `rcutils` allocator 和 `rmw_validate_node_name()` 均通过；
9. 第二批生成源码、Fast RTPS C++ 类型支持、`rcpputils`、
   `rosidl_dynamic_typesupport_fastrtps` 和 `rmw_dds_common` 已由相同
   AArch64 工具链编译为静态库；
10. `velaros_rmw_dds_smoke` 验证生成类型支持、GraphCache participant/node
    更新均通过；
11. 最终 ELF 与 `build.ninja` 的自动扫描未发现 `/opt/ros`、宿主 ROS 共享库
    或 amd64 ROS 二进制污染；
12. `rmw_fastrtps_shared_cpp/cpp` 及其安全、introspection、buffer registry 和
    tracing 边界已静态编译并链接；
13. `velaros_rmw_fastrtps_smoke` 的 context、node、graph 查询以及 graph
    listener/reader/writer、participant、context 完整回收通过；
14. 根因定位为 NuttX UDP 资源关闭时先 close socket、后 join 接收线程的竞态。
    NuttX 已使用 100 ms 有限 `poll()`，因此改为设置停止标志、join 接收线程、
    再 close socket；
15. 合并验收仍完成 discovery、3 发 3 收、RMW 生命周期以及退出无残留检查。
16. `rcl 10.4.4` 最小静态库已由同一工具链编译；`velaros_rcl_smoke` 验证
    context、Fast DDS node、shutdown 和 participant 回收，并正常返回 NSH。
17. `std_msgs 5.9.2` 的 String C/Fast RTPS C 生成源码已锁定并由目标工具链
    编译，输入与生成器版本均写入 manifest；
18. `rcl` publisher/subscription 和两个目标端互操作命令已链接进 ELF；
19. 同一 NuttX 中由两个独立 NSH task group 并发创建两个 Fast DDS participant
    会触发全局单例相关 recursive assert，因此不再把该场景当作主机互操作
    替代品；真实验收保持一个 guest participant 和一个 host participant；
20. QEMU NAT 外部 locator、17410/17411 UDP 重定向和主机 Lyrical 双向验收
    已完成源码与脚本封装；
21. host/guest 均限制 RTPS datagram 为 1200 bytes，并采用
    `fastdds.type_propagation=registration_only`；最终 guest/host 两个方向均
    3 发 3 收，进程正常退出，验收 ELF 为
    `ce343c138f45bb40b1f3a927590035e128be34a23d9400e5cfb97dbe9f9ed7b6`；
22. `rcl` steady clock、timer 和 wait set 已纳入最小静态库；simulator 中
    50 ms timer 经 `rcl_wait()` 唤醒，callback 恰好执行一次，wait set、timer
    和 clock 完整回收；回归后核心 DDS 与 Linux Lyrical 双向验收仍 PASS，ELF 为
    `196b8b70ecc70c1f600dfc59d23d1f4b860fd64fa7fc0543c6a4046de9ce05c9`。
23. VelaROS 最小单线程 executor 已目标编译并运行；它不创建 task、线程池或
    libuv loop，只在调用方 task 内复用一个 `rcl_wait_set`。simulator smoke
    完成 3 次 timer 发布和 3 次 subscription callback，随后完整回收；核心
    DDS 和 Linux Lyrical 双向回归仍 PASS，ELF 为
    `19a4fcbcb720bd78fa6692dceb2f8328ac28deda0558bc086260d41ded8eefda`。
24. `rcutils` → NuttX syslog adapter 和首批白名单 uORB ↔ ROS bridge 已目标编译
    并运行。bridge 复用已有 `sensor_temp`、uORB 持久队列和 VelaROS executor，
    不创建线程、第二套事件循环、运行时类型注册表或额外堆对象；simulator 中
    syslog 2 条、uORB → ROS 3 条、ROS → uORB 3 条均 PASS，核心 DDS 与 Linux
    Lyrical 双向回归仍 PASS，ELF 为
    `1d34f688b490bddfa868f54ecc520e6a92b6712ae2ef19672e0abaf81824b5bd`。
25. openvela KVDB/Binder 控制面已目标编译并运行。`velarosd` 注册
    `openvela.velaros.runtime`，以一个 64 KiB NuttX task 和 `poll()` 分派 AIDL
    请求；`velarosctl` 从独立 task 验证服务发现、11 次请求、KVDB 写入/读取/
    恢复和干净停止。ROS talker/listener 实际读取持久 Domain/participant；未引入
    YAML、ROS launch、Binder thread pool 或 `epoll`。删除无关通用 Binder 示例
    后，核心、融合、DDS 与 Linux Lyrical 双向回归均 PASS，ELF 为
    `7a44d3f0b4a08beb6e43dd756103d605d53bf729d88a4e6edfb45a5db5d7f839`。

上述源码差异保存在
`tools/patches/rcutils-7.1.1-openvela.patch`，Fast DDS 包装层差异保存在
`tools/patches/fastdds-3.6.1-openvela.patch`。当前证据证明 ROS 2 核心接口、
生成图消息、Fast RTPS 类型支持和 `rmw_dds_common` GraphCache 已完成目标编译
和运行 smoke；`rmw_fastrtps_cpp` 完整生命周期已通过，`rcl` 最小
context/node、timer/wait set 生命周期也已接入并通过。publisher/subscription、
`std_msgs/String/Float64`、最小单线程 executor、syslog adapter、首批白名单
uORB bridge、KVDB 本地配置和 Binder 控制面已接入；Linux Lyrical 双向通信通过。
静态 rclcpp RAII Topic/Service/Timer/Action 已接入；远程 ROS 参数服务、机器人
产品 Action 白名单和 K1 实板链路尚未接入，因此不写成“ROS 2 已经完整移植”。

构建日志：

```text
cmake_out/velaros-dds-sim-build.log
```

合并运行日志：

```text
cmake_out/velaros-dds-sim-runtime.log
cmake_out/velaros-ros2-host-runtime.log
cmake_out/velaros-ros2-host-node.log
```

DDS-only 完整验收 ELF SHA256：

```text
61b99d51509012477bcf018ab89187ae1c4478d9df62d64853303e5f48d2193e
```

当前 RMW 集成候选 ELF SHA256（构建和完整生命周期验收 PASS）：

```text
186bb0de97ce925849d108c1b94686f0589b347eff57d02217a80d8a272a2f6a
```

当前 `rcl` 最小客户端层候选 ELF SHA256（合并验收 PASS）：

```text
982e15964bbe443b779568c3cde4daafd917545fb6f784ce5750c9bdf63bf181
```

当前静态 rclcpp + Action RAII 开发验收 ELF SHA256（成功/取消、无警告、回收
PASS）：

```text
5338a437b6fc0a3bbe9482c01d188f30ebede044e230790eded0016217179895
```

当前 timer/wait set + ROS 2 Lyrical 双向互操作 ELF SHA256
（核心与双向验收均 PASS）：

```text
196b8b70ecc70c1f600dfc59d23d1f4b860fd64fa7fc0543c6a4046de9ce05c9
```

当前 VelaROS executor + ROS 2 Lyrical 双向互操作 ELF SHA256
（核心、executor 与双向验收均 PASS）：

```text
19a4fcbcb720bd78fa6692dceb2f8328ac28deda0558bc086260d41ded8eefda
```

当前 openVela syslog/uORB 融合层 + ROS 2 Lyrical 双向互操作 ELF SHA256
（核心、融合 smoke 与双向验收均 PASS）：

```text
1d34f688b490bddfa868f54ecc520e6a92b6712ae2ef19672e0abaf81824b5bd
```

当前 openVela uORB/syslog/KVDB/Binder 四平面融合 + ROS 2 Lyrical 双向互操作
ELF SHA256（650175048 bytes，全部自动验收 PASS）：

```text
7a44d3f0b4a08beb6e43dd756103d605d53bf729d88a4e6edfb45a5db5d7f839
```

当前包含静态 rclcpp RAII Topic/Service/Timer 的开发 ELF SHA256
（核心、RAII、融合与 DDS 生命周期自动验收 PASS）：

```text
ec7417d282f6969d2121d0fb8b9b5f04e23fa34767829e3354069b0af22503be
```
