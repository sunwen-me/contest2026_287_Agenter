#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${PROJECT_ROOT}/.." && pwd)}"
INTERFACE_ROOT="${PROJECT_ROOT}/interfaces/velaros_interfaces"
OUTPUT_ROOT="${WORKSPACE_ROOT}/cmake_out/velaros-host-interfaces"
ROS_SETUP="${VELAROS_HOST_ROS_SETUP:-/opt/ros/lyrical/setup.bash}"
INPUT_MANIFEST="${OUTPUT_ROOT}/input.sha256"
CHECK_ONLY=0

if [[ "${1:-}" == "--check" ]]; then
  CHECK_ONLY=1
elif (($# > 0)); then
  printf 'usage: %s [--check]\n' "$0" >&2
  exit 2
fi

[[ -f "${ROS_SETUP}" ]] || { printf 'missing ROS setup: %s\n' "${ROS_SETUP}" >&2; exit 1; }
[[ -f "${INTERFACE_ROOT}/action/MoveRelative.action" ]] || {
  printf 'missing MoveRelative interface source\n' >&2
  exit 1
}

INPUT_SHA256="$({
  sha256sum \
    "${INTERFACE_ROOT}/CMakeLists.txt" \
    "${INTERFACE_ROOT}/package.xml" \
    "${INTERFACE_ROOT}/action/MoveRelative.action"
} | sha256sum | cut -d' ' -f1)"
SETUP_FILE="${OUTPUT_ROOT}/install/setup.bash"

validate()
{
  [[ -f "${SETUP_FILE}" ]] || { printf 'host interface setup is missing\n' >&2; exit 1; }
  grep -qx "${INPUT_SHA256}" "${INPUT_MANIFEST}" || {
    printf 'host interface build is stale\n' >&2
    exit 1
  }
  bash --noprofile --norc -c \
    'source "$1" && source "$2" && python3 -c "from velaros_interfaces.action import MoveRelative; assert MoveRelative.Goal() is not None"' \
    _ "${ROS_SETUP}" "${SETUP_FILE}"
  printf 'VelaROS host MoveRelative interface: PASS\n'
  printf 'Setup: %s\n' "${SETUP_FILE}"
}

if ((CHECK_ONLY == 1)); then
  validate
  exit 0
fi

if [[ -f "${SETUP_FILE}" && -f "${INPUT_MANIFEST}" ]] &&
   grep -qx "${INPUT_SHA256}" "${INPUT_MANIFEST}"; then
  validate
  printf 'Host interface build cache: HIT\n'
  exit 0
fi

mkdir -p -- "${OUTPUT_ROOT}"
bash --noprofile --norc -c \
  'source "$1" && colcon --log-base "$2/log" build \
    --base-paths "$3" --build-base "$2/build" --install-base "$2/install" \
    --packages-select velaros_interfaces --event-handlers console_direct+ \
    --cmake-args -DBUILD_TESTING=OFF' \
  _ "${ROS_SETUP}" "${OUTPUT_ROOT}" "${INTERFACE_ROOT}"
printf '%s\n' "${INPUT_SHA256}" >"${INPUT_MANIFEST}"
validate
