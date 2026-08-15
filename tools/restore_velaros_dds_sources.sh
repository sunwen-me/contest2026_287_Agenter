#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Restore the exact VelaROS DDS source baseline into an openvela workspace.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
LOCK_FILE="${SCRIPT_DIR}/velaros-dds-sources.lock"
FASTDDS_PATCH="${SCRIPT_DIR}/patches/fastdds-3.6.1-openvela.patch"
FASTDDS_VELAROS_PATCH="${SCRIPT_DIR}/patches/fastdds-3.6.1-velaros-no-native-shm.patch"
LIBCXXABI_PATCH="${SCRIPT_DIR}/patches/libcxxabi-nuttx-task-group-tls.patch"
LIBCXXABI_ROOT="${WORKSPACE_ROOT}/nuttx/libs/libxx/libcxxabi/libcxxabi"
BACKUP_ROOT="${VELAROS_BACKUP_ROOT:-/tmp}"
REPLACE=0
CHECK_ONLY=0
TEMP_ROOT=""
BACKUP_DIR=""

usage()
{
  cat <<'EOF'
Usage: restore_velaros_dds_sources.sh [options]

Options:
  --check       Verify locked revisions and all openvela/VelaROS patches
  --replace     Recoverably replace an existing unmarked/mismatched source tree
  -h, --help    Show this help

Environment:
  OPENVELA_ROOT         openvela workspace root
  VELAROS_BACKUP_ROOT   replacement backup parent (default: /tmp)

The script exports exact Git commits without their .git directories, applies
the reviewed Fast DDS, VelaROS profile and libc++abi NuttX patches, and writes
revision markers.
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
[[ -f "${FASTDDS_PATCH}" ]] || fail "Fast DDS patch is missing: ${FASTDDS_PATCH}"
[[ -f "${FASTDDS_VELAROS_PATCH}" ]] ||
  fail "VelaROS Fast DDS patch is missing: ${FASTDDS_VELAROS_PATCH}"
[[ -f "${LIBCXXABI_PATCH}" ]] || fail "libc++abi patch is missing: ${LIBCXXABI_PATCH}"
[[ -d "${WORKSPACE_ROOT}/external" ]] ||
  fail "external repository is missing under ${WORKSPACE_ROOT}"
[[ -d "${LIBCXXABI_ROOT}" ]] ||
  fail "openvela libc++abi checkout is missing: ${LIBCXXABI_ROOT}"

# shellcheck disable=SC1090
source "${LOCK_FILE}"

declare -a REQUIRED_LOCK_VARS=(
  FASTDDS_URL FASTDDS_REV FASTCDR_URL FASTCDR_REV
  FOONATHAN_MEMORY_URL FOONATHAN_MEMORY_REV ASIO_URL ASIO_REV
  OPENVELA_TINYXML2_VERSION OPENVELA_LIBCXXABI_REV
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

verify_tree()
{
  local name="$1"
  local destination="$2"
  local revision="$3"

  [[ -d "${destination}" ]] || fail "${name} source is missing: ${destination}"
  marker_matches "${destination}" "${revision}" ||
    fail "${name} revision marker is missing or mismatched; run with --replace"
  printf 'verified %-18s %s\n' "${name}" "${revision}"
}

backup_existing()
{
  local destination="$1"

  [[ -e "${destination}" ]] || return
  ((REPLACE == 1)) ||
    fail "${destination} already exists without the expected marker; use --replace"

  if [[ -z "${BACKUP_DIR}" ]]; then
    BACKUP_DIR="${BACKUP_ROOT%/}/velaros-dds-source-backup-$(date -u +%Y%m%dT%H%M%SZ)"
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
    printf 'present  %-18s %s\n' "${name}" "${revision}"
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
  printf 'restored %-18s %s\n' "${name}" "${revision}"
}

port_patch_is_applied()
{
  # The product-profile patch intentionally changes context introduced by the
  # base port patch.  If the former reverses cleanly, it proves the latter was
  # present when the profile patch was applied.
  if velaros_patch_is_applied; then
    return 0
  fi

  patch --batch --silent --dry-run --reverse \
    -d "${WORKSPACE_ROOT}/external" -p1 <"${FASTDDS_PATCH}" >/dev/null 2>&1
}

velaros_patch_is_applied()
{
  patch --batch --silent --dry-run --reverse \
    -d "${WORKSPACE_ROOT}/external" -p1 <"${FASTDDS_VELAROS_PATCH}" >/dev/null 2>&1
}

apply_port_patch()
{
  if patch --batch --silent --dry-run \
    -d "${WORKSPACE_ROOT}/external" -p1 <"${FASTDDS_PATCH}" >/dev/null 2>&1; then
    patch --batch -d "${WORKSPACE_ROOT}/external" -p1 <"${FASTDDS_PATCH}"
    printf 'applied openvela Fast DDS port patch\n'
  elif port_patch_is_applied; then
    printf 'present openvela Fast DDS port patch\n'
  else
    fail "port patch is neither applicable nor already applied"
  fi
}

apply_velaros_patch()
{
  if patch --batch --silent --dry-run \
    -d "${WORKSPACE_ROOT}/external" -p1 <"${FASTDDS_VELAROS_PATCH}" >/dev/null 2>&1; then
    patch --batch -d "${WORKSPACE_ROOT}/external" -p1 <"${FASTDDS_VELAROS_PATCH}"
    printf 'applied VelaROS no-native-SHM profile patch\n'
  elif velaros_patch_is_applied; then
    printf 'present VelaROS no-native-SHM profile patch\n'
  else
    fail "VelaROS profile patch is neither applicable nor already applied"
  fi
}

verify_libcxxabi_revision()
{
  local revision

  revision="$(git -C "${LIBCXXABI_ROOT}" rev-parse HEAD 2>/dev/null)" ||
    fail "cannot read openvela libc++abi revision"
  [[ "${revision}" == "${OPENVELA_LIBCXXABI_REV}" ]] ||
    fail "openvela libc++abi is ${revision}, expected ${OPENVELA_LIBCXXABI_REV}"
  printf 'verified %-18s %s\n' libc++abi "${revision}"
}

libcxxabi_patch_is_applied()
{
  patch --batch --silent --dry-run --reverse \
    -d "${LIBCXXABI_ROOT}" -p1 <"${LIBCXXABI_PATCH}" >/dev/null 2>&1
}

apply_libcxxabi_patch()
{
  verify_libcxxabi_revision
  if patch --batch --silent --dry-run \
    -d "${LIBCXXABI_ROOT}" -p1 <"${LIBCXXABI_PATCH}" >/dev/null 2>&1; then
    patch --batch -d "${LIBCXXABI_ROOT}" -p1 <"${LIBCXXABI_PATCH}"
    printf 'applied openvela libc++abi task-group TLS patch\n'
  elif libcxxabi_patch_is_applied; then
    printf 'present openvela libc++abi task-group TLS patch\n'
  else
    fail "libc++abi patch is neither applicable nor already applied"
  fi
}

tinyxml_version="$(
  sed -n \
    -e 's/\r$//' \
    -e 's/^#define TINYXML2_\(MAJOR\|MINOR\|PATCH\)_VERSION \([0-9][0-9]*\)$/\2/p' \
    "${WORKSPACE_ROOT}/external/tinyxml2/tinyxml2/tinyxml2.h" |
    paste -sd.
)"
[[ "${tinyxml_version}" == "${OPENVELA_TINYXML2_VERSION}" ]] ||
  fail "workspace TinyXML2 is ${tinyxml_version:-unknown}, expected ${OPENVELA_TINYXML2_VERSION}"

if ((CHECK_ONLY == 1)); then
  verify_tree Fast-DDS "${WORKSPACE_ROOT}/external/fastdds/Fast-DDS" "${FASTDDS_REV}"
  verify_tree Fast-CDR "${WORKSPACE_ROOT}/external/fastdds/Fast-CDR" "${FASTCDR_REV}"
  verify_tree foonathan_memory "${WORKSPACE_ROOT}/external/fastdds/memory" \
    "${FOONATHAN_MEMORY_REV}"
  verify_tree Asio "${WORKSPACE_ROOT}/external/asio/asio" "${ASIO_REV}"
  printf 'verified %-18s %s\n' TinyXML2 "${tinyxml_version}"
  port_patch_is_applied || fail "openvela Fast DDS port patch is not applied"
  printf 'verified openvela Fast DDS port patch\n'
  velaros_patch_is_applied ||
    fail "VelaROS no-native-SHM profile patch is not applied"
  printf 'verified VelaROS no-native-SHM profile patch\n'
  verify_libcxxabi_revision
  libcxxabi_patch_is_applied ||
    fail "openvela libc++abi task-group TLS patch is not applied"
  printf 'verified openvela libc++abi task-group TLS patch\n'
  exit 0
fi

TEMP_ROOT="$(mktemp -d /tmp/velaros-dds-restore.XXXXXX)"
trap cleanup EXIT INT TERM

export_revision Fast-DDS "${FASTDDS_URL}" "${FASTDDS_REV}" \
  "${WORKSPACE_ROOT}/external/fastdds/Fast-DDS"
export_revision Fast-CDR "${FASTCDR_URL}" "${FASTCDR_REV}" \
  "${WORKSPACE_ROOT}/external/fastdds/Fast-CDR"
export_revision foonathan_memory "${FOONATHAN_MEMORY_URL}" \
  "${FOONATHAN_MEMORY_REV}" "${WORKSPACE_ROOT}/external/fastdds/memory"
export_revision Asio "${ASIO_URL}" "${ASIO_REV}" \
  "${WORKSPACE_ROOT}/external/asio/asio"

apply_port_patch
apply_velaros_patch
apply_libcxxabi_patch

printf '\nVelaROS DDS sources and runtime patches restored.\n'
printf 'TinyXML2: %s (workspace package)\n' "${tinyxml_version}"
if [[ -n "${BACKUP_DIR}" ]]; then
  printf 'Previous sources: %s\n' "${BACKUP_DIR}"
fi
