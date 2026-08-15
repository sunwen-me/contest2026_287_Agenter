#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Generate the static ROS 2 Lyrical Action wire closure for openvela.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
LOCK_FILE="${SCRIPT_DIR}/velaros-ros2-sources.lock"
SOURCE_ROOT="${WORKSPACE_ROOT}/external/velaros_ros2_sources"
OUTPUT_PARENT="${WORKSPACE_ROOT}/external/velaros_ros2_generated"
OUTPUT_ROOT="${OUTPUT_PARENT}/action"
LOG_DIR="${WORKSPACE_ROOT}/cmake_out"
LOG_FILE="${LOG_DIR}/velaros-action-generation.log"
ROS_SETUP="${VELAROS_HOST_ROS_SETUP:-/opt/ros/lyrical/setup.bash}"
PRUNER="${SCRIPT_DIR}/prune_velaros_action.py"
VELAROS_INTERFACES_ROOT="${CONTEST_ROOT}/interfaces/velaros_interfaces"
FORCE=0
CHECK_ONLY=0
WORK_ROOT=""
STAGE_ROOT=""

usage()
{
  cat <<'EOF'
Usage: generate_velaros_action.sh [options]

Options:
  --check       Verify the generated bounded Action source set
  --force       Regenerate even when the input fingerprint is unchanged
  -h, --help    Show this help

Only builtin_interfaces/Time, unique_identifier_msgs/UUID, action_msgs core,
example_interfaces/Fibonacci, and the fixed-layout
velaros_interfaces/MoveRelative product Action enter openvela. Service events,
runtime type descriptions, host libraries, Python modules, and arbitrary type
loading do not.
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

((CHECK_ONLY == 0 || FORCE == 0)) || fail "--check and --force cannot be combined"
[[ -f "${LOCK_FILE}" ]] || fail "source lock is missing"
[[ -f "${ROS_SETUP}" ]] || fail "host ROS setup is missing: ${ROS_SETUP}"
[[ -x "${PRUNER}" ]] || fail "Action pruner is missing or not executable: ${PRUNER}"
[[ -f "${VELAROS_INTERFACES_ROOT}/action/MoveRelative.action" ]] ||
  fail "VelaROS product Action source is missing"

# shellcheck disable=SC1090
source "${LOCK_FILE}"

declare -A PACKAGE_SOURCE=(
  [builtin_interfaces]="${SOURCE_ROOT}/rcl_interfaces/builtin_interfaces"
  [unique_identifier_msgs]="${SOURCE_ROOT}/unique_identifier_msgs"
  [action_msgs]="${SOURCE_ROOT}/rcl_interfaces/action_msgs"
  [example_interfaces]="${SOURCE_ROOT}/example_interfaces"
)
for package in "${!PACKAGE_SOURCE[@]}"; do
  [[ -f "${PACKAGE_SOURCE[${package}]}/package.xml" ]] ||
    fail "${package} source is missing; run restore_velaros_ros2_sources.sh"
done

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

check_deb_version ros-lyrical-builtin-interfaces "${BUILTIN_INTERFACES_DEB_VERSION}"
check_deb_version ros-lyrical-unique-identifier-msgs "${UNIQUE_IDENTIFIER_MSGS_DEB_VERSION}"
check_deb_version ros-lyrical-action-msgs "${ACTION_MSGS_DEB_VERSION}"
check_deb_version ros-lyrical-example-interfaces "${EXAMPLE_INTERFACES_DEB_VERSION}"
check_deb_version ros-lyrical-service-msgs "${SERVICE_MSGS_DEB_VERSION}"
check_deb_version ros-lyrical-rosidl-generator-c "${ROSIDL_GENERATOR_C_DEB_VERSION}"
check_deb_version ros-lyrical-rosidl-typesupport-fastrtps-c \
  "${ROSIDL_TYPESUPPORT_FASTRTPS_C_DEB_VERSION}"

INPUT_SHA256="$({
  sha256sum \
    "${PACKAGE_SOURCE[builtin_interfaces]}/msg/Time.msg" \
    "${PACKAGE_SOURCE[unique_identifier_msgs]}/msg/UUID.msg" \
    "${PACKAGE_SOURCE[action_msgs]}/msg/GoalInfo.msg" \
    "${PACKAGE_SOURCE[action_msgs]}/msg/GoalStatus.msg" \
    "${PACKAGE_SOURCE[action_msgs]}/msg/GoalStatusArray.msg" \
    "${PACKAGE_SOURCE[action_msgs]}/srv/CancelGoal.srv" \
    "${PACKAGE_SOURCE[example_interfaces]}/action/Fibonacci.action" \
    "${VELAROS_INTERFACES_ROOT}/action/MoveRelative.action" \
    "${VELAROS_INTERFACES_ROOT}/CMakeLists.txt" \
    "${VELAROS_INTERFACES_ROOT}/package.xml" \
    "${PRUNER}"
  printf '%s\n' \
    "${RCL_INTERFACES_REV}" "${UNIQUE_IDENTIFIER_MSGS_REV}" \
    "${EXAMPLE_INTERFACES_REV}" "${ROSIDL_GENERATOR_C_DEB_VERSION}" \
    "${ROSIDL_TYPESUPPORT_FASTRTPS_C_DEB_VERSION}"
} | sha256sum | cut -d' ' -f1)"

