#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Generate ROS 2 Lyrical interface C/C++ sources on the host.  Only generated
# source files are exported; no host object, archive, or shared library crosses
# the openvela target boundary.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
LOCK_FILE="${SCRIPT_DIR}/velaros-ros2-sources.lock"
SOURCE_ROOT="${WORKSPACE_ROOT}/external/velaros_ros2_sources"
PACKAGE_ROOT="${SOURCE_ROOT}/rmw_dds_common/rmw_dds_common"
OUTPUT_PARENT="${WORKSPACE_ROOT}/external/velaros_ros2_generated"
OUTPUT_ROOT="${OUTPUT_PARENT}/rmw_dds_common"
LOG_DIR="${WORKSPACE_ROOT}/cmake_out"
LOG_FILE="${LOG_DIR}/velaros-ros2-interface-generation.log"
ROS_SETUP="${VELAROS_HOST_ROS_SETUP:-/opt/ros/lyrical/setup.bash}"
FORCE=0
CHECK_ONLY=0
WORK_ROOT=""
STAGE_ROOT=""

usage()
{
  cat <<'EOF'
Usage: generate_velaros_ros2_interfaces.sh [options]

Options:
  --check       Verify the generated source set without regenerating it
  --force       Regenerate even when the input fingerprint is unchanged
  -h, --help    Show this help

Environment:
  OPENVELA_ROOT          openvela workspace root
  VELAROS_HOST_ROS_SETUP host ROS setup file

/opt/ros/lyrical is used only as a host code generator.  The exported result
contains source and header files only; target compilation remains ROS-free.
EOF
}

