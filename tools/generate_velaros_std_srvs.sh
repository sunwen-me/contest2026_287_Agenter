#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Generate the target-side std_srvs/SetBool C/C++ and Fast RTPS whitelist.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
LOCK_FILE="${SCRIPT_DIR}/velaros-ros2-sources.lock"
SOURCE_ROOT="${WORKSPACE_ROOT}/external/velaros_ros2_sources"
PACKAGE_ROOT="${SOURCE_ROOT}/common_interfaces/std_srvs"
OUTPUT_PARENT="${WORKSPACE_ROOT}/external/velaros_ros2_generated"
OUTPUT_ROOT="${OUTPUT_PARENT}/std_srvs"
LOG_DIR="${WORKSPACE_ROOT}/cmake_out"
LOG_FILE="${LOG_DIR}/velaros-std-srvs-generation.log"
ROS_SETUP="${VELAROS_HOST_ROS_SETUP:-/opt/ros/lyrical/setup.bash}"
PRUNER="${SCRIPT_DIR}/prune_velaros_std_srvs.py"
FORCE=0
CHECK_ONLY=0
WORK_ROOT=""
STAGE_ROOT=""

usage()
{
  cat <<'EOF'
Usage: generate_velaros_std_srvs.sh [options]

Options:
  --check       Verify the generated std_srvs/SetBool source set
  --force       Regenerate even when the input fingerprint is unchanged
  -h, --help    Show this help

Only SetBool request/response source is exported for the C and VelaROS static
rclcpp profiles. Service-event introspection, host ROS objects, shared
libraries, and Python modules never enter openvela.
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
    --check) CHECK_ONLY=1 ;;
    --force) FORCE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown option: $1" ;;
  esac
  shift
done

((CHECK_ONLY == 0 || FORCE == 0)) ||
  fail "--check and --force cannot be used together"
[[ -f "${LOCK_FILE}" ]] || fail "source lock is missing: ${LOCK_FILE}"
[[ -f "${ROS_SETUP}" ]] || fail "host ROS setup is missing: ${ROS_SETUP}"
[[ -x "${PRUNER}" ]] || fail "std_srvs pruner is missing: ${PRUNER}"
[[ -f "${PACKAGE_ROOT}/srv/SetBool.srv" ]] ||
  fail "std_srvs/SetBool source is missing; run restore_velaros_ros2_sources.sh"
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

check_deb_version ros-lyrical-std-srvs "${STD_SRVS_DEB_VERSION}"
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
    sha256sum "${PACKAGE_ROOT}/srv/SetBool.srv" "${PRUNER}"
    printf '%s\n' \
      "${COMMON_INTERFACES_REV}" \
      "${STD_SRVS_DEB_VERSION}" \
      "${ROSIDL_GENERATOR_C_DEB_VERSION}" \
      "${ROSIDL_GENERATOR_CPP_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_FASTRTPS_C_DEB_VERSION}" \
      "${ROSIDL_TYPESUPPORT_FASTRTPS_CPP_DEB_VERSION}"
  } | sha256sum | cut -d' ' -f1
)"

GENERATED_FILES=(
  rosidl_generator_c/std_srvs/msg/rosidl_generator_c__visibility_control.h
  rosidl_generator_c/std_srvs/srv/set_bool.h
  rosidl_generator_c/std_srvs/srv/detail/set_bool__functions.c
  rosidl_generator_c/std_srvs/srv/detail/set_bool__functions.h
  rosidl_generator_c/std_srvs/srv/detail/set_bool__struct.h
  rosidl_generator_c/std_srvs/srv/detail/set_bool__type_support.h
  rosidl_typesupport_fastrtps_c/std_srvs/msg/rosidl_typesupport_fastrtps_c__visibility_control.h
  rosidl_typesupport_fastrtps_c/std_srvs/srv/detail/set_bool__rosidl_typesupport_fastrtps_c.h
  rosidl_typesupport_fastrtps_c/std_srvs/srv/detail/set_bool__type_support_c.cpp
  rosidl_generator_cpp/std_srvs/msg/rosidl_generator_cpp__visibility_control.hpp
  rosidl_generator_cpp/std_srvs/srv/set_bool.hpp
  rosidl_generator_cpp/std_srvs/srv/detail/set_bool__struct.hpp
  rosidl_typesupport_fastrtps_cpp/std_srvs/msg/rosidl_typesupport_fastrtps_cpp__visibility_control.h
  rosidl_typesupport_fastrtps_cpp/std_srvs/srv/detail/set_bool__rosidl_typesupport_fastrtps_cpp.hpp
  rosidl_typesupport_fastrtps_cpp/std_srvs/srv/detail/dds_fastrtps/set_bool__type_support.cpp
)