validate_output()
{
  local root="$1"
  local count
  [[ -f "${root}/generation.manifest" ]] || fail "Action generation manifest is missing"
  grep -qx "INPUT_SHA256=${INPUT_SHA256}" "${root}/generation.manifest" ||
    fail "generated Action sources are stale"
  grep -qx 'ACTION_TYPES=example_interfaces/Fibonacci,velaros_interfaces/MoveRelative' \
    "${root}/generation.manifest" ||
    fail "Action type whitelist is missing"
  grep -qx 'SERVICE_EVENT_INTROSPECTION=pruned' "${root}/generation.manifest" ||
    fail "Action introspection policy is missing"
  [[ -f "${root}/velaros_action_hashes.c" ]] || fail "Action TypeHash source is missing"
  [[ -f "${root}/rosidl_generator_c/example_interfaces/action/fibonacci.h" ]] ||
    fail "Fibonacci generator C source is missing"
  [[ -f "${root}/rosidl_generator_c/velaros_interfaces/action/move_relative.h" ]] ||
    fail "MoveRelative generator C source is missing"
  [[ -f "${root}/rosidl_typesupport_fastrtps_c/action_msgs/srv/detail/cancel_goal__type_support_c.cpp" ]] ||
    fail "CancelGoal Fast RTPS source is missing"
  if rg -l 'ServiceEventInfo|_Event|get_type_description\(' "${root}" \
      --glob '*.{c,cpp,h,hpp}' >/dev/null; then
    fail "Action introspection or type-description code survived pruning"
  fi
  count="$(find "${root}" -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) | wc -l)"
  printf 'Generated bounded ROS 2 Action whitelist: PASS (%s files)\n' "${count}"
  printf 'Input SHA256: %s\n' "${INPUT_SHA256}"
}

if ((CHECK_ONLY == 1)); then
  validate_output "${OUTPUT_ROOT}"
  exit 0
fi

if ((FORCE == 0)) && [[ -f "${OUTPUT_ROOT}/generation.manifest" ]] &&
   grep -qx "INPUT_SHA256=${INPUT_SHA256}" "${OUTPUT_ROOT}/generation.manifest"; then
  validate_output "${OUTPUT_ROOT}"
  printf 'Action generation cache: HIT\n'
  exit 0
fi

mkdir -p -- "${OUTPUT_PARENT}" "${LOG_DIR}"
WORK_ROOT="$(mktemp -d /tmp/velaros-action-codegen.XXXXXX)"
STAGE_ROOT="$(mktemp -d "${OUTPUT_PARENT}/.action.XXXXXX")"
mkdir -p -- "${WORK_ROOT}/src"
for package in builtin_interfaces unique_identifier_msgs action_msgs example_interfaces; do
  ln -s "${PACKAGE_SOURCE[${package}]}" "${WORK_ROOT}/src/${package}"
done
ln -s "${VELAROS_INTERFACES_ROOT}" "${WORK_ROOT}/src/velaros_interfaces"

set +e
bash --noprofile --norc -c \
  'source "$1" && colcon --log-base "$2/log" build \
    --base-paths "$2/src" --build-base "$2/build" --install-base "$2/install" \
    --packages-select builtin_interfaces unique_identifier_msgs action_msgs example_interfaces velaros_interfaces \
    --allow-overriding builtin_interfaces unique_identifier_msgs action_msgs example_interfaces velaros_interfaces \
    --event-handlers console_direct+ --cmake-args -DBUILD_TESTING=OFF' \
  _ "${ROS_SETUP}" "${WORK_ROOT}" 2>&1 | tee "${LOG_FILE}"
GENERATION_STATUS=${PIPESTATUS[0]}
set -e
((GENERATION_STATUS == 0)) || fail "host Action generation failed; see ${LOG_FILE}"

