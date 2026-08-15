# Fast DDS 的 VelaROS 静态裁剪 Profile

版本：2026-08-02

## 结论

`CONFIG_FASTDDS_VELAROS_STATIC_PROFILE=y` 已建立为 VelaROS 产品配置的 Fast DDS
静态能力边界。它不是换成 micro-ROS，也没有修改 DDS/RTPS wire protocol；它让
openVela 上的 ROS 2 Lyrical 继续通过标准
`rcl -> rmw_fastrtps_cpp -> Fast DDS -> RTPS/UDPv4` 与 Linux ROS 2 通信，同时在
编译期删除 VelaROS 不使用的通用发现、运行时扩展、备用网络 transport，以及
Fast DDS 原生 SHM/DataSharing 后端。板内实时通信改走 openVela uORB，跨设备仍走
标准 CDR + RTPS/UDPv4。

profile 已通过双向 Topic、Service 和 Action 互操作测试。相同 openVela 发布配置下，
整机 Flash 减少 2,529,392 bytes（9.85%），其中 Fast DDS 链接 Flash 减少
2,478,993 bytes（14.37%）。没有用删除 LVGL、curl、mbedTLS、音视频、数据库或
openVela 调试设施来制造体积数字。

## 产品边界

VelaROS 采用静态生成和审计过的 ROS 接口，不在目标端加载任意 IDL 或类型插件。
因此产品的 Fast DDS 路径固定为：

```text
静态 ROS 2 Topic / Service / Action 类型
                  |
        rmw_fastrtps_cpp 9.4.8
                  |
  Fast DDS 3.6.1：SIMPLE PDP + SIMPLE EDP
                  |
       RTPS reliable / best-effort
                  |
          NuttX UDPv4 socket
```

保留的通信能力包括：

- ROS 2 Topic 的 Publisher/Subscription 和标准 CDR 类型支持；
- ROS 2 Service 的 Request/Reply 通道；
- ROS 2 Action 的 SendGoal、GetResult、CancelGoal、Feedback 和 Status 五通道；
- Reliable、Best Effort、History、Durability、Deadline、Liveliness 等 Fast DDS
  核心 QoS 实现；
- RTPS 分片、重传、匹配、participant/endpoint 发现和 ROS graph cache；
- WaitSet，以及 VelaROS 有界单线程 executor；
- UDPv4 单播；NuttX 网络允许时也保留 UDPv4 组播发现能力。

这里的 Service 和 Action 都是标准 ROS 2 DDS 映射，不是私有协议，也不依赖
micro-ROS Agent。

## 实际裁掉的源码

profile 在 `Fast-DDS/src/cpp/source.cmake` 直接排除 36 个编译单元。它们不进入
`libfastdds.a`，而不是仅依赖链接器碰巧回收。

| 能力组 | 排除的源码 | 为什么 VelaROS 不需要 |
|---|---|---|
| DDS TypeLookup Service | `fastdds/builtin/type_lookup_service/` 全部 5 个单元 | 产品类型均在构建期生成并本地注册，不需要向远端请求未知类型 |
| Fast DDS DDS-RPC API | `fastdds/rpc/` 全部 2 个单元 | 这是 Fast DDS 自带的通用 RPC 类型支持，不是 ROS 2 Service；ROS Service 继续走 RMW Request/Reply |
| Discovery Server 数据库 | `rtps/builtin/discovery/database/` 全部 5 个单元 | 产品使用 SIMPLE 分布式发现，不运行集中式 Discovery Server/Backup 数据库 |
| Discovery Server/Client EDP | `EDPClient`、`EDPServer`、`EDPServerListeners` 共 3 个单元 | 与 Discovery Server/Client 模式绑定 |
| 静态 XML Endpoint Discovery | `EDPStatic` 1 个单元 | VelaROS 使用 SIMPLE EDP 和静态生成类型，不维护另一套 endpoint XML 表 |
| Discovery Server/Client PDP | `PDPClient`、`PDPClientListener`、`PDPServer`、`PDPServerListener` 共 4 个单元 | 产品不支持 SERVER、CLIENT、SUPER_CLIENT 或 BACKUP discovery protocol |
| Discovery Server 定时事件 | `DSClientEvent`、`DServerEvent` 共 2 个单元 | 只服务于已删除的 Server/Client 发现模式 |
| 备用网络 transport | TCP 控制消息、acceptor、channel、transport interface、TCPv4、TCPv6 共 9 个单元，以及 `UDPv6Transport.cpp` | 产品传输边界固定为 NuttX UDPv4；这些实现不参与 ROS 2 Topic/Service/Action 的 DDS wire 语义 |
| Fast DDS 原生 SHM/DataSharing | `DataSharingListener.cpp`、`DataSharingNotification.cpp`、`DataSharingPayloadPool.cpp`、`SharedMemTransportDescriptor.cpp` 共 4 个单元；SHM transport source append 同时被关闭 | 板内普通消息由 uORB 替代，跨设备不能使用进程内 SHM；保留这套 host 进程共享内存机制只会重复 openVela 板内总线 |

