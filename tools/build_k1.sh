#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Reproducible host build entry for MUSE Pi Pro K1.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
CONFIG_PATH="vendor/spacemit/boards/k1/muse_pi_pro/configs/nsh"
BUILD_DIR="${WORKSPACE_ROOT}/cmake_out/muse_pi_pro_nsh"
CCACHE_ROOT="${K1_CCACHE_DIR:-${WORKSPACE_ROOT}/cmake_out/.ccache-k1}"
JOBS="${K1_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '8')}"

CLEAN=0
PACKAGE=0
RUN_CHECK=1
declare -a CREATED_LINKS
declare -a CREATED_DIRS

usage()
{
  cat <<'EOF'
Usage: build_k1.sh [options]

Options:
  --clean       Clean the existing CMake output before building
  --package     Generate the U-Boot bring-up package after validation
  --no-check    Skip the post-build ELF validation
  --jobs N      Parallel build jobs
  -h, --help    Show this help

Environment:
  OPENVELA_ROOT  openvela workspace root
  K1_CCACHE_DIR  writable ccache directory
  K1_JOBS        default parallel build jobs
EOF
}

fail()
{
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

cleanup()
{
  local index

  for ((index = ${#CREATED_LINKS[@]} - 1; index >= 0; index--)); do
    unlink "${CREATED_LINKS[${index}]}" 2>/dev/null || true
  done

  for ((index = ${#CREATED_DIRS[@]} - 1; index >= 0; index--)); do
    rmdir "${CREATED_DIRS[${index}]}" 2>/dev/null || true
  done
}

ensure_dir()
{
  local path="$1"

  if [[ ! -d "${path}" ]]; then
    mkdir "${path}"
    CREATED_DIRS+=("${path}")
  fi
}

ensure_mapping()
{
  local source="$1"
  local destination="$2"
  local actual

  if [[ -e "${destination}" || -L "${destination}" ]]; then
    actual="$(readlink -f -- "${destination}")"
    [[ "${actual}" == "${source}" ]] ||
      fail "${destination} exists but maps to ${actual}"
    return
  fi

  ln -s "${source}" "${destination}"
  CREATED_LINKS+=("${destination}")
}

while (($# > 0)); do
  case "$1" in
    --clean)
      CLEAN=1
      shift
      ;;
    --package)
      PACKAGE=1
      shift
      ;;
    --no-check)
      RUN_CHECK=0
      shift
      ;;
    --jobs)
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
[[ -d "${WORKSPACE_ROOT}/nuttx" ]] ||
  fail "NuttX tree not found under ${WORKSPACE_ROOT}"
[[ -d "${WORKSPACE_ROOT}/vendor" ]] ||
  fail "vendor tree not found under ${WORKSPACE_ROOT}"
[[ -d "${CONTEST_ROOT}/chip/k1" ]] || fail "K1 chip source is missing"
[[ -d "${CONTEST_ROOT}/board/k1/muse_pi_pro" ]] ||
  fail "MUSE Pi Pro board source is missing"

trap cleanup EXIT INT TERM

ensure_dir "${WORKSPACE_ROOT}/vendor/spacemit"
ensure_dir "${WORKSPACE_ROOT}/vendor/spacemit/chips"
ensure_dir "${WORKSPACE_ROOT}/vendor/spacemit/boards"
ensure_dir "${WORKSPACE_ROOT}/vendor/spacemit/boards/k1"

ensure_mapping \
  "${CONTEST_ROOT}/chip/k1" \
  "${WORKSPACE_ROOT}/vendor/spacemit/chips/k1"
ensure_mapping \
  "${CONTEST_ROOT}/board/k1/muse_pi_pro" \
  "${WORKSPACE_ROOT}/vendor/spacemit/boards/k1/muse_pi_pro"

mkdir -p "${CCACHE_ROOT}/tmp"

if ((CLEAN == 1)) && [[ -e "${BUILD_DIR}" ]]; then
  [[ "${BUILD_DIR}" == "${WORKSPACE_ROOT}/cmake_out/muse_pi_pro_nsh" ]] ||
    fail "refusing to clean unexpected path: ${BUILD_DIR}"
  rm -rf -- "${BUILD_DIR}"
fi

mkdir -p "${WORKSPACE_ROOT}/cmake_out"
BUILD_LOG="${WORKSPACE_ROOT}/cmake_out/k1-build.log"

printf 'Building MUSE Pi Pro K1 with %s jobs\n' "${JOBS}"
printf 'Toolchain: %s\n' \
  "${WORKSPACE_ROOT}/prebuilts/gcc/linux-x86_64/riscv-none-elf"
printf 'ccache:    %s\n' "${CCACHE_ROOT}"

set +e
(
  cd "${WORKSPACE_ROOT}"
  env \
    CCACHE_DIR="${CCACHE_ROOT}" \
    CCACHE_TEMPDIR="${CCACHE_ROOT}/tmp" \
    ./build.sh "${CONFIG_PATH}" --cmake "-j${JOBS}"
) 2>&1 | tee "${BUILD_LOG}"
BUILD_STATUS=${PIPESTATUS[0]}
set -e

((BUILD_STATUS == 0)) || fail "K1 build failed; see ${BUILD_LOG}"
grep -q 'build completed successfully' "${BUILD_LOG}" ||
  fail "build command returned success without the expected completion marker"
[[ -f "${BUILD_DIR}/nuttx" ]] || fail "ELF was not generated"

if ((RUN_CHECK == 1)); then
  "${SCRIPT_DIR}/check_k1_elf.sh" \
    --elf "${BUILD_DIR}/nuttx" \
    --config "${BUILD_DIR}/.config" \
    --uart-source "${CONTEST_ROOT}/chip/k1/k1_console.c"
fi

if ((PACKAGE == 1)); then
  "${SCRIPT_DIR}/package_k1_bringup.sh" \
    --elf "${BUILD_DIR}/nuttx" \
    --config "${BUILD_DIR}/.config"
fi

printf '\nK1 reproducible build completed\n'
printf '  ELF: %s\n' "${BUILD_DIR}/nuttx"
printf '  Log: %s\n' "${BUILD_LOG}"
