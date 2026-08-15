#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

OUTPUT_DIR="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${OUTPUT_DIR}/.config"
BUILD_NINJA="${OUTPUT_DIR}/build.ninja"
FASTDDS_CONFIG="${OUTPUT_DIR}/apps/external/fastdds/Fast-DDS/include/fastdds/config.hpp"
ELF_FILE="${OUTPUT_DIR}/nuttx"
NM_TOOL="${AARCH64_NM:-${PROJECT_ROOT}/../prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin/aarch64-none-elf-nm}"
BASELINE_DEFCONFIG="${PROJECT_ROOT}/configs/goldfish-arm64-v8a-ap-fastdds/defconfig"
RELEASE_DEFCONFIG="${PROJECT_ROOT}/configs/goldfish-arm64-v8a-ap-velaros/defconfig"

fail()
{
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

[[ -n "${OUTPUT_DIR}" ]] || fail "usage: check_velaros_release_config.sh OUTPUT_DIR"
[[ -f "${CONFIG_FILE}" ]] || fail "missing generated config: ${CONFIG_FILE}"
[[ -f "${BASELINE_DEFCONFIG}" ]] || fail "missing baseline defconfig"
[[ -f "${RELEASE_DEFCONFIG}" ]] || fail "missing release defconfig"

normalize_platform_defconfig()
{
  sed -n -E \
    -e 's/^# (CONFIG_[A-Za-z0-9_]+) is not set$/\1=n/' \
    -e '/^CONFIG_[A-Za-z0-9_]+=/p' "$1" | \
  grep -Ev '^CONFIG_(FASTDDS_(HelloWorldExample|NoExample|DEVELOPER_DIAGNOSTICS|ENABLE_OLD_LOG_MACROS|VELAROS_STATIC_PROFILE)|VELAROS_ACTION_SEQUENCE_CAPACITY|VELAROS_BUFFER_(BACKEND|BACKEND_SMOKE|POOL_SLOTS|POOL_SLOT_SIZE|POOL_MAX_LEASES)|VELAROS_ROBOT_NODE|VELAROS_(CORE_SMOKE|RMW_DDS_COMMON_SMOKE|RMW_FASTRTPS_SMOKE|RCL_SMOKE|INTEROP|ACTION_FIBONACCI|ACTION_INTEROP|EXECUTOR_SMOKE|RCLCPP|RCLCPP_SMOKE|RCLCPP_ACTION_SMOKE|AMP_SMOKE|NAVIGATION|NAVIGATION_SMOKE|NAV_MAX_MAP_CELLS|NAV_MAX_PATH_NODES|LIO_SMOKE|OPENVELA_INTEGRATION_SMOKE))=' | \
    sort
}

if ! platform_diff="$(diff -u \
    <(normalize_platform_defconfig "${BASELINE_DEFCONFIG}") \
    <(normalize_platform_defconfig "${RELEASE_DEFCONFIG}"))"; then
  printf '%s\n' "${platform_diff}" >&2
  fail "release defconfig changed settings outside the ROS/DDS trim allowlist"
fi

required=(
  FASTDDS FASTDDS_NoExample FASTDDS_VELAROS_STATIC_PROFILE
  VELAROS_CORE VELAROS_FASTRTPS_TYPESUPPORT
  VELAROS_RMW_DDS_COMMON VELAROS_RMW_FASTRTPS_CPP
  VELAROS_RCL VELAROS_SERVICES VELAROS_EXECUTOR
  VELAROS_RCLCPP
  VELAROS_ACTIONS
  VELAROS_ROBOT_NODE
  VELAROS_SYSLOG_ADAPTER VELAROS_UORB_BRIDGE
  VELAROS_BUFFER_BACKEND
  LIB_EIGEN VELAROS_LIO
  VELAROS_PLATFORM_CONFIG VELAROS_RUNTIME_SERVICE
  VELAROS_SERVICE_GATEWAY UORB KVDB KVDB_UNQLITE UNQLITE
  ANDROID_BINDER ANDROID_SERVICEMANAGER LIBC_NETDB NET NET_SOCKOPTS
  NET_UDP NET_IGMP NET_LOOPBACK NET_LOCAL
  GRAPHICS_LVGL AUDIO VIDEO INTERPRETERS_QUICKJS LIBUV UTILS_CURL
  CRYPTO_MBEDTLS LIB_FREETYPE LIB_JPEG_TURBO LIB_PNG
  LIB_GOOGLETEST TESTING_CXXTEST TESTING_OSTEST
  DEBUG_FEATURES DEBUG_SYMBOLS ALLSYMS MM_KASAN FDCHECK FDSAN
  SYSTEM_POPEN NET_TCP NETDB_DNSCLIENT
)

for symbol in "${required[@]}"; do
  grep -qx "CONFIG_${symbol}=y" "${CONFIG_FILE}" ||
    fail "required release capability CONFIG_${symbol}=y is missing"
done

grep -qx 'CONFIG_KVDB_PERSIST_PATH="/data/persist.db"' "${CONFIG_FILE}" ||
  fail 'openVela baseline UnQLite path must remain /data/persist.db'
grep -qx 'CONFIG_VELAROS_BUFFER_POOL_SLOTS=4' "${CONFIG_FILE}" ||
  fail 'VelaROS release buffer pool must keep four bounded slots'
grep -qx 'CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE=16384' "${CONFIG_FILE}" ||
  fail 'VelaROS release buffer slots must keep the reviewed 16 KiB size'
grep -qx 'CONFIG_VELAROS_BUFFER_POOL_MAX_LEASES=4' "${CONFIG_FILE}" ||
  fail 'VelaROS release buffer slots must keep four bounded leases'
grep -qx 'CONFIG_VELAROS_LIO_MAX_LOCAL_VOXELS=32768' "${CONFIG_FILE}" ||
  fail 'VelaROS release LIO local voxel capacity changed'
grep -qx 'CONFIG_VELAROS_LIO_MAX_GLOBAL_POINTS=16384' "${CONFIG_FILE}" ||
  fail 'VelaROS release LIO global map capacity changed'
grep -qx 'CONFIG_VELAROS_LIO_MAX_CLOUD_POINTS=4096' "${CONFIG_FILE}" ||
  fail 'VelaROS release LIO input cloud capacity changed'

forbidden=(
  FASTDDS_DEVELOPER_DIAGNOSTICS FASTDDS_ENABLE_OLD_LOG_MACROS
  FASTDDS_HelloWorldExample VELAROS_CORE_SMOKE
  VELAROS_RMW_DDS_COMMON_SMOKE VELAROS_RMW_FASTRTPS_SMOKE
  VELAROS_RCL_SMOKE VELAROS_INTEROP
  VELAROS_ACTION_FIBONACCI
  VELAROS_ACTION_INTEROP
  VELAROS_EXECUTOR_SMOKE VELAROS_RCLCPP_SMOKE
  VELAROS_RCLCPP_ACTION_SMOKE
  VELAROS_AMP_SMOKE
  VELAROS_OPENVELA_INTEGRATION_SMOKE
  VELAROS_BUFFER_BACKEND_SMOKE
  VELAROS_LIO_SMOKE
)

for symbol in "${forbidden[@]}"; do
  if grep -Eq "^CONFIG_${symbol}=(y|m)$" "${CONFIG_FILE}"; then
    fail "forbidden release feature CONFIG_${symbol} is enabled"
  fi
done

if [[ -f "${BUILD_NINJA}" ]]; then
  grep -q 'FASTDDS_VELAROS_STATIC_PROFILE' "${BUILD_NINJA}" ||
    fail "Fast DDS VelaROS static profile did not reach compiler definitions"
  for token in __INTERNALDEBUG FASTDDS_STATISTICS ENABLE_OLD_LOG_MACROS_; do
    ! grep -q "${token}" "${BUILD_NINJA}" ||
      fail "Fast DDS diagnostic token leaked into release build: ${token}"
  done
  for fibonacci_source in fibonacci__functions.c.o \
      fibonacci__type_support_c.cpp.o; do
    ! grep -q "${fibonacci_source}" "${BUILD_NINJA}" ||
      fail "development Fibonacci source entered the product build graph: ${fibonacci_source}"
  done
  for buffer_source in velaros_buffer_pool.c velaros_buffer_backend.cpp; do
    grep -q "${buffer_source}" "${BUILD_NINJA}" ||
      fail "VelaROS buffer backend source is missing from the product build: ${buffer_source}"
  done
  for lio_source in navigation/lio_runtime.cpp \
      lio/src/map/super_lio_octvox_map.cpp \
      lio/src/frontend/small_point_lio_frontend.cpp \
      lio/src/map/global_map.cpp lio/src/system/slam_system.cpp; do
    grep -q "${lio_source}" "${BUILD_NINJA}" ||
      fail "VelaROS LIO source is missing from the product build: ${lio_source}"
  done
  ! grep -q 'velaros_buffer_backend_smoke.cpp' "${BUILD_NINJA}" ||
    fail 'VelaROS buffer backend smoke entered the product build graph'
  ! grep -q 'velaros_lio_smoke.cpp' "${BUILD_NINJA}" ||
    fail 'VelaROS LIO smoke entered the product build graph'
fi

[[ -f "${ELF_FILE}" ]] || fail "missing release ELF: ${ELF_FILE}"
[[ -x "${NM_TOOL}" ]] || fail "missing AArch64 nm tool: ${NM_TOOL}"
"${NM_TOOL}" -S -C "${ELF_FILE}" | grep -F '0000000000010000 b g_payloads' >/dev/null ||
  fail 'release ELF does not contain the reviewed 64 KiB VelaROS payload pool'
"${NM_TOOL}" -C "${ELF_FILE}" | grep -F ' T velaros_buffer_acquire' >/dev/null ||
  fail 'release ELF does not contain the VelaROS fixed pool API'
"${NM_TOOL}" -C "${ELF_FILE}" | grep -F ' T velaros::create_buffer_backend()' >/dev/null ||
  fail 'release ELF does not contain the static ROSIDL VelaROS backend factory'
if "${NM_TOOL}" -C "${ELF_FILE}" |
    grep -F 'velaros_buffer_backend_smoke_main' >/dev/null; then
  fail 'VelaROS buffer backend smoke command leaked into the release ELF'
fi

[[ -f "${FASTDDS_CONFIG}" ]] || fail "missing generated Fast DDS config header"
grep -qx '/\* #undef FASTDDS_STATISTICS \*/' "${FASTDDS_CONFIG}" ||
  fail "Fast DDS Statistics module is enabled in the generated config"

# Three statistics namespace QoS/API adapter files are unconditional Fast DDS
# sources.  Reject only the backend objects guarded by FASTDDS_STATISTICS.
for object in DomainParticipantImpl DomainParticipantStatisticsListener \
    MonitorService MonitorServiceListener StatisticsReaderImpl StatisticsBase \
    StatisticsWriterImpl monitorservice_typesPubSubTypes \
    monitorservice_typesTypeObjectSupport typesPubSubTypes \
    typesTypeObjectSupport; do
  if find "${OUTPUT_DIR}/apps/external/fastdds/Fast-DDS/src/cpp" \
      -path '*/statistics/*' \
      \( -name "${object}.cpp.o" -o -name "${object}.cxx.o" \) \
      -print -quit 2>/dev/null | grep -q .; then
    fail "Fast DDS Statistics backend object was compiled: ${object}"
  fi
done

"${SCRIPT_DIR}/check_velaros_fastdds_profile.sh" "${OUTPUT_DIR}"

printf 'VelaROS ROS-only trim and openVela baseline preservation: PASS\n'
