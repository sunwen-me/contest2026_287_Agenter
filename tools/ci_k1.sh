#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# One-command K1 validation for local use and hosted static CI.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="$(cd -- "${CONTEST_ROOT}/.." && pwd)"
STATIC_ONLY=0
JOBS="${K1_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '8')}"

usage()
{
  cat <<'EOF'
Usage: ci_k1.sh [options]

Options:
  --static-only  Run repository-only checks without openvela/toolchain
  --jobs N       Parallel jobs for the full clean build
  -h, --help     Show this help
EOF
}

while (($# > 0)); do
  case "$1" in
    --static-only)
      STATIC_ONLY=1
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
      printf 'ERROR: unknown option: %s\n' "$1" >&2
      exit 1
      ;;
  esac
done

[[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] ||
  {
    printf 'ERROR: jobs must be a positive integer\n' >&2
    exit 1
  }

"${SCRIPT_DIR}/check_k1_sources.sh"

if ((STATIC_ONLY == 0)); then
  "${SCRIPT_DIR}/build_k1.sh" --clean --package --jobs "${JOBS}"

  "${SCRIPT_DIR}/build_k1.sh" \
    --clean \
    --package \
    --config vendor/spacemit/boards/k1/muse_pi_pro/configs/hardware_bringup \
    --build-dir "${WORKSPACE_ROOT}/cmake_out/k1-ci-hardware_bringup" \
    --package-dir "${WORKSPACE_ROOT}/out/k1-bringup-hardware_bringup" \
    --experimental-irq \
    --jobs "${JOBS}"
fi

printf '\nK1 CI completed (%s)\n' \
  "$([[ "${STATIC_ONLY}" == 1 ]] && printf 'static' || printf 'full')"
