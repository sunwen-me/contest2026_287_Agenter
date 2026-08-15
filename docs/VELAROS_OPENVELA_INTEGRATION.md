# VelaROS 与 openvela 能力融合边界

版本：2026-08-02

## 目标

VelaROS 不是在 openvela 上再造一套 Linux 用户空间。它只保留 ROS 2 对外可见的
graph、Topic、QoS 和 DDS wire compatibility，并优先复用 openvela/NuttX 已有的
调度、时间、日志、配置、板内通信和 AMP 基础设施。

每增加一个 ROS 2 包或功能前，必须先回答三个问题：

1. 该功能是否属于 ROS 2 对外协议或 API 语义；
2. openvela 是否已经提供等价的本地系统能力；
3. 能否用薄适配层完成语义映射，而不移植第二套基础设施。

## 当前能力映射

| 能力 | openvela/NuttX 已有实现 | VelaROS 处理方式 | 明确不做 |
|---|---|---|---|
| 任务与优先级 | NuttX task/pthread、HPWORK、LPWORK | ROS executor 运行在调用方指定的 NuttX task；优先级和栈由 Kconfig/应用决定 | executor 内再创建隐藏线程池 |
| 事件循环 | `poll()`、NuttX wait primitive、已移植 libuv | DDS/RMW readiness 继续由 `rcl_wait()` 统一等待；非 ROS 异步业务直接使用 libuv/work queue | 为 ROS executor 再维护一套 libuv loop，或要求 Linux `epoll` |
| 时间与定时器 | `CLOCK_MONOTONIC`、POSIX timer、NuttX watchdog/work queue | `RCL_STEADY_TIME` 映射 `clock_gettime()`；`rcl_timer` 只保存 ROS timer 状态，由 wait set 计算唤醒时间 | 为每个 ROS timer 创建线程或复制硬件 timer 驱动 |
| 板内 pub/sub | openvela uORB，支持队列、`poll()` 和 RPMsg 扩展 | 高频传感器/执行器数据优先走 uORB；只在选定边界做 uORB ↔ ROS 2 Topic bridge | 同一板内所有数据先序列化为 DDS 再自发自收 |
| 板内大 payload | openvela 可控内存区、uORB/RPMsg descriptor | 已实现固定池、有界 lease、uORB descriptor 和静态 ROSIDL backend；真实产品消息与 K1 DMA/cache 仍待接入 | 继续保留 Fast DDS host 进程 SHM，或把跨设备 CDR 宣传为零拷贝 |
| 跨设备 ROS 网络 | NuttX UDP socket、`poll()`、网卡驱动 | 保留 `rmw_fastrtps_cpp + Fast DDS`，提供标准 ROS 2 graph、QoS 和 RTPS | 用 uORB 取代 ROS 2 wire protocol，或引入 micro-ROS Agent |
| 日志 | NuttX syslog、多 channel、时间戳、PID | 保留 ROS logger 名称/级别 API，后端薄适配到 syslog | 移植第二套 spdlog、文件轮转和动态日志插件 |
| 参数持久化 | openvela KVDB、settings/property 机制 | 已用 KVDB 保存 VelaROS 本地运行配置；仅在需要远端 ROS 参数服务时保留相应 ROS 接口 | 仅为参数引入完整 YAML 文件系统和 Linux 配置栈 |
| 进程/服务管理 | openvela init、service manager、Binder | 已用 Binder 实现 `velarosd`/`velarosctl` 板内控制面；K1 再接入系统自启动策略 | 用 ROS launch 替代设备启动和故障恢复机制 |
| AMP/跨核 | NuttX RPMsg、RPMsg virtio、uORB RPMsg 扩展 | AMP 场景优先通过 RPMsg/uORB 汇聚到一个 DDS gateway | 每个 hart 都运行完整 DDS participant，或首版自研 DDS RPMsg transport |
| 内存 | NuttX allocator、静态配置与资源上限 | executor 初始化时一次性分配有界实体槽，spin 阶段复用 wait set | 无界容器、运行期插件加载和隐式线程栈膨胀 |

上述“已有实现”是能力选择依据，不表示所有选项都必须在 K1 首版同时打开。
goldfish 开发验证配置当前已经启用 `CONFIG_UORB`、`CONFIG_LIBUV`、
`CONFIG_SCHED_HPWORK`、`CONFIG_SCHED_LPWORK`、`CONFIG_KVDB`、syslog channel、
Android Binder 和 service manager；K1 配置应按实际产品闭包逐项选择。
ROS-only 发布配置继续保留 libuv 供 openVela 其他模块使用，但 VelaROS 通信
自身仍以 NuttX wait/poll 和 caller-owned executor 实现，不新增 libuv 事件环；
两种配置边界见 `VELAROS_COMMUNICATION_PROFILE.md`。

## 最小 executor 的定位

VelaROS executor 不是新的操作系统调度器。它只完成 ROS 客户端层必须具备的
语义组合：

