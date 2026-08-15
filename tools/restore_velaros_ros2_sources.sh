#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Restore the exact ROS 2 Lyrical source baseline used by VelaROS.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
LOCK_FILE="${SCRIPT_DIR}/velaros-ros2-sources.lock"
RCUTILS_PATCH="${SCRIPT_DIR}/patches/rcutils-7.1.1-openvela.patch"
ROSIDL_PATCH="${SCRIPT_DIR}/patches/rosidl-5.2.1-openvela.patch"
RMW_FASTRTPS_PATCH="${SCRIPT_DIR}/patches/rmw-fastrtps-9.4.8-openvela.patch"
RMW_FASTRTPS_BUFFER_PATCH="${SCRIPT_DIR}/patches/rmw-fastrtps-9.4.8-velaros-static-buffer.patch"
RCL_PATCH="${SCRIPT_DIR}/patches/rcl-10.4.4-openvela.patch"
SOURCE_ROOT="${WORKSPACE_ROOT}/external/velaros_ros2_sources"
BACKUP_ROOT="${VELAROS_BACKUP_ROOT:-/tmp}"
REPLACE=0
CHECK_ONLY=0
TEMP_ROOT=""
BACKUP_DIR=""

usage()
{
  cat <<'EOF'
Usage: restore_velaros_ros2_sources.sh [options]

Options:
  --check       Verify all source revision markers and package versions
  --replace     Recoverably replace an existing mismatched source tree
  -h, --help    Show this help

Environment:
  OPENVELA_ROOT         openvela workspace root
  VELAROS_BACKUP_ROOT   replacement backup parent (default: /tmp)

The script exports exact Git commits without .git directories.  Target-side
builds use these sources and do not link libraries from /opt/ros/lyrical.
EOF
}

