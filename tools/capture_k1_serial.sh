#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONTEST_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_ROOT="${OPENVELA_ROOT:-$(cd -- "${CONTEST_ROOT}/.." && pwd)}"
OUTPUT_DIR="${K1_SERIAL_LOG_DIR:-${WORKSPACE_ROOT}/out/k1-serial}"

DEVICE=""
OUTPUT=""
BAUD=115200
DURATION=0

usage()
{
  cat <<'EOF'
Usage: capture_k1_serial.sh --device PATH [options]

Options:
  --device PATH  Serial device, for example /dev/ttyUSB0
  --output PATH  Raw log path (default: timestamped under out/k1-serial)
  --baud RATE    Baud rate (default: 115200)
  --duration SEC Stop after SEC; zero waits for Ctrl-C
  -h, --help     Show this help
EOF
}

while (($# > 0)); do
  case "$1" in
    --device)
      DEVICE="$2"
      shift 2
      ;;
    --output)
      OUTPUT="$2"
      shift 2
      ;;
    --baud)
      BAUD="$2"
      shift 2
      ;;
    --duration)
      DURATION="$2"
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

[[ -n "${DEVICE}" ]] ||
  {
    printf 'ERROR: --device is required\n' >&2
    exit 1
  }

if [[ -z "${OUTPUT}" ]]; then
  mkdir -p "${OUTPUT_DIR}"
  OUTPUT="${OUTPUT_DIR}/k1-$(date -u +%Y%m%dT%H%M%SZ).log"
fi

exec python3 "${SCRIPT_DIR}/capture_k1_serial.py" \
  --device "${DEVICE}" \
  --output "${OUTPUT}" \
  --baud "${BAUD}" \
  --duration "${DURATION}"
