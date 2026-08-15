#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Generate the target-side selected standard Topic C/C++ and Fast RTPS sources.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
LOCK_FILE="${SCRIPT_DIR}/velaros-ros2-sources.lock"
SOURCE_ROOT="${WORKSPACE_ROOT}/external/velaros_ros2_sources"
STD_PACKAGE_ROOT="${SOURCE_ROOT}/common_interfaces/std_msgs"
GEOMETRY_PACKAGE_ROOT="${SOURCE_ROOT}/common_interfaces/geometry_msgs"
OUTPUT_PARENT="${WORKSPACE_ROOT}/external/velaros_ros2_generated"
OUTPUT_ROOT="${OUTPUT_PARENT}/std_msgs"
LOG_DIR="${WORKSPACE_ROOT}/cmake_out"
LOG_FILE="${LOG_DIR}/velaros-std-msgs-generation.log"
ROS_SETUP="${VELAROS_HOST_ROS_SETUP:-/opt/ros/lyrical/setup.bash}"
FORCE=0
CHECK_ONLY=0
WORK_ROOT=""
STAGE_ROOT=""

usage()
{
  cat <<'EOF'
Usage: generate_velaros_std_msgs.sh [options]

Options:
  --check       Verify the generated standard Topic source set
  --force       Regenerate even when the input fingerprint is unchanged
  -h, --help    Show this help

Only source and header files for std_msgs/String, std_msgs/Float64,
geometry_msgs/Vector3, and geometry_msgs/Twist are exported.  C types support
the minimal rcl API; C++ types support the VelaROS static rclcpp RAII profile.
Host ROS objects, archives, shared libraries, and Python modules never enter
openvela.
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
[[ -f "${STD_PACKAGE_ROOT}/msg/String.msg" ]] ||
  fail "std_msgs source is missing; run restore_velaros_ros2_sources.sh"
[[ -f "${STD_PACKAGE_ROOT}/msg/Float64.msg" ]] ||
  fail "std_msgs/Float64 source is missing; run restore_velaros_ros2_sources.sh"
[[ -f "${GEOMETRY_PACKAGE_ROOT}/msg/Vector3.msg" ]] ||
  fail "geometry_msgs/Vector3 source is missing; run restore_velaros_ros2_sources.sh"
[[ -f "${GEOMETRY_PACKAGE_ROOT}/msg/Twist.msg" ]] ||
  fail "geometry_msgs/Twist source is missing; run restore_velaros_ros2_sources.sh"
[[ -f "${SOURCE_ROOT}/common_interfaces/.velaros-source-revision" ]] ||
  fail "common_interfaces source revision marker is missing"

# shellcheck disable=SC1090
source "${LOCK_FILE}"

[[ "$(<"${SOURCE_ROOT}/common_interfaces/.velaros-source-revision")" == \
   "${COMMON_INTERFACES_REV}" ]] ||
  fail "common_interfaces source does not match the lock"

check_deb_version()
{
  local package_name="$1"
  local expected_version="$2"
  local actual_version

  actual_version="$(dpkg-query -W -f='${Version}' "${package_name}" 2>/dev/null)" ||
    fail "required host package is missing: ${package_name}"
  [[ "${actual_version}" == "${expected_version}" ]] ||
    fail "${package_name}=${actual_version}, expected ${expected_version}"
}

check_deb_version ros-lyrical-std-msgs "${STD_MSGS_DEB_VERSION}"
check_deb_version ros-lyrical-geometry-msgs "${GEOMETRY_MSGS_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-generator-c "${ROSIDL_GENERATOR_C_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-generator-cpp "${ROSIDL_GENERATOR_CPP_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-typesupport-fastrtps-c \
  "${ROSIDL_TYPESUPPORT_FASTRTPS_C_DEB_VERSION}"
check_deb_version \
  ros-lyrical-rosidl-typesupport-fastrtps-cpp \
  "${ROSIDL_TYPESUPPORT_FASTRTPS_CPP_DEB_VERSION}"

INPUT_SHA256="$(
  {
    sha256sum \
      "${STD_PACKAGE_ROOT}/msg/String.msg" \
      "${STD_PACKAGE_ROOT}/msg/Float64.msg" \
      "${GEOMETRY_PACKAGE_ROOT}/msg/Vector3.msg" \
      "${GEOMETRY_PACKAGE_ROOT}/msg/Twist.msg"
    printf '%s\n' \
      "${COMMON_INTERFACES_REV}" \
      "${STD_MSGS_DEB_VERSION}" \
      "${GEOMETRY_MSGS_DEB_VERSION}" \
      "${ROSIDL_GENERATOR_C_DEB_VERSION}" \
      "${ROSIDL_GENERATOR_CPP_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_FASTRTPS_C_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_FASTRTPS_CPP_DEB_VERSION}"
  } | sha256sum | cut -d' ' -f1
)"

STD_GENERATED_FILES=(
  rosidl_generator_c/std_msgs/msg/string.h
  rosidl_generator_c/std_msgs/msg/float64.h
  rosidl_generator_c/std_msgs/msg/rosidl_generator_c__visibility_control.h
  rosidl_generator_c/std_msgs/msg/detail/string__description.c
  rosidl_generator_c/std_msgs/msg/detail/string__functions.c
  rosidl_generator_c/std_msgs/msg/detail/string__functions.h
  rosidl_generator_c/std_msgs/msg/detail/string__struct.h
  rosidl_generator_c/std_msgs/msg/detail/string__type_support.c
  rosidl_generator_c/std_msgs/msg/detail/string__type_support.h
  rosidl_generator_c/std_msgs/msg/detail/float64__description.c
  rosidl_generator_c/std_msgs/msg/detail/float64__functions.c
  rosidl_generator_c/std_msgs/msg/detail/float64__functions.h
  rosidl_generator_c/std_msgs/msg/detail/float64__struct.h
  rosidl_generator_c/std_msgs/msg/detail/float64__type_support.c
  rosidl_generator_c/std_msgs/msg/detail/float64__type_support.h
  rosidl_generator_cpp/std_msgs/msg/string.hpp
  rosidl_generator_cpp/std_msgs/msg/float64.hpp
  rosidl_generator_cpp/std_msgs/msg/rosidl_generator_cpp__visibility_control.hpp
  rosidl_generator_cpp/std_msgs/msg/detail/string__builder.hpp
  rosidl_generator_cpp/std_msgs/msg/detail/string__struct.hpp
  rosidl_generator_cpp/std_msgs/msg/detail/string__traits.hpp
  rosidl_generator_cpp/std_msgs/msg/detail/string__type_support.hpp
  rosidl_generator_cpp/std_msgs/msg/detail/float64__builder.hpp
  rosidl_generator_cpp/std_msgs/msg/detail/float64__struct.hpp
  rosidl_generator_cpp/std_msgs/msg/detail/float64__traits.hpp
  rosidl_generator_cpp/std_msgs/msg/detail/float64__type_support.hpp
  rosidl_typesupport_fastrtps_c/std_msgs/msg/rosidl_typesupport_fastrtps_c__visibility_control.h
  rosidl_typesupport_fastrtps_c/std_msgs/msg/detail/string__rosidl_typesupport_fastrtps_c.h
  rosidl_typesupport_fastrtps_c/std_msgs/msg/detail/string__type_support_c.cpp
  rosidl_typesupport_fastrtps_c/std_msgs/msg/detail/float64__rosidl_typesupport_fastrtps_c.h
  rosidl_typesupport_fastrtps_c/std_msgs/msg/detail/float64__type_support_c.cpp
  rosidl_typesupport_fastrtps_cpp/std_msgs/msg/rosidl_typesupport_fastrtps_cpp__visibility_control.h
  rosidl_typesupport_fastrtps_cpp/std_msgs/msg/detail/string__rosidl_typesupport_fastrtps_cpp.hpp
  rosidl_typesupport_fastrtps_cpp/std_msgs/msg/detail/float64__rosidl_typesupport_fastrtps_cpp.hpp
  rosidl_typesupport_fastrtps_cpp/std_msgs/msg/detail/dds_fastrtps/string__type_support.cpp
  rosidl_typesupport_fastrtps_cpp/std_msgs/msg/detail/dds_fastrtps/float64__type_support.cpp
)

GEOMETRY_GENERATED_FILES=(
  rosidl_generator_c/geometry_msgs/msg/vector3.h
  rosidl_generator_c/geometry_msgs/msg/twist.h
  rosidl_generator_c/geometry_msgs/msg/rosidl_generator_c__visibility_control.h
  rosidl_generator_c/geometry_msgs/msg/detail/vector3__description.c
  rosidl_generator_c/geometry_msgs/msg/detail/vector3__functions.c
  rosidl_generator_c/geometry_msgs/msg/detail/vector3__functions.h
  rosidl_generator_c/geometry_msgs/msg/detail/vector3__struct.h
  rosidl_generator_c/geometry_msgs/msg/detail/vector3__type_support.c
  rosidl_generator_c/geometry_msgs/msg/detail/vector3__type_support.h
  rosidl_generator_c/geometry_msgs/msg/detail/twist__description.c
  rosidl_generator_c/geometry_msgs/msg/detail/twist__functions.c
  rosidl_generator_c/geometry_msgs/msg/detail/twist__functions.h
  rosidl_generator_c/geometry_msgs/msg/detail/twist__struct.h
  rosidl_generator_c/geometry_msgs/msg/detail/twist__type_support.c
  rosidl_generator_c/geometry_msgs/msg/detail/twist__type_support.h
  rosidl_generator_cpp/geometry_msgs/msg/vector3.hpp
  rosidl_generator_cpp/geometry_msgs/msg/twist.hpp
  rosidl_generator_cpp/geometry_msgs/msg/rosidl_generator_cpp__visibility_control.hpp
  rosidl_generator_cpp/geometry_msgs/msg/detail/vector3__builder.hpp
  rosidl_generator_cpp/geometry_msgs/msg/detail/vector3__struct.hpp
  rosidl_generator_cpp/geometry_msgs/msg/detail/vector3__traits.hpp
  rosidl_generator_cpp/geometry_msgs/msg/detail/vector3__type_support.hpp
  rosidl_generator_cpp/geometry_msgs/msg/detail/twist__builder.hpp
  rosidl_generator_cpp/geometry_msgs/msg/detail/twist__struct.hpp
  rosidl_generator_cpp/geometry_msgs/msg/detail/twist__traits.hpp
  rosidl_generator_cpp/geometry_msgs/msg/detail/twist__type_support.hpp
  rosidl_typesupport_fastrtps_c/geometry_msgs/msg/rosidl_typesupport_fastrtps_c__visibility_control.h
  rosidl_typesupport_fastrtps_c/geometry_msgs/msg/detail/vector3__rosidl_typesupport_fastrtps_c.h
  rosidl_typesupport_fastrtps_c/geometry_msgs/msg/detail/vector3__type_support_c.cpp
  rosidl_typesupport_fastrtps_c/geometry_msgs/msg/detail/twist__rosidl_typesupport_fastrtps_c.h
  rosidl_typesupport_fastrtps_c/geometry_msgs/msg/detail/twist__type_support_c.cpp
  rosidl_typesupport_fastrtps_cpp/geometry_msgs/msg/rosidl_typesupport_fastrtps_cpp__visibility_control.h
  rosidl_typesupport_fastrtps_cpp/geometry_msgs/msg/detail/vector3__rosidl_typesupport_fastrtps_cpp.hpp
  rosidl_typesupport_fastrtps_cpp/geometry_msgs/msg/detail/twist__rosidl_typesupport_fastrtps_cpp.hpp
  rosidl_typesupport_fastrtps_cpp/geometry_msgs/msg/detail/dds_fastrtps/vector3__type_support.cpp
  rosidl_typesupport_fastrtps_cpp/geometry_msgs/msg/detail/dds_fastrtps/twist__type_support.cpp
)

GENERATED_FILES=("${STD_GENERATED_FILES[@]}" "${GEOMETRY_GENERATED_FILES[@]}")

validate_output()
{
  local root="$1"
  local relative_path
  local generated_count

  [[ -f "${root}/generation.manifest" ]] ||
    fail "standard Topic generation manifest is missing"
  grep -qx "INPUT_SHA256=${INPUT_SHA256}" "${root}/generation.manifest" ||
    fail "generated standard Topic whitelist sources are stale"
  grep -qx 'TOPIC_TYPES=std_msgs/String,std_msgs/Float64,geometry_msgs/Vector3,geometry_msgs/Twist' \
    "${root}/generation.manifest" || fail "standard Topic whitelist is missing"

  for relative_path in "${GENERATED_FILES[@]}"; do
    [[ -f "${root}/${relative_path}" ]] ||
      fail "generated standard Topic file is missing: ${relative_path}"
  done

  generated_count="$(
    find "${root}" -type f \
      \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) |
      wc -l
  )"
  ((${generated_count} == ${#GENERATED_FILES[@]})) ||
    fail "expected ${#GENERATED_FILES[@]} standard Topic files, found ${generated_count}"

  if grep -R -l -E '/opt/ros|\.so([.0-9]*)?$' \
      "${root}"/rosidl_* >/dev/null; then
    fail "host ROS path or shared-library reference found in standard Topic source"
  fi

  printf 'Generated bounded standard Topic interfaces: PASS (%s files)\n' \
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
  printf 'std_msgs generation cache: HIT\n'
  exit 0
fi

mkdir -p -- "${OUTPUT_PARENT}" "${LOG_DIR}"
WORK_ROOT="$(mktemp -d /tmp/velaros-std-msgs-codegen.XXXXXX)"
STAGE_ROOT="$(mktemp -d "${OUTPUT_PARENT}/.std-msgs.XXXXXX")"
mkdir -p -- "${WORK_ROOT}/src"
ln -s "${STD_PACKAGE_ROOT}" "${WORK_ROOT}/src/std_msgs"
ln -s "${GEOMETRY_PACKAGE_ROOT}" "${WORK_ROOT}/src/geometry_msgs"

set +e
bash --noprofile --norc -c \
  'source "$1" &&
   colcon --log-base "$2/log" build \
     --base-paths "$2/src" \
     --build-base "$2/build" \
     --install-base "$2/install" \
     --packages-select std_msgs geometry_msgs \
     --allow-overriding std_msgs geometry_msgs \
     --event-handlers console_direct+ \
     --cmake-args -DBUILD_TESTING=OFF' \
  _ "${ROS_SETUP}" "${WORK_ROOT}" 2>&1 | tee "${LOG_FILE}"
GENERATION_STATUS=${PIPESTATUS[0]}
set -e

((GENERATION_STATUS == 0)) ||
  fail "host std_msgs generation failed; see ${LOG_FILE}"

for relative_path in "${GENERATED_FILES[@]}"; do
  if [[ "${relative_path}" == */geometry_msgs/* ]]; then
    BUILD_ROOT="${WORK_ROOT}/build/geometry_msgs"
  else
    BUILD_ROOT="${WORK_ROOT}/build/std_msgs"
  fi
  mkdir -p -- "${STAGE_ROOT}/$(dirname -- "${relative_path}")"
  cp -a -- "${BUILD_ROOT}/${relative_path}" "${STAGE_ROOT}/${relative_path}"
done

{
  printf 'FORMAT=1\n'
  printf 'ROS_DISTRO=%s\n' "${ROS_DISTRO}"
  printf 'COMMON_INTERFACES_REV=%s\n' "${COMMON_INTERFACES_REV}"
  printf 'TOPIC_TYPES=std_msgs/String,std_msgs/Float64,geometry_msgs/Vector3,geometry_msgs/Twist\n'
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
  fail "could not publish generated std_msgs sources"
fi
STAGE_ROOT=""
if [[ -e "${BACKUP_ROOT}" ]]; then
  rm -rf -- "${BACKUP_ROOT}"
fi

printf 'Published: %s\n' "${OUTPUT_ROOT}"
printf 'Log:       %s\n' "${LOG_FILE}"