fail()
{
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

cleanup()
{
  if [[ -n "${TEMP_ROOT}" && -d "${TEMP_ROOT}" ]]; then
    rm -rf -- "${TEMP_ROOT}"
  fi
}

while (($# > 0)); do
  case "$1" in
    --check)
      CHECK_ONLY=1
      shift
      ;;
    --replace)
      REPLACE=1
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

[[ -f "${LOCK_FILE}" ]] || fail "source lock is missing: ${LOCK_FILE}"
[[ -f "${RCUTILS_PATCH}" ]] || fail "rcutils patch is missing: ${RCUTILS_PATCH}"
[[ -f "${ROSIDL_PATCH}" ]] || fail "rosidl patch is missing: ${ROSIDL_PATCH}"
[[ -f "${RMW_FASTRTPS_PATCH}" ]] ||
  fail "rmw_fastrtps patch is missing: ${RMW_FASTRTPS_PATCH}"
[[ -f "${RMW_FASTRTPS_BUFFER_PATCH}" ]] ||
  fail "rmw_fastrtps buffer patch is missing: ${RMW_FASTRTPS_BUFFER_PATCH}"
[[ -f "${RCL_PATCH}" ]] || fail "rcl patch is missing: ${RCL_PATCH}"
[[ -d "${WORKSPACE_ROOT}/external" ]] ||
  fail "external repository is missing under ${WORKSPACE_ROOT}"

# shellcheck disable=SC1090
source "${LOCK_FILE}"

declare -a REQUIRED_LOCK_VARS=(
  RCUTILS_URL RCUTILS_VERSION RCUTILS_REV
  ROSIDL_URL ROSIDL_VERSION ROSIDL_REV
  ROSIDL_TYPESUPPORT_URL ROSIDL_TYPESUPPORT_VERSION ROSIDL_TYPESUPPORT_REV
  ROSIDL_DYNAMIC_TYPESUPPORT_URL ROSIDL_DYNAMIC_TYPESUPPORT_VERSION
  ROSIDL_DYNAMIC_TYPESUPPORT_REV
  RMW_URL RMW_VERSION RMW_REV
  RCPPUTILS_URL RCPPUTILS_VERSION RCPPUTILS_REV
  RMW_FASTRTPS_URL RMW_FASTRTPS_VERSION RMW_FASTRTPS_REV
  RMW_DDS_COMMON_URL RMW_DDS_COMMON_VERSION RMW_DDS_COMMON_REV
  ROSIDL_TYPESUPPORT_FASTRTPS_URL ROSIDL_TYPESUPPORT_FASTRTPS_VERSION
  ROSIDL_TYPESUPPORT_FASTRTPS_REV
  ROSIDL_DYNAMIC_TYPESUPPORT_FASTRTPS_URL
  ROSIDL_DYNAMIC_TYPESUPPORT_FASTRTPS_VERSION
  ROSIDL_DYNAMIC_TYPESUPPORT_FASTRTPS_REV
  RCL_URL RCL_VERSION RCL_REV
  RCLCPP_URL RCLCPP_VERSION RCLCPP_REV
  RCL_INTERFACES_URL RCL_INTERFACES_VERSION RCL_INTERFACES_REV
  RCL_LOGGING_URL RCL_LOGGING_VERSION RCL_LOGGING_REV
  LIBYAML_VENDOR_URL LIBYAML_VENDOR_VERSION LIBYAML_VENDOR_REV
  ROS2_TRACING_URL ROS2_TRACING_VERSION ROS2_TRACING_REV
  UNIQUE_IDENTIFIER_MSGS_URL UNIQUE_IDENTIFIER_MSGS_VERSION
  UNIQUE_IDENTIFIER_MSGS_REV
  COMMON_INTERFACES_URL COMMON_INTERFACES_VERSION COMMON_INTERFACES_REV
  EXAMPLE_INTERFACES_URL EXAMPLE_INTERFACES_VERSION EXAMPLE_INTERFACES_REV
)

for lock_var in "${REQUIRED_LOCK_VARS[@]}"; do
  [[ -n "${!lock_var:-}" ]] || fail "lock variable ${lock_var} is empty"
done

marker_matches()
{
  local destination="$1"
  local revision="$2"
  local marker="${destination}/.velaros-source-revision"

  [[ -f "${marker}" ]] && [[ "$(<"${marker}")" == "${revision}" ]]
}

package_version()
{
  local package_file="$1"

  sed -n 's:.*<version>\([^<]*\)</version>.*:\1:p' "${package_file}" |
    head -n 1
}

verify_tree()
{
  local name="$1"
  local destination="$2"
  local revision="$3"
  local package_path="$4"
  local expected_version="$5"
  local actual_version

  [[ -d "${destination}" ]] || fail "${name} source is missing: ${destination}"
  marker_matches "${destination}" "${revision}" ||
    fail "${name} revision marker is missing or mismatched; run with --replace"
  [[ -f "${destination}/${package_path}" ]] ||
    fail "${name} package.xml is missing: ${package_path}"
  actual_version="$(package_version "${destination}/${package_path}")"
  [[ "${actual_version}" == "${expected_version}" ]] ||
    fail "${name} package is ${actual_version:-unknown}, expected ${expected_version}"
  printf 'verified %-28s %s (%s)\n' "${name}" "${revision}" "${actual_version}"
}

backup_existing()
{
  local destination="$1"

  [[ -e "${destination}" ]] || return 0
  ((REPLACE == 1)) ||
    fail "${destination} already exists without the expected marker; use --replace"

  if [[ -z "${BACKUP_DIR}" ]]; then
    BACKUP_DIR="${BACKUP_ROOT%/}/velaros-ros2-source-backup-$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir -p -- "${BACKUP_DIR}"
  fi

  mv -- "${destination}" "${BACKUP_DIR}/$(basename -- "${destination}")"
}

export_revision()
{
  local name="$1"
  local url="$2"
  local revision="$3"
  local destination="$4"
  local clone_dir="${TEMP_ROOT}/${name}.git"
  local export_dir="${TEMP_ROOT}/${name}.export"

  if marker_matches "${destination}" "${revision}"; then
    printf 'present  %-28s %s\n' "${name}" "${revision}"
    return
  fi

  backup_existing "${destination}"
  git clone --quiet --no-checkout "${url}" "${clone_dir}"
  git -C "${clone_dir}" checkout --quiet --detach "${revision}"
  [[ "$(git -C "${clone_dir}" rev-parse HEAD)" == "${revision}" ]] ||
    fail "${name} checkout did not resolve to ${revision}"

  mkdir -p -- "${export_dir}" "$(dirname -- "${destination}")"
  git -C "${clone_dir}" archive --format=tar HEAD |
    tar -xf - -C "${export_dir}"
  mv -- "${export_dir}" "${destination}"
  printf '%s\n' "${revision}" >"${destination}/.velaros-source-revision"
  printf 'restored %-28s %s\n' "${name}" "${revision}"
}

source_patch_is_applied()
{
  local source_dir="$1"
  local patch_file="$2"

  patch --batch --silent --dry-run --reverse -d "${source_dir}" -p1 \
    <"${patch_file}" >/dev/null 2>&1
}

apply_source_patch()
{
  local name="$1"
  local source_dir="$2"
  local patch_file="$3"

  if patch --batch --silent --dry-run \
    --forward -d "${source_dir}" -p1 <"${patch_file}" >/dev/null 2>&1; then
    patch --batch --forward -d "${source_dir}" -p1 <"${patch_file}"
    printf 'applied openvela %s patch\n' "${name}"
  elif source_patch_is_applied "${source_dir}" "${patch_file}"; then
    printf 'present openvela %s patch\n' "${name}"
  else
    fail "${name} patch is neither applicable nor already applied"
  fi
}

verify_all()
{
  verify_tree rcutils "${SOURCE_ROOT}/rcutils" "${RCUTILS_REV}" \
    package.xml "${RCUTILS_VERSION}"
  verify_tree rosidl "${SOURCE_ROOT}/rosidl" "${ROSIDL_REV}" \
    rosidl_runtime_c/package.xml "${ROSIDL_VERSION}"
  verify_tree rosidl_typesupport "${SOURCE_ROOT}/rosidl_typesupport" \
    "${ROSIDL_TYPESUPPORT_REV}" rosidl_typesupport_cpp/package.xml \
    "${ROSIDL_TYPESUPPORT_VERSION}"
  verify_tree rosidl_dynamic_typesupport \
    "${SOURCE_ROOT}/rosidl_dynamic_typesupport" \
    "${ROSIDL_DYNAMIC_TYPESUPPORT_REV}" package.xml \
    "${ROSIDL_DYNAMIC_TYPESUPPORT_VERSION}"
  verify_tree rmw "${SOURCE_ROOT}/rmw" "${RMW_REV}" \
    rmw/package.xml "${RMW_VERSION}"
  verify_tree rcpputils "${SOURCE_ROOT}/rcpputils" "${RCPPUTILS_REV}" \
    package.xml "${RCPPUTILS_VERSION}"
  verify_tree rmw_fastrtps "${SOURCE_ROOT}/rmw_fastrtps" \
    "${RMW_FASTRTPS_REV}" rmw_fastrtps_cpp/package.xml \
    "${RMW_FASTRTPS_VERSION}"
  verify_tree rmw_dds_common "${SOURCE_ROOT}/rmw_dds_common" \
    "${RMW_DDS_COMMON_REV}" rmw_dds_common/package.xml \
    "${RMW_DDS_COMMON_VERSION}"
  verify_tree rosidl_typesupport_fastrtps \
    "${SOURCE_ROOT}/rosidl_typesupport_fastrtps" \
    "${ROSIDL_TYPESUPPORT_FASTRTPS_REV}" \
    rosidl_typesupport_fastrtps_cpp/package.xml \
    "${ROSIDL_TYPESUPPORT_FASTRTPS_VERSION}"
  verify_tree rosidl_dynamic_typesupport_fastrtps \
    "${SOURCE_ROOT}/rosidl_dynamic_typesupport_fastrtps" \
    "${ROSIDL_DYNAMIC_TYPESUPPORT_FASTRTPS_REV}" package.xml \
    "${ROSIDL_DYNAMIC_TYPESUPPORT_FASTRTPS_VERSION}"
  verify_tree rcl "${SOURCE_ROOT}/rcl" "${RCL_REV}" \
    rcl/package.xml "${RCL_VERSION}"
  verify_tree rclcpp "${SOURCE_ROOT}/rclcpp" "${RCLCPP_REV}" \
    rclcpp/package.xml "${RCLCPP_VERSION}"
  verify_tree rcl_interfaces "${SOURCE_ROOT}/rcl_interfaces" \
    "${RCL_INTERFACES_REV}" rcl_interfaces/package.xml \
    "${RCL_INTERFACES_VERSION}"
  verify_tree rcl_logging "${SOURCE_ROOT}/rcl_logging" \
    "${RCL_LOGGING_REV}" rcl_logging_interface/package.xml \
    "${RCL_LOGGING_VERSION}"
  verify_tree libyaml_vendor "${SOURCE_ROOT}/libyaml_vendor" \
    "${LIBYAML_VENDOR_REV}" package.xml "${LIBYAML_VENDOR_VERSION}"
  verify_tree ros2_tracing "${SOURCE_ROOT}/ros2_tracing" \
    "${ROS2_TRACING_REV}" tracetools/package.xml \
    "${ROS2_TRACING_VERSION}"
  verify_tree unique_identifier_msgs \
    "${SOURCE_ROOT}/unique_identifier_msgs" \
    "${UNIQUE_IDENTIFIER_MSGS_REV}" package.xml \
    "${UNIQUE_IDENTIFIER_MSGS_VERSION}"
  verify_tree common_interfaces "${SOURCE_ROOT}/common_interfaces" \
    "${COMMON_INTERFACES_REV}" std_msgs/package.xml \
    "${COMMON_INTERFACES_VERSION}"
  verify_tree example_interfaces "${SOURCE_ROOT}/example_interfaces" \
    "${EXAMPLE_INTERFACES_REV}" package.xml \
    "${EXAMPLE_INTERFACES_VERSION}"
}

if ((CHECK_ONLY == 1)); then
  verify_all
  source_patch_is_applied "${SOURCE_ROOT}/rcutils" "${RCUTILS_PATCH}" ||
    fail "openvela rcutils patch is not applied"
  printf 'verified openvela rcutils patch\n'
  source_patch_is_applied "${SOURCE_ROOT}/rosidl" "${ROSIDL_PATCH}" ||
    fail "openvela rosidl patch is not applied"
  printf 'verified openvela rosidl patch\n'
  source_patch_is_applied "${SOURCE_ROOT}/rmw_fastrtps" \
    "${RMW_FASTRTPS_PATCH}" ||
    fail "openvela rmw_fastrtps patch is not applied"
  printf 'verified openvela rmw_fastrtps patch\n'
  source_patch_is_applied "${SOURCE_ROOT}/rmw_fastrtps" \
    "${RMW_FASTRTPS_BUFFER_PATCH}" ||
    fail "VelaROS static buffer backend patch is not applied"
  printf 'verified VelaROS static buffer backend patch\n'
  source_patch_is_applied "${SOURCE_ROOT}/rcl" "${RCL_PATCH}" ||
    fail "openvela rcl patch is not applied"
  printf 'verified openvela rcl patch\n'
  exit 0
fi

TEMP_ROOT="$(mktemp -d /tmp/velaros-ros2-restore.XXXXXX)"
trap cleanup EXIT INT TERM

export_revision rcutils "${RCUTILS_URL}" "${RCUTILS_REV}" \
  "${SOURCE_ROOT}/rcutils"
export_revision rosidl "${ROSIDL_URL}" "${ROSIDL_REV}" \
  "${SOURCE_ROOT}/rosidl"
export_revision rosidl_typesupport "${ROSIDL_TYPESUPPORT_URL}" \
  "${ROSIDL_TYPESUPPORT_REV}" "${SOURCE_ROOT}/rosidl_typesupport"
export_revision rosidl_dynamic_typesupport "${ROSIDL_DYNAMIC_TYPESUPPORT_URL}" \
  "${ROSIDL_DYNAMIC_TYPESUPPORT_REV}" "${SOURCE_ROOT}/rosidl_dynamic_typesupport"
export_revision rmw "${RMW_URL}" "${RMW_REV}" "${SOURCE_ROOT}/rmw"
export_revision rcpputils "${RCPPUTILS_URL}" "${RCPPUTILS_REV}" \
  "${SOURCE_ROOT}/rcpputils"
export_revision rmw_fastrtps "${RMW_FASTRTPS_URL}" \
  "${RMW_FASTRTPS_REV}" "${SOURCE_ROOT}/rmw_fastrtps"
export_revision rmw_dds_common "${RMW_DDS_COMMON_URL}" \
  "${RMW_DDS_COMMON_REV}" "${SOURCE_ROOT}/rmw_dds_common"
export_revision rosidl_typesupport_fastrtps \
  "${ROSIDL_TYPESUPPORT_FASTRTPS_URL}" \
  "${ROSIDL_TYPESUPPORT_FASTRTPS_REV}" \
  "${SOURCE_ROOT}/rosidl_typesupport_fastrtps"
export_revision rosidl_dynamic_typesupport_fastrtps \
  "${ROSIDL_DYNAMIC_TYPESUPPORT_FASTRTPS_URL}" \
  "${ROSIDL_DYNAMIC_TYPESUPPORT_FASTRTPS_REV}" \
  "${SOURCE_ROOT}/rosidl_dynamic_typesupport_fastrtps"
export_revision rcl "${RCL_URL}" "${RCL_REV}" "${SOURCE_ROOT}/rcl"
export_revision rclcpp "${RCLCPP_URL}" "${RCLCPP_REV}" \
  "${SOURCE_ROOT}/rclcpp"
export_revision rcl_interfaces "${RCL_INTERFACES_URL}" \
  "${RCL_INTERFACES_REV}" "${SOURCE_ROOT}/rcl_interfaces"
export_revision rcl_logging "${RCL_LOGGING_URL}" \
  "${RCL_LOGGING_REV}" "${SOURCE_ROOT}/rcl_logging"
export_revision libyaml_vendor "${LIBYAML_VENDOR_URL}" \
  "${LIBYAML_VENDOR_REV}" "${SOURCE_ROOT}/libyaml_vendor"
export_revision ros2_tracing "${ROS2_TRACING_URL}" \
  "${ROS2_TRACING_REV}" "${SOURCE_ROOT}/ros2_tracing"
export_revision unique_identifier_msgs "${UNIQUE_IDENTIFIER_MSGS_URL}" \
  "${UNIQUE_IDENTIFIER_MSGS_REV}" \
  "${SOURCE_ROOT}/unique_identifier_msgs"
export_revision common_interfaces "${COMMON_INTERFACES_URL}" \
  "${COMMON_INTERFACES_REV}" "${SOURCE_ROOT}/common_interfaces"
export_revision example_interfaces "${EXAMPLE_INTERFACES_URL}" \
  "${EXAMPLE_INTERFACES_REV}" "${SOURCE_ROOT}/example_interfaces"

apply_source_patch rcutils "${SOURCE_ROOT}/rcutils" "${RCUTILS_PATCH}"
apply_source_patch rosidl "${SOURCE_ROOT}/rosidl" "${ROSIDL_PATCH}"
apply_source_patch rmw_fastrtps "${SOURCE_ROOT}/rmw_fastrtps" \
  "${RMW_FASTRTPS_PATCH}"
apply_source_patch rmw_fastrtps_static_buffer \
  "${SOURCE_ROOT}/rmw_fastrtps" "${RMW_FASTRTPS_BUFFER_PATCH}"
apply_source_patch rcl "${SOURCE_ROOT}/rcl" "${RCL_PATCH}"
verify_all

printf '\nVelaROS ROS 2 Lyrical sources restored.\n'
printf 'Source root: %s\n' "${SOURCE_ROOT}"
if [[ -n "${BACKUP_DIR}" ]]; then
  printf 'Previous sources: %s\n' "${BACKUP_DIR}"
fi
