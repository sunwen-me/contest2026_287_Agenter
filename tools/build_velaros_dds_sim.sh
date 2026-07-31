#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Reproducible, ROS-environment-free Fast DDS goldfish build.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
CONFIG_PATH="contest2026_287_Agenter/configs/goldfish-arm64-v8a-ap-fastdds"
OUTPUT_DIR="${WORKSPACE_ROOT}/cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds"
DEFCONFIG="${CONTEST_ROOT}/configs/goldfish-arm64-v8a-ap-fastdds/defconfig"
CONFIG_STAMP="${WORKSPACE_ROOT}/cmake_out/.velaros-dds-sim-defconfig.sha256"
CCACHE_ROOT="${VELAROS_CCACHE_DIR:-/tmp/velaros-ccache}"
JOBS="${VELAROS_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '8')}"
CLEAN=0
RESTORE=0
GENERATE_INTERFACES=1

usage()
{
  cat <<'EOF'
Usage: build_velaros_dds_sim.sh [options]

Options:
  --clean       Remove only the dedicated DDS simulator output before building
  --restore     Restore locked DDS sources before building
  --no-codegen  Skip the cached host-side ROS interface generation check
  --jobs N      Parallel build jobs
  -h, --help    Show this help

Environment:
  OPENVELA_ROOT       openvela workspace root
  VELAROS_CCACHE_DIR  writable ccache directory (default: /tmp/velaros-ccache)
  VELAROS_JOBS        default parallel build jobs

The build deliberately removes all inherited ROS/ament/colcon variables, so a
shell that auto-sources /opt/ros/lyrical cannot contaminate target discovery.
EOF
}

fail()
{
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

while (($# > 0)); do
  case "$1" in
    --clean)
      CLEAN=1
      shift
      ;;
    --restore)
      RESTORE=1
      shift
      ;;
    --no-codegen)
      GENERATE_INTERFACES=0
      shift
      ;;
    --jobs)
      (($# >= 2)) || fail "--jobs requires a value"
      JOBS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

[[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] || fail "jobs must be a positive integer"
[[ -x "${WORKSPACE_ROOT}/build.sh" ]] ||
  fail "build.sh not found under ${WORKSPACE_ROOT}"
[[ -f "${DEFCONFIG}" ]] ||
  fail "DDS simulator defconfig is missing"

if ((RESTORE == 1)); then
  OPENVELA_ROOT="${WORKSPACE_ROOT}" \
    "${SCRIPT_DIR}/restore_velaros_dds_sources.sh" --replace
fi

if ((GENERATE_INTERFACES == 1)); then
  OPENVELA_ROOT="${WORKSPACE_ROOT}" \
    "${SCRIPT_DIR}/generate_velaros_ros2_interfaces.sh"
  OPENVELA_ROOT="${WORKSPACE_ROOT}" \
    "${SCRIPT_DIR}/generate_velaros_std_msgs.sh"
fi

if ((CLEAN == 1)) && [[ -e "${OUTPUT_DIR}" ]]; then
  [[ "${OUTPUT_DIR}" == \
    "${WORKSPACE_ROOT}/cmake_out/contest2026_287_Agenter_goldfish-arm64-v8a-ap-fastdds" ]] ||
    fail "refusing to clean unexpected output: ${OUTPUT_DIR}"
  rm -rf -- "${OUTPUT_DIR}"
fi

DEFCONFIG_SHA256="$(sha256sum "${DEFCONFIG}" | cut -d' ' -f1)"
PREVIOUS_DEFCONFIG_SHA256=""
if [[ -f "${CONFIG_STAMP}" ]]; then
  PREVIOUS_DEFCONFIG_SHA256="$(<"${CONFIG_STAMP}")"
fi
if [[ -d "${OUTPUT_DIR}" ]] &&
   [[ "${PREVIOUS_DEFCONFIG_SHA256}" != "${DEFCONFIG_SHA256}" ]]; then
  printf 'Defconfig changed; refreshing dedicated output directory\n'
  rm -rf -- "${OUTPUT_DIR}"
fi

mkdir -p -- "${CCACHE_ROOT}/tmp" "${WORKSPACE_ROOT}/cmake_out"
printf '%s\n' "${DEFCONFIG_SHA256}" >"${CONFIG_STAMP}"
BUILD_LOG="${WORKSPACE_ROOT}/cmake_out/velaros-dds-sim-build.log"

printf 'Building VelaROS DDS simulator baseline with %s jobs\n' "${JOBS}"
printf 'Output: %s\n' "${OUTPUT_DIR}"
printf 'ccache: %s\n' "${CCACHE_ROOT}"

set +e
(
  cd "${WORKSPACE_ROOT}"
  env \
    -u AMENT_PREFIX_PATH \
    -u CMAKE_PREFIX_PATH \
    -u CMAKE_MODULE_PATH \
    -u COLCON_PREFIX_PATH \
    -u LD_LIBRARY_PATH \
    -u PKG_CONFIG_PATH \
    -u PYTHONPATH \
    -u ROS_DISTRO \
    -u ROS_VERSION \
    -u ROS_PYTHON_VERSION \
    -u ROS_PACKAGE_PATH \
    -u ROS_AUTOMATIC_DISCOVERY_RANGE \
    CCACHE_DIR="${CCACHE_ROOT}" \
    CCACHE_TEMPDIR="${CCACHE_ROOT}/tmp" \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    ./build.sh "${CONFIG_PATH}/" --cmake "-j${JOBS}"
) 2>&1 | tee "${BUILD_LOG}"
BUILD_STATUS=${PIPESTATUS[0]}
set -e

((BUILD_STATUS == 0)) || fail "build failed; see ${BUILD_LOG}"
grep -q 'build completed successfully' "${BUILD_LOG}" ||
  fail "build returned success without the expected completion marker"
[[ -f "${OUTPUT_DIR}/nuttx" ]] || fail "firmware ELF was not generated"

printf '\nVelaROS DDS simulator build completed\n'
printf 'ELF:    %s\n' "${OUTPUT_DIR}/nuttx"
printf 'SHA256: %s\n' "$(sha256sum "${OUTPUT_DIR}/nuttx" | cut -d' ' -f1)"
printf 'Log:    %s\n' "${BUILD_LOG}"