fail()
{
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

cleanup()
{
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf -- "${WORK_ROOT}"
  fi
  if [[ -n "${STAGE_ROOT}" && -d "${STAGE_ROOT}" ]]; then
    rm -rf -- "${STAGE_ROOT}"
  fi
}

trap cleanup EXIT

while (($# > 0)); do
  case "$1" in
    --check)
      CHECK_ONLY=1
      shift
      ;;
    --force)
      FORCE=1
      shift
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

((CHECK_ONLY == 0 || FORCE == 0)) ||
  fail "--check and --force cannot be used together"
[[ -f "${LOCK_FILE}" ]] || fail "source lock is missing: ${LOCK_FILE}"
[[ -f "${ROS_SETUP}" ]] || fail "host ROS setup is missing: ${ROS_SETUP}"
[[ -f "${PACKAGE_ROOT}/package.xml" ]] ||
  fail "rmw_dds_common source is missing; run restore_velaros_ros2_sources.sh"
[[ -f "${SOURCE_ROOT}/rmw_dds_common/.velaros-source-revision" ]] ||
  fail "rmw_dds_common source revision marker is missing"

# shellcheck disable=SC1090
source "${LOCK_FILE}"

[[ "$(<"${SOURCE_ROOT}/rmw_dds_common/.velaros-source-revision")" == \
   "${RMW_DDS_COMMON_REV}" ]] ||
  fail "rmw_dds_common source does not match the lock"

check_deb_version()
{
  local package_name="$1"
  local expected_version="$2"
  local actual_version

  actual_version="$(dpkg-query -W -f='${Version}' "${package_name}" 2>/dev/null)" ||
    fail "required host generator package is missing: ${package_name}"
  [[ "${actual_version}" == "${expected_version}" ]] ||
    fail "${package_name}=${actual_version}, expected ${expected_version}"
}

check_deb_version ros-lyrical-ament-cmake "${AMENT_CMAKE_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-default-generators \
  "${ROSIDL_DEFAULT_GENERATORS_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-generator-c "${ROSIDL_GENERATOR_C_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-generator-cpp "${ROSIDL_GENERATOR_CPP_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-typesupport-c "${ROSIDL_TYPESUPPORT_C_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-typesupport-cpp "${ROSIDL_TYPESUPPORT_CPP_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-typesupport-fastrtps-c \
  "${ROSIDL_TYPESUPPORT_FASTRTPS_C_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-typesupport-fastrtps-cpp \
  "${ROSIDL_TYPESUPPORT_FASTRTPS_CPP_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-typesupport-introspection-c \
  "${ROSIDL_TYPESUPPORT_INTROSPECTION_C_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-typesupport-introspection-cpp \
  "${ROSIDL_TYPESUPPORT_INTROSPECTION_CPP_DEB_VERSION}"

INPUT_SHA256="$(
  {
    sha256sum \
      "${PACKAGE_ROOT}/msg/Gid.msg" \
      "${PACKAGE_ROOT}/msg/NodeEntitiesInfo.msg" \
      "${PACKAGE_ROOT}/msg/ParticipantEntitiesInfo.msg"
    printf '%s\n' \
      "${RMW_DDS_COMMON_REV}" \
      "${AMENT_CMAKE_DEB_VERSION}" \
      "${ROSIDL_DEFAULT_GENERATORS_DEB_VERSION}" \
      "${ROSIDL_GENERATOR_C_DEB_VERSION}" \
      "${ROSIDL_GENERATOR_CPP_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_C_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_CPP_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_FASTRTPS_C_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_FASTRTPS_CPP_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_INTROSPECTION_C_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_INTROSPECTION_CPP_DEB_VERSION}"
  } | sha256sum | cut -d' ' -f1
)"

GENERATED_DIRS=(
  rosidl_generator_c
  rosidl_generator_cpp
  rosidl_typesupport_c
  rosidl_typesupport_cpp
  rosidl_typesupport_fastrtps_c
  rosidl_typesupport_fastrtps_cpp
  rosidl_typesupport_introspection_c
  rosidl_typesupport_introspection_cpp
)

validate_output()
{
  local root="$1"
  local directory
  local invalid_file
  local generated_count

  [[ -f "${root}/generation.manifest" ]] ||
    fail "generation manifest is missing under ${root}"
  grep -qx "INPUT_SHA256=${INPUT_SHA256}" "${root}/generation.manifest" ||
    fail "generated interfaces are stale"

  for directory in "${GENERATED_DIRS[@]}"; do
    [[ -d "${root}/${directory}/rmw_dds_common" ]] ||
      fail "generated directory is missing: ${directory}/rmw_dds_common"
  done

  generated_count="$(
    find "${root}" -type f \
      \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) |
      wc -l
  )"
  ((generated_count == 71)) ||
    fail "expected 71 generated C/C++ files, found ${generated_count}"

  invalid_file="$(
    find "${root}" -type f ! \( \
      -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o \
      -name 'generation.manifest' \) -print -quit
  )"
  [[ -z "${invalid_file}" ]] ||
    fail "non-source host artifact escaped generation: ${invalid_file}"

  if grep -R -l -E '/opt/ros|\\.so([.0-9]*)?$' \
      "${root}"/rosidl_* >/dev/null; then
    fail "host ROS path or shared-library reference found in generated source"
  fi

  printf 'Generated rmw_dds_common interfaces: PASS (%s files)\n' \
    "${generated_count}"
  printf 'Input SHA256: %s\n' "${INPUT_SHA256}"
}

if ((CHECK_ONLY == 1)); then
  validate_output "${OUTPUT_ROOT}"
  exit 0
fi

if ((FORCE == 0)) && [[ -f "${OUTPUT_ROOT}/generation.manifest" ]] &&
   grep -qx "INPUT_SHA256=${INPUT_SHA256}" \
     "${OUTPUT_ROOT}/generation.manifest"; then
  validate_output "${OUTPUT_ROOT}"
  printf 'Interface generation cache: HIT\n'
  exit 0
fi

mkdir -p -- "${OUTPUT_PARENT}" "${LOG_DIR}"
WORK_ROOT="$(mktemp -d /tmp/velaros-ros2-codegen.XXXXXX)"
STAGE_ROOT="$(mktemp -d "${OUTPUT_PARENT}/.rmw-dds-common.XXXXXX")"
mkdir -p -- "${WORK_ROOT}/src"
ln -s "${PACKAGE_ROOT}" "${WORK_ROOT}/src/rmw_dds_common"

printf 'Generating rmw_dds_common interfaces with ROS 2 %s\n' "${ROS_DISTRO}"
printf 'Host generator setup: %s\n' "${ROS_SETUP}"

set +e
bash --noprofile --norc -c \
  'source "$1" &&
   colcon --log-base "$2/log" build \
     --base-paths "$2/src" \
     --build-base "$2/build" \
     --install-base "$2/install" \
     --packages-select rmw_dds_common \
     --allow-overriding rmw_dds_common \
     --event-handlers console_direct+ \
     --cmake-args -DBUILD_TESTING=OFF' \
  _ "${ROS_SETUP}" "${WORK_ROOT}" 2>&1 | tee "${LOG_FILE}"
GENERATION_STATUS=${PIPESTATUS[0]}
set -e

((GENERATION_STATUS == 0)) ||
  fail "host interface generation failed; see ${LOG_FILE}"

BUILD_ROOT="${WORK_ROOT}/build/rmw_dds_common"
for directory in "${GENERATED_DIRS[@]}"; do
  [[ -d "${BUILD_ROOT}/${directory}/rmw_dds_common" ]] ||
    fail "generator did not produce ${directory}/rmw_dds_common"
  mkdir -p -- "${STAGE_ROOT}/${directory}"
  cp -a -- \
    "${BUILD_ROOT}/${directory}/rmw_dds_common" \
    "${STAGE_ROOT}/${directory}/"
done

{
  printf 'FORMAT=1\n'
  printf 'ROS_DISTRO=%s\n' "${ROS_DISTRO}"
  printf 'RMW_DDS_COMMON_REV=%s\n' "${RMW_DDS_COMMON_REV}"
  printf 'INPUT_SHA256=%s\n' "${INPUT_SHA256}"
  printf 'SOURCE_ORIGIN=host-generated-source-only\n'
} >"${STAGE_ROOT}/generation.manifest"

validate_output "${STAGE_ROOT}"

BACKUP_ROOT="${OUTPUT_ROOT}.old.$$"
if [[ -e "${OUTPUT_ROOT}" ]]; then
  mv -- "${OUTPUT_ROOT}" "${BACKUP_ROOT}"
fi
if ! mv -- "${STAGE_ROOT}" "${OUTPUT_ROOT}"; then
  if [[ -e "${BACKUP_ROOT}" ]]; then
    mv -- "${BACKUP_ROOT}" "${OUTPUT_ROOT}"
  fi
  fail "could not publish generated interfaces"
fi
STAGE_ROOT=""
if [[ -e "${BACKUP_ROOT}" ]]; then
  rm -rf -- "${BACKUP_ROOT}"
fi

printf 'Published: %s\n' "${OUTPUT_ROOT}"
printf 'Log:       %s\n' "${LOG_FILE}"