Map 中 Fast DDS 的已链接对象数相应从 202 降为 166。生成的 Ninja 图会编译
183 个 Fast DDS C++/CXX 单元；二者口径不同，前者是最终被固件引用的对象，后者
包含构建了但被静态链接器回收的对象。

## 同时锁死的运行时路径

只从 source list 删除文件还不够，调用点也做了 profile 分支：

- participant discovery 只接受 `NONE` 或 `SIMPLE`；VelaROS 正常节点使用
  `SIMPLE`；
- endpoint discovery 强制使用 `EDPSimple`，禁用 `EDPStatic`；
- 不创建、发布或回收 TypeLookup Manager 及其 request/reply endpoints；
- 收到远端 endpoint 后直接按本地已注册类型匹配，不启动异步 TypeLookup；
- 跳过 Easy Mode 和自动 Discovery Server/Client 建立逻辑；
- builtin transport 只接受 `NONE`、`DEFAULT` 或 `UDPv4`，其中 `DEFAULT` 明确映射
  为 UDPv4；其他 builtin transport 选择会返回错误；
- XML transport descriptor 在 profile 下只创建 UDPv4，不再实例化 TCPv4、TCPv6
  或 UDPv6；保留的通用 TCP/TLS 解析 API 会明确返回 `XML_ERROR`；
- Discovery Server 环境处理不再自动补建 TCP/UDPv6 transport，locator 归一化也
  不再引用 TCP descriptor；
- participant 网络变化处理不再更新 Discovery Server 列表；
- 产品 profile 同时定义 `FASTDDS_SHM_TRANSPORT_DISABLED` 和
  `FASTDDS_DATASHARING_DISABLED`；DataReader/DataWriter、RTPS reader/writer、
  payload pool、participant、XML parser 和共享内存 watchdog 的调用点均在编译期
  关闭；
- 显式请求 `DataSharingKind::ON` 返回 `RETCODE_UNSUPPORTED`，而不是静默退化；
  AUTO/OFF 在产品 profile 中确定性使用普通 Topic payload pool 与 UDPv4；
- XML 中请求 SHM transport 会明确返回解析错误，不会创建一个无后端 descriptor；
- Fast DDS Statistics、内部开发日志和旧日志宏仍由发布配置保持关闭。

这些分支和源码排除互相校验：如果以后某个保留路径重新引用被删除的后端，链接会
直接失败，而不会静默把功能带回产品镜像。

## 明确保留、尚未继续裁的部分

为了避免以“能编过”为目标破坏高级通信，第一版 profile 有意保留以下代码：

- 本地 TypeObject registry 和生成类型元数据。ROS 2 TypeHash、RMW 类型支持及
  endpoint 匹配仍会用到；删除的是网络 TypeLookup Service，不是本地类型系统；
- DynamicData/XTypes 的本地基础实现、ContentFilteredTopic 和 DDS SQL filter；
- XML profile/parser。当前 Fast DDS 默认 QoS 和 RMW 初始化仍与这部分有耦合；
  VelaROS 产品节点不依赖外部 XML 文件，profile 下的网络 transport 类型已限缩；
- Persistence reader/writer 和 flow controller；
- `DataSharingQosPolicy` 的公共 QoS/序列化元数据。它属于 Fast DDS QoS 与 wire ABI，
  但产品端永远解析为关闭；原生 DataSharing listener、notification、payload pool、
  descriptor、segment 和 watchdog 实现均不进入归档或 ELF；
- DDS loaned-sample API。loan API 也可由普通 Topic payload pool 实现，不等同于
  Fast DDS 原生 SHM，因此没有随 SHM 后端一起误删；
- Fast DDS logging 的普通错误/告警路径。删除开发统计不等于删除故障信息。

这份清单是下一轮裁剪的审计边界。只有在拆开 XML/transport/dynamic type 的核心
耦合并补齐 QoS 回归后，才能继续从 source list 排除；目前不会为了一个更漂亮的
数字牺牲 Topic/Service/Action 或 ROS 2 互操作性。