copy_generated_type()
{
  local package="$1"
  local kind="$2"
  local snake_name="$3"
  local build_root="${WORK_ROOT}/build/${package}"
  local relative
  local files=(
    "rosidl_generator_c/${package}/${kind}/${snake_name}.h"
    "rosidl_generator_c/${package}/${kind}/detail/${snake_name}__functions.c"
    "rosidl_generator_c/${package}/${kind}/detail/${snake_name}__functions.h"
    "rosidl_generator_c/${package}/${kind}/detail/${snake_name}__struct.h"
    "rosidl_generator_c/${package}/${kind}/detail/${snake_name}__type_support.h"
    "rosidl_typesupport_fastrtps_c/${package}/${kind}/detail/${snake_name}__rosidl_typesupport_fastrtps_c.h"
    "rosidl_typesupport_fastrtps_c/${package}/${kind}/detail/${snake_name}__type_support_c.cpp"
  )
  for relative in "${files[@]}"; do
    mkdir -p -- "${STAGE_ROOT}/$(dirname -- "${relative}")"
    cp -a -- "${build_root}/${relative}" "${STAGE_ROOT}/${relative}"
  done
  for relative in \
    "rosidl_generator_c/${package}/msg/rosidl_generator_c__visibility_control.h" \
    "rosidl_typesupport_fastrtps_c/${package}/msg/rosidl_typesupport_fastrtps_c__visibility_control.h"; do
    mkdir -p -- "${STAGE_ROOT}/$(dirname -- "${relative}")"
    if [[ ! -f "${STAGE_ROOT}/${relative}" ]]; then
      cp -a -- "${build_root}/${relative}" "${STAGE_ROOT}/${relative}"
    fi
  done
}

copy_generated_type builtin_interfaces msg time
copy_generated_type unique_identifier_msgs msg uuid
copy_generated_type action_msgs msg goal_info
copy_generated_type action_msgs msg goal_status
copy_generated_type action_msgs msg goal_status_array
copy_generated_type action_msgs srv cancel_goal
copy_generated_type example_interfaces action fibonacci
copy_generated_type velaros_interfaces action move_relative

"${PRUNER}" "${STAGE_ROOT}" \
  --description "${WORK_ROOT}/build/builtin_interfaces/rosidl_generator_c/builtin_interfaces/msg/detail/time__description.c" \
  --description "${WORK_ROOT}/build/unique_identifier_msgs/rosidl_generator_c/unique_identifier_msgs/msg/detail/uuid__description.c" \
  --description "${WORK_ROOT}/build/action_msgs/rosidl_generator_c/action_msgs/msg/detail/goal_info__description.c" \
  --description "${WORK_ROOT}/build/action_msgs/rosidl_generator_c/action_msgs/msg/detail/goal_status__description.c" \
  --description "${WORK_ROOT}/build/action_msgs/rosidl_generator_c/action_msgs/msg/detail/goal_status_array__description.c" \
  --description "${WORK_ROOT}/build/action_msgs/rosidl_generator_c/action_msgs/srv/detail/cancel_goal__description.c" \
  --description "${WORK_ROOT}/build/example_interfaces/rosidl_generator_c/example_interfaces/action/detail/fibonacci__description.c" \
  --description "${WORK_ROOT}/build/velaros_interfaces/rosidl_generator_c/velaros_interfaces/action/detail/move_relative__description.c"

{
  printf 'FORMAT=1\n'
  printf 'ROS_DISTRO=%s\n' "${ROS_DISTRO}"
  printf 'RCL_VERSION=%s\n' "${RCL_VERSION}"
  printf 'ACTION_TYPES=example_interfaces/Fibonacci,velaros_interfaces/MoveRelative\n'
  printf 'MAX_GOALS_CONFIG=CONFIG_VELAROS_ACTION_MAX_GOALS\n'
  printf 'SEQUENCE_CAPACITY_CONFIG=CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY\n'
  printf 'SERVICE_EVENT_INTROSPECTION=pruned\n'
  printf 'RUNTIME_TYPE_DESCRIPTION=pruned\n'
  printf 'INPUT_SHA256=%s\n' "${INPUT_SHA256}"
} >"${STAGE_ROOT}/generation.manifest"

validate_output "${STAGE_ROOT}"

BACKUP_ROOT="${OUTPUT_ROOT}.old.$$"
if [[ -e "${OUTPUT_ROOT}" ]]; then
  mv -- "${OUTPUT_ROOT}" "${BACKUP_ROOT}"
fi
if ! mv -- "${STAGE_ROOT}" "${OUTPUT_ROOT}"; then
  [[ ! -e "${BACKUP_ROOT}" ]] || mv -- "${BACKUP_ROOT}" "${OUTPUT_ROOT}"
  fail "could not publish generated Action sources"
fi
STAGE_ROOT=""
[[ ! -e "${BACKUP_ROOT}" ]] || rm -rf -- "${BACKUP_ROOT}"

printf 'Published: %s\n' "${OUTPUT_ROOT}"
printf 'Log:       %s\n' "${LOG_FILE}"
