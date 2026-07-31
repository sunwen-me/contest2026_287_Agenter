# VelaROS ROS 2 Lyrical 最小依赖闭包

版本：2026-07-31  
范围：目标端五批 `ROS 2 runtime + rmw_dds_common + rmw_fastrtps_cpp + rcl + std_msgs`

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
security 和 enclave validation 核心源码，并以静态库连接现有
`rmw_fastrtps_cpp`。simulator 已验证：

```text
rcl_init_options_init
  -> rcl_init
  -> rcl_node_init
  -> Fast DDS participant/node
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
`std_msgs/msg/String` 所需的 11 个 C/Fast RTPS C 文件，目标端直接使用
`rosidl_typesupport_fastrtps_c` 符号，不加载动态类型支持库。

2026-07-31 已完成 AArch64 交叉编译，固件中新增：

```text
velaros_ros2_talker [count]
velaros_ros2_listener [count]
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
`tools/check_velaros_ros2_host.py`。外部 locator 源码已固化进
`rmw-fastrtps-9.4.8-openvela.patch`；该最后一项修改尚需下一轮目标重编译后
执行双向运行验收，不能把脚本就绪写成互操作已经 PASS。

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

## 尚未接入的下一层闭包

`rcl` publisher/subscription 与第一个用户消息已经接入。下一层按顺序加入：

1. QEMU 与宿主 ROS 2 Lyrical 双向 UDP 互操作验收；
2. 必要的 `rcl_interfaces` 生成消息；
3. `rcl_logging_interface` 与裁剪后的 YAML/参数边界；
4. timer/wait set 和最小 executor；
5. 所需的 `rclcpp 32.0.0` 子集。

Fast DDS 3.6.1、Fast-CDR 2.3.6 已独立完成 openvela simulator 验收。

## openvela 构建方式

比赛仓通过 manifest 将
`middleware/velaros_ros2` 映射到 `external/velaros_ros2`。上游源码恢复到：

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

`std_msgs/String` 使用独立的可重复生成入口：

```bash
tools/generate_velaros_std_msgs.sh
```

生成物位于 `external/velaros_ros2_generated/std_msgs`，共 11 个文件；
输入指纹为
`3395fc8d516b15cde4e0b6e5528edf0edca27d17c879224b84a7c8400d5b572d`。

构建配置启用：

```text
CONFIG_VELAROS_ROS2_CORE=y
CONFIG_VELAROS_ROS2_CORE_SMOKE=y
CONFIG_VELAROS_ROS2_RMW_DDS_COMMON=y
CONFIG_VELAROS_ROS2_RMW_DDS_COMMON_SMOKE=y
CONFIG_VELAROS_ROS2_RMW_FASTRTPS_CPP=y
CONFIG_VELAROS_ROS2_RMW_FASTRTPS_SMOKE=y
CONFIG_VELAROS_ROS2_RCL=y
CONFIG_VELAROS_ROS2_RCL_SMOKE=y
CONFIG_VELAROS_ROS2_INTEROP=y
```

构建完成后将生成六个 NSH 命令：

```text
velaros_core_smoke
velaros_rmw_dds_smoke
velaros_rmw_fastrtps_smoke
velaros_rcl_smoke
velaros_ros2_talker
velaros_ros2_listener
```

第一个检查 target-side `rcutils` allocator、`rmw_validate_node_name()` 和
Fast DDS 动态类型支持标识符；第二个检查生成 Fast RTPS 类型支持的序列化边界
以及 `GraphCache` 的 participant/node 更新；第三个检查 RMW context、Fast DDS
node、ROS graph 查询和完整清理；第四个检查 `rcl` context/node 的创建与
完整回收。四者均不加载主机 ROS 库。

## 交叉编译记录

截至 2026-07-31 的实际 AArch64 交叉编译结果：

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
18. `rcl` publisher/subscription 和两个目标端互操作命令已链接进 ELF
    `d0531983a42e81058d5ad5f238af35b2d63cf2eb3a203bac63e839ecdb5b0287`；
19. 同一 NuttX 中由两个独立 NSH task group 并发创建两个 Fast DDS participant
    会触发全局单例相关 recursive assert，因此不再把该场景当作主机互操作
    替代品；真实验收保持一个 guest participant 和一个 host participant；
20. QEMU NAT 外部 locator、17410/17411 UDP 重定向和主机 Lyrical 双向验收
    已完成源码与脚本封装，目标重编译及运行结果仍待记录。

上述源码差异保存在
`tools/patches/rcutils-7.1.1-openvela.patch`，Fast DDS 包装层差异保存在
`tools/patches/fastdds-3.6.1-openvela.patch`。当前证据证明 ROS 2 核心接口、
生成图消息、Fast RTPS 类型支持和 `rmw_dds_common` GraphCache 已完成目标编译
和运行 smoke；`rmw_fastrtps_cpp` 完整生命周期已通过，`rcl` 最小
context/node 生命周期也已接入并通过。publisher/subscription、timer、executor、
参数服务和 `rclcpp` 尚未接入，因此不写成“ROS 2 已经完整移植”。

构建日志：

```text
cmake_out/velaros-dds-sim-build.log
```

合并运行日志：

```text
cmake_out/velaros-dds-sim-runtime.log
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
