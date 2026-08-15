/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__BUFFER_POOL_H
#define VELAROS__BUFFER_POOL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

/* A descriptor is safe to copy through uORB/RPMsg.  It deliberately contains
 * no process pointer or physical address.  The tuple pool_cookie + slot +
 * generation identifies storage, while lease + owner identifies one bounded
 * right to access and release it.
 */
typedef struct velaros_buffer_descriptor_s
{
  uint64_t pool_cookie;
  uint32_t slot;
  uint32_t generation;
  uint32_t lease;
  uint32_t owner;
  uint32_t length;
  uint32_t capacity;
  uint32_t flags;
} velaros_buffer_descriptor_t;

enum
{
  VELAROS_BUFFER_FLAG_COMMITTED = 1u << 0,
  VELAROS_BUFFER_FLAG_READ_ONLY = 1u << 1
};

typedef enum velaros_buffer_ret_e
{
  VELAROS_BUFFER_OK = 0,
  VELAROS_BUFFER_INVALID_ARGUMENT = -1,
  VELAROS_BUFFER_TOO_LARGE = -2,
  VELAROS_BUFFER_EXHAUSTED = -3,
  VELAROS_BUFFER_STALE_DESCRIPTOR = -4,
  VELAROS_BUFFER_LEASE_EXHAUSTED = -5,
  VELAROS_BUFFER_NOT_COMMITTED = -6,
  VELAROS_BUFFER_OWNER_MISMATCH = -7,
  VELAROS_BUFFER_BUSY = -8,
  VELAROS_BUFFER_INTERNAL_ERROR = -9
} velaros_buffer_ret_t;

typedef struct velaros_buffer_pool_stats_s
{
  size_t slot_count;
  size_t slot_size;
  size_t payload_bytes;
  size_t active_slots;
  size_t active_leases;
  size_t high_water_slots;
  size_t allocations;
  size_t retains;
  size_t releases;
  size_t exhausted;
  size_t rejected;
  size_t reclaimed;
} velaros_buffer_pool_stats_t;

uint64_t velaros_buffer_pool_cookie(void);

/* Reserve one fixed slot and its first lease.  The writable pointer remains
 * stable until every lease for the descriptor generation has been released.
 */
velaros_buffer_ret_t velaros_buffer_acquire(
  size_t capacity,
  uint32_t owner,
  velaros_buffer_descriptor_t * descriptor,
  void ** writable_payload);

/* Freeze the payload length before a descriptor is transferred. */
velaros_buffer_ret_t velaros_buffer_commit(
  velaros_buffer_descriptor_t * descriptor,
  size_t length);

/* Create another bounded lease for a consumer. */
velaros_buffer_ret_t velaros_buffer_retain(
  const velaros_buffer_descriptor_t * source,
  uint32_t new_owner,
  velaros_buffer_descriptor_t * retained);

/* Map a committed descriptor without copying its payload. */
velaros_buffer_ret_t velaros_buffer_map(
  const velaros_buffer_descriptor_t * descriptor,
  const void ** payload,
  size_t * length);

velaros_buffer_ret_t velaros_buffer_release(
  const velaros_buffer_descriptor_t * descriptor);

/* Explicit lifecycle cleanup for a stopped task/endpoint.  There is no
 * timeout worker: the owner which created a lease is responsible for cleanup.
 */
size_t velaros_buffer_reclaim_owner(uint32_t owner);

velaros_buffer_ret_t velaros_buffer_pool_reset(void);

void velaros_buffer_pool_get_stats(velaros_buffer_pool_stats_t * stats);

#ifdef __cplusplus
}
#endif

#endif  /* VELAROS__BUFFER_POOL_H */
