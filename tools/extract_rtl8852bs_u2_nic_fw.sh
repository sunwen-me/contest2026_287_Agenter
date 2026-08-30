#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
#
# Extract an exact RTL8852B firmware image initializer from the GPL source.

set -euo pipefail

usage()
{
  cat <<'EOF'
Usage: extract_rtl8852bs_u2_nic_fw.sh SOURCE OUTPUT [ARRAY_SYMBOL EXPECTED_BYTES]

SOURCE must be hal8852b_fw.c from the recorded SpacemiT Linux revision.
OUTPUT receives only the comma-separated initializer bytes for inclusion in
the GPL-2.0-only K1 RTL8852BS2 component.

Without ARRAY_SYMBOL and EXPECTED_BYTES, this extracts array_8852b_u2_nic
(276544 bytes).  Pass array_8852b_u2_nicce 341216 for the U2 NICCE image.
EOF
}

if (($# != 2 && $# != 4)); then
  usage >&2
  exit 1
fi

source_file="$1"
output_file="$2"
array_symbol="${3:-array_8852b_u2_nic}"
expected_bytes="${4:-276544}"
temporary_file="$(mktemp "${output_file}.tmp.XXXXXX")"

cleanup()
{
  rm -f -- "${temporary_file}"
}

trap cleanup EXIT INT TERM

[[ -f "${source_file}" ]] || {
  printf 'ERROR: source is not a file: %s\n' "${source_file}" >&2
  exit 1
}

[[ "${array_symbol}" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || {
  printf 'ERROR: invalid C array symbol: %s\n' "${array_symbol}" >&2
  exit 1
}

[[ "${expected_bytes}" =~ ^[0-9]+$ ]] || {
  printf 'ERROR: expected byte count is not decimal: %s\n' \
    "${expected_bytes}" >&2
  exit 1
}

awk -v array_symbol="${array_symbol}" '
  $0 == "u8 " array_symbol "[] = {" { in_array = 1; next }
  in_array && /^};$/ { exit }
  in_array { print }
' "${source_file}" >"${temporary_file}"

actual_bytes="$(rg -o '0x[0-9A-Fa-f]{2}' "${temporary_file}" | wc -l)"
[[ "${actual_bytes}" == "${expected_bytes}" ]] || {
  printf 'ERROR: extracted %s bytes, expected %s\n' \
    "${actual_bytes}" "${expected_bytes}" >&2
  exit 1
}

mv -- "${temporary_file}" "${output_file}"
trap - EXIT INT TERM
printf 'Extracted %s bytes to %s\n' "${actual_bytes}" "${output_file}"