## 体积结果

比较对象是同一 `goldfish-arm64-v8a-ap-velaros` defconfig、同一 AArch64 工具链和
`-O3`，唯一产品能力差异是 Fast DDS 静态 profile。数字来自链接 Map 的装载
section；带调试信息的 ELF 文件大小不是烧录体积。

| 指标 | profile 前 | profile 后 | 变化 |
|---|---:|---:|---:|
| 整机 Flash | 25,672,616 B | 23,143,224 B | -2,529,392 B（-9.85%） |
| 整机静态 RAM | 695,616 B | 651,680 B | -43,936 B（-6.32%） |
| 整机 `.text` | 22,744,696 B | 20,422,936 B | -2,321,760 B（-10.21%） |
| 整机 `.rodata` | 2,632,888 B | 2,461,672 B | -171,216 B（-6.50%） |
| 整机 `.data` | 295,032 B | 258,616 B | -36,416 B（-12.34%） |
| 整机 `.bss` | 400,584 B | 393,064 B | -7,520 B（-1.88%） |
| Fast DDS 链接 Flash | 17,248,198 B | 14,769,205 B | -2,478,993 B（-14.37%） |
| Fast DDS 链接对象 | 202 | 170 | -32 |

第二轮仅移除 TCP/UDPv6 后端；相对第一轮结果又减少整机 Flash 667,792 B
（2.80%）、静态 RAM 8,592 B（1.30%），其中 Fast DDS 链接 Flash 减少
665,861 B（4.31%）。

第三轮只移除 Fast DDS 原生 SHM/DataSharing，使用同一份当前产品 defconfig、源码
闭包和工具链前后重建。这里是独立、同功能口径的精确 ELF section 结果：

| 指标 | 删除前 | 删除后 | 变化 |
|---|---:|---:|---:|
| 整机 Flash | 23,485,960 B | 23,125,496 B | -360,464 B（-1.535%） |
| 整机静态 RAM | 663,640 B | 661,752 B | -1,888 B（-0.284%） |
| 整机 `.text` | 20,719,912 B | 20,376,888 B | -343,024 B |
| 整机 `.rodata` | 2,495,768 B | 2,479,416 B | -16,352 B |
| 整机 `.data` | 270,280 B | 269,192 B | -1,088 B |
| 整机 `.bss` | 393,360 B | 392,560 B | -800 B |
| Fast DDS 已链接对象 | 170 | 166 | -4 |

`codesize` 的 Map 归属分析把其中约 362.29 KiB Flash、772 B RAM 归到
`libfastdds.a`；它与上表的全 ELF section 口径不同，因此不混用百分比。

用于复核的本地制品：

- profile 前 ELF SHA256：
  `794d193d0192d734022595c985972c15e2867e2d4dfdbfde9286da4d0bab09a9`；
- profile 后发布 ELF SHA256：
  `46b1cc34cd756db2a9ea89c7caa9b29621171e1a706cbb0b79499c95a2482aeb`；
- 未修改源码连续执行两次发布构建均得到上述 SHA256，已确认 ELF 可重复构建；
- profile 前/后 Map：`cmake_out/velaros-before-fastdds-static-profile.map` 和
  `cmake_out/velaros-after-fastdds-static-profile-v2.map`。第一轮中间结果保留为
  `cmake_out/velaros-after-fastdds-static-profile.map`。

上述 SHA 和 Map 是 Fast DDS profile 前后体积测量快照。加入静态 rclcpp/Action
SDK 后的历史发布 ELF 为
`16fc2e7b4e11793a797173a85b406e8a9498f1254077633a7adf7bd4b53e5498`。当前产品
Release 再加入 `Twist`、`MoveRelative`、uORB 产品节点和固定共享缓冲 backend，
并关闭 Fibonacci，当前 ELF SHA256 为
`82f5b314d433c00be9c115831e8d14bad4b8f2dc5490db94d484e7fd81803505`；Map 装载
Flash 为 23,152,776 B，静态 RAM 为 728,376 B。静态 profile 门禁和产品
Topic/Action 验收仍 PASS；这些功能演进不改写历史 profile 前后对比口径。

Map 复核显示 `liblvgl.a`、`libcurl.a`、`libmbedtls.a`、`libfreetype.a`、
`libunqlite.a` 和 `libuorb.a` 的装载大小及对象数前后相同。发布配置门禁还要求
音视频、QuickJS、JPEG/PNG、libuv、Binder、KVDB、TCP/DNS、KASAN、gtest/ostest
等 openVela 能力继续启用。

