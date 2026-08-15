#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

OUTPUT_DIR="${1:-}"
CONFIG_FILE="${OUTPUT_DIR}/.config"
BUILD_NINJA="${OUTPUT_DIR}/build.ninja"

fail()
{
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

[[ -n "${OUTPUT_DIR}" ]] ||
  fail "usage: check_velaros_fastdds_profile.sh OUTPUT_DIR"
[[ -f "${CONFIG_FILE}" ]] || fail "missing generated config: ${CONFIG_FILE}"
[[ -f "${BUILD_NINJA}" ]] || fail "missing Ninja graph: ${BUILD_NINJA}"

grep -qx 'CONFIG_FASTDDS_VELAROS_STATIC_PROFILE=y' "${CONFIG_FILE}" ||
  fail "CONFIG_FASTDDS_VELAROS_STATIC_PROFILE is not enabled"
grep -q -- '-DFASTDDS_VELAROS_STATIC_PROFILE' "${BUILD_NINJA}" ||
  fail "VelaROS profile define did not reach the Fast DDS compiler rules"
grep -q -- '-DFASTDDS_SHM_TRANSPORT_DISABLED' "${BUILD_NINJA}" ||
  fail "Fast DDS native SHM is not compile-time disabled"
grep -q -- '-DFASTDDS_DATASHARING_DISABLED' "${BUILD_NINJA}" ||
  fail "Fast DDS DataSharing is not compile-time disabled"

# Check the generated build graph, not the object directory: incremental builds
# may leave stale .o files after a source is removed from source.cmake.
for source in \
    '/fastdds/builtin/type_lookup_service/' \
    '/fastdds/rpc/' \
    '/rtps/builtin/discovery/database/' \
    '/rtps/builtin/discovery/endpoint/EDPClient.cpp' \
    '/rtps/builtin/discovery/endpoint/EDPServer.cpp' \
    '/rtps/builtin/discovery/endpoint/EDPServerListeners.cpp' \
    '/rtps/builtin/discovery/endpoint/EDPStatic.cpp' \
    '/rtps/builtin/discovery/participant/PDPClient.cpp' \
    '/rtps/builtin/discovery/participant/PDPClientListener.cpp' \
    '/rtps/builtin/discovery/participant/PDPServer.cpp' \
    '/rtps/builtin/discovery/participant/PDPServerListener.cpp' \
    '/rtps/builtin/discovery/participant/timedevent/DSClientEvent.cpp' \
    '/rtps/builtin/discovery/participant/timedevent/DServerEvent.cpp' \
    '/rtps/transport/tcp/RTCPMessageManager.cpp' \
    '/rtps/transport/tcp/TCPControlMessage.cpp' \
    '/rtps/transport/TCPAcceptor.cpp' \
    '/rtps/transport/TCPAcceptorBasic.cpp' \
    '/rtps/transport/TCPChannelResource.cpp' \
    '/rtps/transport/TCPChannelResourceBasic.cpp' \
    '/rtps/transport/TCPTransportInterface.cpp' \
    '/rtps/transport/TCPv4Transport.cpp' \
    '/rtps/transport/TCPv6Transport.cpp' \
    '/rtps/transport/UDPv6Transport.cpp' \
    '/rtps/DataSharing/' \
    '/rtps/transport/shared_mem/'; do
  if grep -Fq "${source}" "${BUILD_NINJA}"; then
    fail "excluded Fast DDS source is still in the build graph: ${source}"
  fi
done

for source in \
    '/rtps/builtin/discovery/endpoint/EDPSimple.cpp' \
    '/rtps/builtin/discovery/participant/PDPSimple.cpp' \
    '/rtps/transport/UDPv4Transport.cpp'; do
  grep -Fq "${source}" "${BUILD_NINJA}" ||
    fail "required VelaROS Fast DDS source is missing: ${source}"
done

archive_rule="$(grep -m1 -E '^build .*Fast-DDS/src/cpp/libfastdds\.a:' "${BUILD_NINJA}" || true)"
[[ -n "${archive_rule}" ]] || fail "cannot locate the libfastdds.a Ninja rule"
object_count="$(printf '%s\n' "${archive_rule}" | grep -oE '\.(cpp|cxx)\.o' | wc -l)"

FAST_DDS_ARCHIVE="${OUTPUT_DIR}/apps/external/fastdds/Fast-DDS/src/cpp/libfastdds.a"
ELF="${OUTPUT_DIR}/nuttx"
[[ -f "${FAST_DDS_ARCHIVE}" ]] || fail "Fast DDS archive is missing"
[[ -f "${ELF}" ]] || fail "firmware ELF is missing"

if ar t "${FAST_DDS_ARCHIVE}" | grep -Eq 'DataSharing|SharedMem|shared_mem'; then
  fail "native SHM/DataSharing object remains in libfastdds.a"
fi

native_shm_symbols='DataSharing(List|Notification|PayloadPool|Notifier)|SharedMem(Transport|Watchdog|Segment)|BoostAtExitRegistry'
if nm -C "${ELF}" | grep -Eq "${native_shm_symbols}"; then
  fail "native SHM/DataSharing symbol remains in firmware ELF"
fi

printf 'VelaROS Fast DDS static profile: PASS\n'
printf 'compiled Fast DDS translation units: %s\n' "${object_count}"
printf 'discovery: SIMPLE PDP + SIMPLE EDP\n'
printf 'builtin transport: UDPv4 only\n'
printf 'excluded: TypeLookup service, DDS-RPC, Discovery Server/Client/database, static EDP, TCP, UDPv6, native SHM/DataSharing\n'