validate_output()
{
  local root="$1"
  local relative_path
  local generated_count

  [[ -f "${root}/generation.manifest" ]] ||
    fail "std_srvs generation manifest is missing"
  grep -qx "INPUT_SHA256=${INPUT_SHA256}" "${root}/generation.manifest" ||
    fail "generated std_srvs whitelist sources are stale"
  grep -qx 'SERVICE_EVENT_INTROSPECTION=pruned' "${root}/generation.manifest" ||
    fail "std_srvs introspection policy is missing"

  for relative_path in "${GENERATED_FILES[@]}"; do
    [[ -f "${root}/${relative_path}" ]] ||
      fail "generated std_srvs file is missing: ${relative_path}"
  done

  generated_count="$(
    find "${root}" -type f \
      \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) |
      wc -l
  )"
  ((generated_count == ${#GENERATED_FILES[@]})) ||
    fail "expected ${#GENERATED_FILES[@]} std_srvs files, found ${generated_count}"
  if grep -R -l -E '/opt/ros|\.so([.0-9]*)?$|ServiceEventInfo|SetBool_Event' \
      "${root}"/rosidl_* >/dev/null; then
    fail "host dependency or service introspection found in std_srvs source"
  fi

  printf 'Generated std_srvs/SetBool request-response: PASS (%s files)\n' \
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
  printf 'std_srvs generation cache: HIT\n'
  exit 0
fi

mkdir -p -- "${OUTPUT_PARENT}" "${LOG_DIR}"
WORK_ROOT="$(mktemp -d /tmp/velaros-std-srvs-codegen.XXXXXX)"
STAGE_ROOT="$(mktemp -d "${OUTPUT_PARENT}/.std-srvs.XXXXXX")"
mkdir -p -- "${WORK_ROOT}/src"
ln -s "${PACKAGE_ROOT}" "${WORK_ROOT}/src/std_srvs"

set +e
bash --noprofile --norc -c \
  'source "$1" &&
   colcon --log-base "$2/log" build \
     --base-paths "$2/src" \
     --build-base "$2/build" \
     --install-base "$2/install" \
     --packages-select std_srvs \
     --allow-overriding std_srvs \
     --event-handlers console_direct+ \
     --cmake-args -DBUILD_TESTING=OFF' \
  _ "${ROS_SETUP}" "${WORK_ROOT}" 2>&1 | tee "${LOG_FILE}"
GENERATION_STATUS=${PIPESTATUS[0]}
set -e
((GENERATION_STATUS == 0)) ||
  fail "host std_srvs generation failed; see ${LOG_FILE}"

BUILD_ROOT="${WORK_ROOT}/build/std_srvs"
for relative_path in "${GENERATED_FILES[@]}"; do
  mkdir -p -- "${STAGE_ROOT}/$(dirname -- "${relative_path}")"
  cp -a -- "${BUILD_ROOT}/${relative_path}" "${STAGE_ROOT}/${relative_path}"
done
"${PRUNER}" "${STAGE_ROOT}"

{
  printf 'FORMAT=1\n'
  printf 'ROS_DISTRO=%s\n' "${ROS_DISTRO}"
  printf 'COMMON_INTERFACES_REV=%s\n' "${COMMON_INTERFACES_REV}"
  printf 'INPUT_SHA256=%s\n' "${INPUT_SHA256}"
  printf 'SOURCE_ORIGIN=host-generated-source-only\n'
  printf 'SERVICE_EVENT_INTROSPECTION=pruned\n'
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
  fail "could not publish generated std_srvs sources"
fi
STAGE_ROOT=""
if [[ -e "${BACKUP_ROOT}" ]]; then
  rm -rf -- "${BACKUP_ROOT}"
fi

printf 'Published: %s\n' "${OUTPUT_ROOT}"
printf 'Log:       %s\n' "${LOG_FILE}"
