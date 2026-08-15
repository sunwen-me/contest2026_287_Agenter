# VelaROS 与主机 ROS 2 双向通信验收记录

更新时间：2026-08-02

## 结论

openvela goldfish-arm64 模拟器中的裁剪 ROS 2 节点已与本机 ROS 2 Lyrical
通过 Fast DDS UDP 单播完成 Topic、Service 和 Action 通信：

1. `velaros_talker` 到主机 Python listener：3 发 3 收；
2. 主机 Python talker 到 `velaros_listener`：3 发 3 收；
3. 主机调用 guest `std_srvs/SetBool`：2/2；
4. guest Action server 与 host client：正常完成 + 取消；
5. host Action server 与 guest client：正常完成 + 取消；
6. 主机 `geometry_msgs/Twist /cmd_vel` 进入 guest 持久 uORB；
7. 产品 `MoveRelative` 完成成功 Goal、12 条 Feedback、Result 和取消 Goal；
8. guest 侧 `ps` 无 VelaROS Topic/Service/Action/产品节点任务残留。

ROS-only 发布配置已经从“Service-only SDK”推进为真实产品节点，并完成
host → guest Twist/uORB 与 MoveRelative 成功/取消验收：SHA256
`82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`。
发布配置完整保留 openVela 的 UI、媒体、网络、数据库、调试和测试能力，同时
不编译开发 Fibonacci 对象或符号。

最终验收 ELF：

```text
/home/sw/Dev/k1-workspace/cmake_out/
  contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds/nuttx
sha256: ed9b3928f3cf1f37fbfaf547698ecf55ad8a814624e20fe39416f8c08f3ce3a7
```

该结论只覆盖 simulator、选定的静态 ROS 2 类型和 Fast DDS
单播链路，不代表 K1 实板网络或完整 ROS 2 已经验收。

## 已执行验收

构建：

```bash
cd /home/sw/Dev/k1-workspace/contest2026_287_Agenter
tools/build_velaros_dds_sim.sh --jobs 8
```

双向互操作修复构建通过；随后加入 `rcl` time/timer/wait、VelaROS 单线程
executor、syslog adapter、白名单 uORB bridge、KVDB 配置、Binder 控制面和对应
smoke，最终构建通过。

核心闭包与 DDS 生命周期：

```bash
python3 tools/check_velaros_dds_sim.py --timeout 150
```

结果为 PASS，覆盖 `rcutils`、生成类型支持、`rmw_dds_common`、
`rmw_fastrtps_cpp`、`rcl` context/node、timer/wait set、VelaROS 单线程
executor、syslog/uORB 融合、KVDB/Binder 控制面、DDS 3 发 3 收和资源回收。

主机 ROS 2 双向通信：

```bash
python3 tools/check_velaros_ros2_host.py
```

关键输出：

```text
rcl steady clock init: PASS
rcl timer/wait set trigger: PASS
rcl wait set fini: PASS
rcl timer/clock fini: PASS

VelaROS executor timer callbacks: 3
VelaROS executor subscription callbacks: 3
VelaROS minimal single-thread executor smoke: PASS

VelaROS syslog adapter messages: 2
VelaROS uORB -> ROS samples: 3
VelaROS ROS -> uORB samples: 3
VelaROS openVela integration smoke: PASS

VelaROS runtime service ready: openvela.velaros.runtime
VelaROS Binder service discovery: PASS
VelaROS KVDB cross-task config: PASS
VelaROS Binder single-task poll loop: PASS
VelaROS service lifecycle: PASS
VelaROS runtime service stopped: requests=11

HOST RECEIVED: Hello from openvela VelaROS #1
HOST RECEIVED: Hello from openvela VelaROS #2
HOST RECEIVED: Hello from openvela VelaROS #3
HOST listener complete: RECEIVED=3

VelaROS RECEIVED: Hello from ROS 2 Lyrical host #1
VelaROS RECEIVED: Hello from ROS 2 Lyrical host #2
VelaROS RECEIVED: Hello from ROS 2 Lyrical host #3
VelaROS listener complete: RECEIVED=3

HOST service complete: RESPONSES=2

HOST ACTION RESULT: order=8 status=4
HOST ACTION RESULT: order=20 status=5
VelaROS ACTION CLIENT RESULT: status=4
VelaROS ACTION CLIENT RESULT: status=5

VelaROS <-> ROS 2 Lyrical communication acceptance: PASS
```

日志分别保存在：

```text
cmake_out/velaros-dds-sim-runtime.log
cmake_out/velaros-ros2-host-runtime.log
cmake_out/velaros-ros2-host-node.log
```

## 网络模型

QEMU user networking 使用以下确定性路径：

```text
host 127.0.0.1:17410 -> guest 10.0.2.15:7410  RTPS metatraffic
host 127.0.0.1:17411 -> guest 10.0.2.15:7411  RTPS user data
host 127.0.0.1:17412 -> guest 10.0.2.15:7412  participant 1 metatraffic
host 127.0.0.1:17413 -> guest 10.0.2.15:7413  participant 1 user data

guest 10.0.2.15 -> host 10.0.2.2:7410          RTPS metatraffic
guest 10.0.2.15 -> host 10.0.2.2:7411          RTPS user data
```

guest participant ID 默认为 0，也可由命令第二个参数指定 0..119：

```text
velaros_talker [count] [participant-id]
velaros_listener [count] [participant-id]
```

