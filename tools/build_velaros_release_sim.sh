#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
CONFIG_NAME="goldfish-arm64-v8a-ap-velaros"
CONFIG_PATH="contest2026_287_Agenter/configs/${CONFIG_NAME}"
OUTPUT_DIR="${WORKSPACE_ROOT}/cmake_out/contest2026_287_Agenter_${CONFIG_NAME}"
DEFCONFIG="${CONTEST_ROOT}/configs/${CONFIG_NAME}/defconfig"
CONFIG_STAMP="${WORKSPACE_ROOT}/cmake_out/.velaros-release-defconfig.sha256"
CCACHE_ROOT="${VELAROS_CCACHE_DIR:-/tmp/velaros-ccache}"
JOBS="${VELAROS_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '8')}"
CLEAN=0
GENERATE_INTERFACES=1

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
      printf 'Usage: build_velaros_release_sim.sh [--clean] [--no-codegen] [--jobs N]\n'
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

[[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] || fail "jobs must be a positive integer"
[[ -x "${WORKSPACE_ROOT}/build.sh" ]] || fail "openvela build.sh is missing"
[[ -f "${DEFCONFIG}" ]] || fail "release defconfig is missing"

if ((GENERATE_INTERFACES == 1)); then
  OPENVELA_ROOT="${WORKSPACE_ROOT}" "${SCRIPT_DIR}/generate_velaros_ros2_interfaces.sh"
  OPENVELA_ROOT="${WORKSPACE_ROOT}" "${SCRIPT_DIR}/generate_velaros_std_msgs.sh"
  OPENVELA_ROOT="${WORKSPACE_ROOT}" "${SCRIPT_DIR}/generate_velaros_std_srvs.sh"
  OPENVELA_ROOT="${WORKSPACE_ROOT}" "${SCRIPT_DIR}/generate_velaros_action.sh"
fi

if ((CLEAN == 1)) && [[ -d "${OUTPUT_DIR}" ]]; then
  [[ "${OUTPUT_DIR}" == "${WORKSPACE_ROOT}/cmake_out/contest2026_287_Agenter_${CONFIG_NAME}" ]] ||
    fail "refusing to clean unexpected output: ${OUTPUT_DIR}"
  rm -rf -- "${OUTPUT_DIR}"
fi

DEFCONFIG_SHA256="$(sha256sum "${DEFCONFIG}" | cut -d' ' -f1)"
PREVIOUS_SHA256=""
[[ ! -f "${CONFIG_STAMP}" ]] || PREVIOUS_SHA256="$(<"${CONFIG_STAMP}")"
if [[ -d "${OUTPUT_DIR}" && "${PREVIOUS_SHA256}" != "${DEFCONFIG_SHA256}" ]]; then
  printf 'Release defconfig changed; refreshing dedicated output directory\n'
  rm -rf -- "${OUTPUT_DIR}"
fi

mkdir -p -- "${CCACHE_ROOT}/tmp" "${WORKSPACE_ROOT}/cmake_out"
printf '%s\n' "${DEFCONFIG_SHA256}" >"${CONFIG_STAMP}"
BUILD_LOG="${WORKSPACE_ROOT}/cmake_out/velaros-release-sim-build.log"

printf 'Building VelaROS release profile with %s jobs\n' "${JOBS}"
set +e
(
  cd "${WORKSPACE_ROOT}"
  env \
    -u AMENT_PREFIX_PATH -u CMAKE_PREFIX_PATH -u CMAKE_MODULE_PATH \
    -u COLCON_PREFIX_PATH -u LD_LIBRARY_PATH -u PKG_CONFIG_PATH \
    -u PYTHONPATH -u ROS_DISTRO -u ROS_VERSION -u ROS_PYTHON_VERSION \
    -u ROS_PACKAGE_PATH -u ROS_AUTOMATIC_DISCOVERY_RANGE \
    CCACHE_DIR="${CCACHE_ROOT}" CCACHE_TEMPDIR="${CCACHE_ROOT}/tmp" \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    ./build.sh "${CONFIG_PATH}/" --cmake "-j${JOBS}"
) 2>&1 | tee "${BUILD_LOG}"
BUILD_STATUS=${PIPESTATUS[0]}
set -e

((BUILD_STATUS == 0)) || fail "release build failed; see ${BUILD_LOG}"
grep -q 'build completed successfully' "${BUILD_LOG}" ||
  fail "build returned success without completion marker"
[[ -f "${OUTPUT_DIR}/nuttx" ]] || fail "release ELF was not generated"
"${SCRIPT_DIR}/check_velaros_release_config.sh" "${OUTPUT_DIR}"

printf '\nVelaROS release build completed\n'
printf 'ELF:    %s\n' "${OUTPUT_DIR}/nuttx"
printf 'SHA256: %s\n' "$(sha256sum "${OUTPUT_DIR}/nuttx" | cut -d' ' -f1)"
printf 'Log:    %s\n' "${BUILD_LOG}"