```text
caller-owned NuttX task
        |
velaros_executor_spin_once()
        |
one reusable rcl_wait_set
        +--> ready ROS subscription -> rcl_take -> callback
        +--> ready ROS client       -> rcl_take_response -> callback
        +--> ready ROS service      -> take request -> callback -> send response
        +--> ready ROS timer        -> rcl_timer_call
        |
rmw_fastrtps_cpp / Fast DDS / NuttX clock + socket wait primitives
```

当前实现不创建线程、不拥有 task、不创建 libuv loop，也不直接操作硬件 timer。
subscription/client/service 消息缓冲区、ROS 实体和 `rcl_timer_t` 均由调用方持有，
executor 只保存引用并按注册顺序单线程分派。

验收中的同 participant `std_msgs/String` 自发自收只用于同时覆盖 timer 与
subscription callback，不是产品侧板内消息架构。实际机器人数据路径应是：

```text
sensor/driver -> uORB -> selected VelaROS bridge -> ROS 2/DDS network
ROS 2/DDS network -> VelaROS executor callback -> selected uORB actuator topic
```

## 为什么 executor 不直接改用 libuv

libuv 已经存在于 openvela，但 `rcl_wait()` 等待的是 RMW subscription、guard
condition 和 DDS 状态，并不是一个可直接交给 `uv_poll_t` 的普通文件描述符。
强行在 executor 内再运行 `uv_run()` 会形成嵌套事件循环和两套 timeout 管理。

因此首版采用一个调用方 task + 一个 `rcl_wait_set`。需要同时处理普通 fd 的应用
可以让现有 libuv/work queue 处理本地业务，再通过 guard condition 或有界队列唤醒
ROS executor；是否增加该桥接必须以真实应用需求和时延数据为依据。

## 已落地的融合基线

### logging

`velaros_logging_syslog` 已把 `rcutils` 的 DEBUG/INFO/WARN/ERROR/FATAL 映射到
NuttX syslog priority，并保留 ROS logger 名称。适配器使用固定 384-byte 栈
缓冲，记录输出、截断和非法级别计数；不创建线程、不打开日志文件、不引入
spdlog 或动态插件。它替换的是 `rcutils` 全局 output handler，因此应在其他
task 开始记录 ROS 日志之前安装，结束时可恢复原 handler。

### uORB bridge

第一批桥接只实现两条编译期白名单映射：

| 方向 | openVela uORB | ROS 2 Topic/类型 | 语义 |
|---|---|---|---|
| uORB → ROS 2 | 已有 `sensor_temp` | `/velaros/sensor/temperature` / `std_msgs/Float64` | 非阻塞 `orb_check/copy` 后发布 |
| ROS 2 → uORB | `velaros_control_setpoint` | `/velaros/control/setpoint` / `std_msgs/Float64` | executor callback 写入 uORB 持久队列 |

通用 bridge 只保存调用方提供的消息缓冲和编译期 conversion callback，不分配堆、
不创建 task/线程、事件循环或运行时类型注册表。uORB → ROS 的 `pump()` 由应用
现有循环调用；ROS → uORB callback 直接注册到 VelaROS executor。控制设定值是
状态而非边沿事件，因此复用 `orb_advertise_multi_queue_persist()`，让晚启动的
执行器也能读取当前值。

这里的 `std_msgs/Float64` 是证明双向融合机制和静态类型支持的最小标量类型，
不是最终产品语义。实机闭包允许时，温度应换成 `sensor_msgs/Temperature`，控制量
应换成具体执行器消息；每增加一种类型仍需显式白名单和资源验收。

goldfish `velaros_openvela_integration_smoke` 已验证 syslog 2 条、uORB → ROS 3 条、
ROS → uORB 3 条，且全部 ROS/uORB 实体正常回收。该结果不代表 K1 驱动或实板
时延已经验证。

### Fast DDS 原生 SHM 的替代边界

VelaROS 产品 profile 已从编译图、`libfastdds.a` 和最终 ELF 删除 Fast DDS 原生
SHM transport 与 DataSharing 实现。它们解决的是同一主机多进程 DDS payload
共享；在 openVela 产品架构中，这与板内 uORB/RPMsg 总线职责重叠，且不能用于
物理跨设备 UDP 通信。

当前已经明确且落地的替代分为两条：板内普通控制量/传感器消息走白名单 uORB
bridge，该路径的 `orb_copy()` 仍复制小消息；板内大 payload 可放入固定共享池，
uORB/ROSIDL 只传 descriptor。发往外部 ROS 网络时仍转换为标准 CDR payload。

大 payload 基础设施的已实现数据路径是：

```text
producer -> fixed openVela shared buffer pool
         -> uORB descriptor {pool, slot, generation, lease, owner, length}
         -> VelaBufferBackend / DDS boundary
consumer -> release buffer by bounded ownership protocol
```