## 验收结果

profile 已在 goldfish-arm64 simulator 完成以下测试：

- VelaROS guest -> Linux ROS 2 Lyrical：`std_msgs/String` 3/3；
- Linux ROS 2 Lyrical -> VelaROS guest：`std_msgs/String` 3/3；
- Linux ROS 2 Lyrical -> VelaROS Service：`std_srvs/SetBool` 2/2；
- host Action client -> guest Action server：SUCCEEDED + CANCELED；
- guest Action client -> host Action server：SUCCEEDED + CANCELED；
- 两向 Action 均覆盖 Goal、Result、Cancel、Feedback 和 Status；
- 产品 `geometry_msgs/Twist` -> openVela uORB：PASS；
- 产品 `MoveRelative` 成功、Feedback、Result 和取消：开发/Release 均 PASS；
- Release Fibonacci 编译对象和 ELF 符号：0；
- Fast DDS HelloWorld 3 SENT / 3 RECEIVED；
- rcutils、rmw_dds_common、rmw_fastrtps、rcl、timer/wait set、VelaROS executor、
  uORB 双向 bridge 和 Binder/KVDB 控制面 smoke 全部 PASS；
- 每轮结束后的 `ps` 中无 ROS/DDS 示例或后台任务残留。

完整通信回归使用启用 profile 的开发镜像，ELF SHA256 为
`5338a437b6fc0a3bbe9482c01d188f30ebede044e230790eded0016217179895`。通用开发
defconfig 已恢复为 profile 关闭，便于比较和诊断；产品 release defconfig 始终
启用 profile。最终发布 ELF 又单独通过产品 Topic/Action 和清理验收。

## 构建和门禁

构建产品镜像：

```bash
tools/build_velaros_release_sim.sh --clean --jobs 8
```

静态 profile 门禁：

```bash
tools/check_velaros_fastdds_profile.sh \
  ../cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-velaros
```

门禁读取 `.config`、`build.ninja`、`libfastdds.a` 和最终 ELF，确认 profile 宏已
到达编译器、36 个后端不在构建图中，原生 DataSharing/SharedMem 对象和关键符号
为零，并确认 SIMPLE PDP、SIMPLE EDP 和 UDPv4 仍存在。发布配置门禁还会自动调用
它。可重复源码恢复由 `tools/patches/fastdds-3.6.1-openvela.patch` 与
`tools/patches/fastdds-3.6.1-velaros-no-native-shm.patch` 分层固化，
`tools/restore_velaros_dds_sources.sh --check` 已验证通过。

通用 `goldfish-arm64-v8a-ap-fastdds` 开发配置保持 profile 关闭，上游 SHM/DataSharing
源码仍可用于对照回归；删除发生在 VelaROS 产品编译闭包，不修改 DDS wire ABI。

## SHM 替代边界

- 板内普通控制量和传感器消息：使用 uORB 与编译期白名单 bridge；这是已经落地并
  通过 simulator 验收的替代路径。
- 板内大消息：openVela 固定共享缓冲池、有界 lease、uORB descriptor 和静态
  `VelaBufferBackend` 已在 simulator 跑通；同 pool 端点映射同一 payload，其他
  endpoint 自动回落 CPU/CDR。真实产品大消息和 K1 DMA/cache 尚未验收，不能把
  该结果扩大成跨设备零拷贝。详见
  [`VELAROS_BUFFER_BACKEND.md`](VELAROS_BUFFER_BACKEND.md)。
- AMP/跨核：复用 RPMsg/uORB RPMsg 传 descriptor，payload 放在双方约定的共享池；
  不让每个 hart 都运行完整 DDS participant。
- 跨设备 ROS 2：继续标准 CDR + RTPS/UDPv4。物理跨设备不存在进程共享内存意义上
  的零拷贝，不能用板内 SHM 替代网络 wire protocol。

## 当前限制

这些结果是 goldfish simulator 结果，不等于 K1 实板指标。拿到板子后仍需验证：

- K1 Ethernet 上的单播/组播 discovery；
- Reliable/Best Effort、不同 history depth、大消息分片与丢包恢复；
- Topic/Service/Action 延迟、吞吐、CPU、峰值 heap 和各任务 stack；
- participant 反复创建/销毁和至少 30 分钟长稳；
- 用 K1 链接脚本和最终产品节点重新生成 ELF/Map 体积基线。