guest 绑定和 external redirect 端口按 participant ID 增加 `2 * id`；其 initial
peer 始终指向 host participant 0 的 7410。验收脚本为 participant 0/1 添加四个
redirect，避免连续短生命周期客户端复用 DDS GUID。

## 最终修复

### External locator

host 监听 `0.0.0.0:7410/7411`，并公告从 guest 可达的
`10.0.2.2:7410/7411`。external locator mask 必须是 `/32`；使用 `/24` 会把
不可从 host 路由的 guest 内部地址 `10.0.2.15` 判断为同一外部网络。

guest 监听 `7410/7411`，公告 host 可访问的
`127.0.0.1:17410/17411`，mask 同样固定为 `/32`。

### RTPS 报文尺寸

主机 ROS 节点默认会一次公告多个 SEDP endpoint。未限制时 Fast DDS 曾生成
5536-byte UDP datagram，超过 guest `eth0` 的 1500-byte MTU，表现为 SPDP
participant discovery 成功、SEDP endpoint discovery 失败。

host XML 和 guest QEMU participant 均设置：

```text
fastdds.max_message_size=1200
```

### 静态 ROS 2 类型

ROS 2 生成类型已在两侧本地注册，不需要通过 TypeLookup service 动态获取
TypeObject。此前 guest 收到 SEDP DATA 后进入 `async_get_type()`，回调未完成，
导致远端 endpoint 迟迟不能加入匹配表。

两侧现设置：

```text
fastdds.type_propagation=registration_only
```

这样仍按 ROS 2 类型名和 QoS 完成 DDS endpoint 匹配，同时移除本次最小闭包中
不需要的 TypeLookup 内置端点和线程。

### 验收器稳定性

guest 和 host 已使用独立日志，避免两个文件描述符互相覆盖。`HostNode` 也会在
判断短生命周期进程退出前排空 stdout 管道，避免节点已经 3/3 完成却被误报为
“status 0 before marker”。

Action host server 使用可重入 callback group 与多线程 executor，使同步执行回调
期间仍能处理 CancelGoal；最后一个 execute callback 返回后继续排空 executor 1 秒，
避免销毁 server 与待发送 GetResult 响应之间的竞态。

主机测试节点关闭 rosout、参数服务和 logger service，只保留 ROS graph 与
`/velaros/chatter` 所需端点。设置 `VELAROS_HOST_STRACE=1` 可保留网络系统调用
诊断；正常验收不启用。

## 可重复源码状态

以下检查均通过：

```bash
OPENVELA_ROOT=/home/sw/Dev/k1-workspace \
  tools/restore_velaros_dds_sources.sh --check
OPENVELA_ROOT=/home/sw/Dev/k1-workspace \
  tools/restore_velaros_ros2_sources.sh --check
git diff --check
```

Fast DDS、Fast-CDR、libc++abi、ROS 2 Lyrical 依赖 revision 以及所有 openvela
补丁都能由恢复脚本验证。QEMU 的 MTU、静态类型和 locator 配置已固化在
`tools/patches/rmw-fastrtps-9.4.8-openvela.patch`；NuttX 上的静态 buffer backend
入口单独固化在
`tools/patches/rmw-fastrtps-9.4.8-velaros-static-buffer.patch`。

## 当前边界与下一步

当前已完成的是无 Agent 的原生 DDS 路径：

```text
rcl -> rmw_fastrtps_cpp -> Fast DDS -> UDP
```

openvela 深度融合基线也已完成：uORB 板内数据面、rcutils→syslog、KVDB 本地
运行配置、Binder/servicemanager 控制面。Domain/participant 配置由实际
talker/listener 读取；`velarosd` 当前在 goldfish 由 NSH 手工启动，以便验收完整
生命周期。

尚未完成：

- K1 实板 Ethernet、PHY、DMA、IRQ、clock/reset 验证；
- K1 init 自启动、KVDB 真实持久分区和掉电恢复验收；
- 最终 Demo 的语义消息，以及对应的机器人 Action 静态 traits/生成类型；静态
  C++ Action RAII 基础层已经完成；
- 更多消息、QoS 组合、Action 类型和长时间压力测试；
- K1 上双向 ROS 2 Topic 与资源基线。

executor、syslog adapter 和首批白名单 uORB ↔ ROS Topic bridge 已完成 simulator
基线。bridge 复用现有 `sensor_temp`、uORB 持久队列与 executor callback，不创建
线程或第二套事件循环；当前 Float64 映射只是机制验证，不代表任意类型转换或最终
产品消息已经完成。

静态 rclcpp RAII Topic/Service/Timer 层也已完成 simulator 基线：Topic 3/3、
SetBool 1/1、Timer 3 次和完整回收。它是源码级子集，不代表完整上游 rclcpp ABI；
边界见 [`VELAROS_RCLCPP_STATIC_PROFILE.md`](VELAROS_RCLCPP_STATIC_PROFILE.md)。
后续功能必须按
[`VELAROS_OPENVELA_INTEGRATION.md`](VELAROS_OPENVELA_INTEGRATION.md)
先检查 openVela 已有能力；板内数据使用 uORB，DDS 保留给跨设备 ROS 2 互操作。

模拟器主动关闭后外层 emulator launcher 仍可能打印 segmentation fault/139；
该行为发生在所有 guest 断言和资源残留检查通过之后，不属于 DDS 或 ROS 2
进程退出失败。
