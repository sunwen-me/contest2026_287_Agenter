# VelaROS Navigation Migration Handoff

## Scope

The default ROS-free LIO and navigation core from
`/home/sw/x86_lio_slam_ws/integration/x86_lio_slam` is now integrated into
the VelaROS target under `middleware/velaros`. The migration keeps LIO and
navigation independent of ROS 2, connects the target LIO runtime to the
versioned pose/map snapshot buffer, and sends commands through the persistent
`velaros_motion_command` uORB boundary.

This is the target migration of the x86 project's default Small Point-LIO +
navigation path. It does not copy Nav2, the ROS bag replay executable, or the
optional experimental Batch LIO/block-PCG and ISA-specific kernels into the
target profile.

## Target integration

The target library contains:

- `velaros_lio` with Small Point-LIO, SmallIVox, Super-LIO OctVox
  localization, global map, keyframes, dense pose graph/PGO, loop closure,
  dead reckoning, map-odom correction, and the target snapshot bridge;
- fixed-capacity `velaros_lio::Runtime` accepting bounded IMU and LiDAR POD
  records and publishing the LIO pose/map snapshot;
- fixed-capacity costmap and bounded A*;
- RPP/DWPP controller with bounded collision-arc checks;
- fixed two-slot pose/map snapshot publication;
- `NavigationPipeline` with goal, map-version, timeout, and emergency-stop
  handling;
- fixed POD `VelaRosNavigationAdapter`;
- `UorbMotionCommandSink` for the existing persistent uORB topic.

The build is controlled by:

```text
CONFIG_VELAROS_NAVIGATION
CONFIG_VELAROS_NAV_MAX_MAP_CELLS=16384
CONFIG_VELAROS_NAV_MAX_PATH_NODES=4096
CONFIG_VELAROS_NAVIGATION_SMOKE
```

The development simulator enables the smoke command. The release simulator
builds the navigation library but disables the smoke application.

## Command boundary

The adapter publishes bounded `linear_x_mps` and `angular_z_rps`, a zero
`linear_y_mps`, a monotonic timestamp, a 250 ms timeout, and a monotonic
sequence. The uORB message stores timestamp, forward speed, angular speed,
timeout, and sequence. Any nonzero lateral command is converted to a zero
velocity stop before uORB publication.

The target-specific uORB message remains in `velaros/uorb_topics.h`; it is
not exposed through the ROS-free navigation headers. `velaros_lio::Runtime`
is the current producer: it calls the migrated LIO system, converts its global
map to bounded points, and publishes the versioned snapshot consumed by
`NavigationPipeline`. No point cloud is serialized through ROS between LIO
and navigation.

## Validation

Host validation in the source project:

```text
cmake --build build -j2                         PASS
cmake --build build-navigation-core-only -j2    PASS
ctest --test-dir build --output-on-failure       2/2 PASS
benchmark_navigation                            1000/1000 PASS
planning_allocations=0, control_allocations=0
```

The VelaROS development simulator build completed with the existing aarch64
prebuilt toolchain:

```text
/home/sw/Dev/k1-workspace/prebuilts/tools/linux/x86_64/
```

Development ELF from the navigation-enabled build:

```text
/home/sw/Dev/k1-workspace/cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds/nuttx
SHA256: 7db77df9ba828d8b121bb320d4a925773ea1f36c2fe4d11190a2f5b272465ff2
```

The complete simulator acceptance, including the navigation smoke, passed:

```bash
cd /home/sw/Dev/k1-workspace/contest2026_287_Agenter
python3 tools/check_velaros_dds_sim.py --timeout 150
```

```text
VelaROS navigation smoke: PASS
iterations=40
bounds=1
metadata=1
stale_stop=1
emergency_stop=1
lateral_reject=1
```

The navigation smoke checks goal progress through the reference plant,
command bounds and metadata, stale-snapshot stop, emergency stop, and
lateral-velocity rejection through the real uORB sink.

The LIO smoke uses synthetic IMU plus a bounded 64-point LiDAR plane. It
exercises the migrated `SmallPointLioFrontend` and `SlamSystem`, asserts a
valid pose, non-empty global map, and changing map version, then ticks the
navigation pipeline against that snapshot and checks 12 motion-command uORB
publications:

```text
VelaROS LIO smoke: PASS frames=12 map_points=64 map_version=3 navigation_commands=12
```

The release simulator profile also completed successfully. Its static release
configuration gate passed, and both the navigation and LIO libraries are
linked while the development smoke commands remain disabled.

```text
/home/sw/Dev/k1-workspace/cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-velaros/nuttx
SHA256: 9287349c3ffd53f5c3b90b272c1c9466d4c7847e7b552f009a4c29371f870a65
VelaROS ROS-only trim and openVela baseline preservation: PASS
```

## Remaining work

- Connect the board's actual IMU/LiDAR driver ingress to the fixed
  `ImuSample`/`PointSample` POD boundary, including timestamping, frame
  calibration, queue back-pressure, and sensor-task ownership. The current
  repository has no concrete K1 sensor uORB topic/driver to reuse.
- Measure control period, target memory, command timeout behavior, and
  runtime stack usage on the final product configuration.
- Validate the navigation command consumer and localization/map timing on a
  K1 board when the RISC-V toolchain, board, and base hardware are available.

Do not describe the synthetic ingress simulator result as real-sensor, K1, or
hardware navigation acceptance. The LIO algorithm and its LIO-to-navigation
snapshot path are target-linked and runtime-smoke-tested; only hardware
sensor ingress and board acceptance remain outside that result.