缓冲区数量、单块上限和 lease 数量在编译期固定，耗尽时返回有界错误；不建立
无界缓存，也不为每个 buffer 创建线程。goldfish 已验证 uORB 映射同一 payload
地址、ROSIDL 静态 backend 和非兼容端点 CPU/CDR fallback。K1 DMA/cache coherency、
RPMsg 跨核和真实相机/点云产品类型仍未实现，详细边界见
[`VELAROS_BUFFER_BACKEND.md`](VELAROS_BUFFER_BACKEND.md)。

### KVDB runtime config

本地设备配置不依赖 ROS YAML/parameter file，而是使用 openvela property/KVDB：

| KVDB key | 类型/范围 | 消费者 |
|---|---|---|
| `persist.velaros.bridge` | bool | bridge 策略 |
| `persist.velaros.domain` | 0..232 | `rcl_init_options_set_domain_id()` |
| `persist.velaros.participant` | 0..119 | Fast DDS participant ID |
| `persist.velaros.heartbeat_ms` | 10..60000 ms | heartbeat 策略 |

缺失或越界值在 `velarosd` 启动时写回有界默认值并 `property_commit()`；实际
`velaros_talker/listener` 在创建 context 前读取 Domain 和 participant，
命令行 participant 参数仍可作为单次显式覆盖。goldfish 开发配置和 ROS-only
发布配置都保留 openVela 的 UnQLite 后端，数据库文件为 `/data/persist.db`。K1
必须把 `CONFIG_KVDB_PERSIST_PATH` 绑定到真实持久分区。
当前自动验收覆盖跨 task 写入、读取、恢复和 commit；尚未把掉电注入测试写成
K1 验收结论。

### Binder runtime control

`velarosd` 向现有 openvela service manager 注册
`openvela.velaros.runtime`，AIDL 接口提供状态/配置读取、配置更新、请求计数和
停止。`velarosctl` 是独立 NuttX task，通过 Binder 完成服务发现和调用。

服务没有启用 Binder thread pool，而是在唯一的 64 KiB `velarosd` task 中调用
`IPCThreadState::setupPolling()`，再用 NuttX `poll()` 分派命令；客户端栈为
32 KiB。这样复用 openvela 已有 Binder/servicemanager，同时不引入 Linux
`epoll`、ROS launch 或新的后台线程池。自动验收已覆盖服务发现、11 次跨 task
请求、KVDB 配置一致性、RUNNING→STOPPING→STOPPED 和无 `velarosd` 残留。

goldfish 当前由 NSH 手工启动 `velarosd`，便于测试完整启动/停止生命周期；K1
产品配置后续应把它接入 openvela init，并定义重启与故障策略。修改 Domain 或
participant 只影响下一次创建的 ROS context，不热迁移已运行的 DDS participant。

## 后续移植门禁

### `rclcpp`

已接入静态源码级 RAII 子集：Context、Node、QoS、Publisher、Subscription、
Client、Service、WallTimer、Action Client/Server 和 SingleThreadedExecutor。
它直接拥有 `rcl/rcl_action` 实体并调用现有 VelaROS C executor；simulator 已通过
Topic 3/3、SetBool 1/1、Timer 3 次，以及 Action 成功/取消和完整回收。

仍不整体移植上游默认 executor、线程池、callback group、组件管理、parameters、
rosout 或动态类型加载。Action 使用固定 Goal/Result 容量、函数指针回调和同一
caller-owned executor，不引入每 Goal 线程。详细范围见
[`VELAROS_RCLCPP_STATIC_PROFILE.md`](VELAROS_RCLCPP_STATIC_PROFILE.md) 与
[`VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md`](VELAROS_RCLCPP_ACTION_STATIC_PROFILE.md)。

### logging 增量

syslog adapter 已完成。只有演示确实需要远程调整 logger level 时，才评估对应
ROS 2 服务；不得因为上游依赖方便而引入 spdlog 或动态共享库加载。

### parameters 增量

本地运行配置到 KVDB 的映射已经完成。只有演示确实需要 Linux ROS 2 远程读写
参数时，才加入 `rcl_interfaces` 参数消息与服务，并把它薄映射到现有 KVDB；
YAML 解析仍不是首版前置条件。

### uORB bridge 增量

首批一个传感器和一个控制 Topic 已完成 simulator 功能验证。扩展仍必须使用
白名单消息映射，不做任意运行时类型转换；K1 上板后补测队列深度、丢包、
端到端延迟和调度抖动。

### AMP

AMP 仍是第二阶段能力。Linux/openvela 或多 hart 之间先复用 RPMsg/uORB RPMsg；
只有在确认必须让远端核直接成为 DDS participant 时，才评估 Fast DDS custom
transport。

## 每项功能的验收问题

提交新依赖或新模块前必须在评审记录中说明：

1. 它提供的 ROS 2 对外语义是什么；
2. 对应的 openvela 能力和源码/Kconfig 在哪里；
3. 选择复用、适配或保留上游实现的理由；
4. 新增线程、栈、堆、socket 和持久化空间分别是多少；
5. 是否引入第二套事件循环、日志、配置或板内消息总线；
6. simulator 和 K1 实板分别如何验收；
7. 关闭该功能后是否仍可裁剪且不影响基础 DDS 链路。
