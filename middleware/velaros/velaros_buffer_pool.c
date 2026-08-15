/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "velaros/buffer_pool.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#if CONFIG_VELAROS_BUFFER_POOL_MAX_LEASES > 32
#  error "VelaROS buffer leases use a 32-bit bounded bitmap"
#endif

typedef struct velaros_buffer_slot_s
{
  uint32_t generation;
  uint32_t capacity;
  uint32_t length;
  uint32_t lease_mask;
  uint32_t owners[CONFIG_VELAROS_BUFFER_POOL_MAX_LEASES];
  bool committed;
} velaros_buffer_slot_t;

static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_payloads
  [CONFIG_VELAROS_BUFFER_POOL_SLOTS]
  [CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE] __attribute__((aligned(64)));
static velaros_buffer_slot_t g_slots[CONFIG_VELAROS_BUFFER_POOL_SLOTS];
static velaros_buffer_pool_stats_t g_stats;
static uint64_t g_pool_cookie;

static uint64_t velaros_mix64(uint64_t value)
{
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  value ^= value >> 31;
  return value;
}

static void velaros_buffer_pool_initialize_locked(void)
{
  if (g_pool_cookie == 0) {
    struct timespec now = {0};
    uint64_t seed = (uint64_t)(uintptr_t)&g_payloads[0][0];

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
      seed ^= ((uint64_t)now.tv_sec << 32) ^ (uint64_t)now.tv_nsec;
    }
    seed ^= UINT64_C(0x56454c41524f5301);
    g_pool_cookie = velaros_mix64(seed);
    if (g_pool_cookie == 0) {
      g_pool_cookie = UINT64_C(0x56454c41524f5301);
    }

    g_stats.slot_count = CONFIG_VELAROS_BUFFER_POOL_SLOTS;
    g_stats.slot_size = CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE;
    g_stats.payload_bytes =
      (size_t)CONFIG_VELAROS_BUFFER_POOL_SLOTS *
      (size_t)CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE;
  }
}

static velaros_buffer_ret_t velaros_validate_locked(
  const velaros_buffer_descriptor_t * descriptor,
  velaros_buffer_slot_t ** slot_out)
{
  velaros_buffer_slot_t * slot;
  uint32_t lease_bit;

  if (descriptor == NULL || descriptor->pool_cookie != g_pool_cookie ||
      descriptor->slot >= CONFIG_VELAROS_BUFFER_POOL_SLOTS ||
      descriptor->lease >= CONFIG_VELAROS_BUFFER_POOL_MAX_LEASES) {
    return VELAROS_BUFFER_STALE_DESCRIPTOR;
  }

  slot = &g_slots[descriptor->slot];
  lease_bit = UINT32_C(1) << descriptor->lease;
  if (slot->generation != descriptor->generation ||
      (slot->lease_mask & lease_bit) == 0) {
    return VELAROS_BUFFER_STALE_DESCRIPTOR;
  }
  if (descriptor->owner == 0 ||
      slot->owners[descriptor->lease] != descriptor->owner) {
    return VELAROS_BUFFER_OWNER_MISMATCH;
  }

  if (slot_out != NULL) {
    *slot_out = slot;
  }
  return VELAROS_BUFFER_OK;
}

static void velaros_note_rejection_locked(void)
{
  ++g_stats.rejected;
}

uint64_t velaros_buffer_pool_cookie(void)
{
  uint64_t cookie = 0;

  if (pthread_mutex_lock(&g_pool_lock) != 0) {
    return 0;
  }
  velaros_buffer_pool_initialize_locked();
  cookie = g_pool_cookie;
  (void)pthread_mutex_unlock(&g_pool_lock);
  return cookie;
}

velaros_buffer_ret_t velaros_buffer_acquire(
  size_t capacity,
  uint32_t owner,
  velaros_buffer_descriptor_t * descriptor,
  void ** writable_payload)
{
  size_t index;
  velaros_buffer_slot_t * slot;

  if (capacity == 0 || descriptor == NULL || writable_payload == NULL ||
      owner == 0) {
    return VELAROS_BUFFER_INVALID_ARGUMENT;
  }
  if (capacity > CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE) {
    return VELAROS_BUFFER_TOO_LARGE;
  }
  if (pthread_mutex_lock(&g_pool_lock) != 0) {
    return VELAROS_BUFFER_INTERNAL_ERROR;
  }
  velaros_buffer_pool_initialize_locked();

  for (index = 0; index < CONFIG_VELAROS_BUFFER_POOL_SLOTS; ++index) {
    slot = &g_slots[index];
    if (slot->lease_mask != 0) {
      continue;
    }

    ++slot->generation;
    if (slot->generation == 0) {
      ++slot->generation;
    }
    slot->capacity = (uint32_t)capacity;
    slot->length = 0;
    slot->lease_mask = UINT32_C(1);
    memset(slot->owners, 0, sizeof(slot->owners));
    slot->owners[0] = owner;
    slot->committed = false;

    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->pool_cookie = g_pool_cookie;
    descriptor->slot = (uint32_t)index;
    descriptor->generation = slot->generation;
    descriptor->lease = 0;
    descriptor->owner = owner;
    descriptor->capacity = (uint32_t)capacity;
    *writable_payload = &g_payloads[index][0];

    ++g_stats.active_slots;
    ++g_stats.active_leases;
    ++g_stats.allocations;
    if (g_stats.active_slots > g_stats.high_water_slots) {
      g_stats.high_water_slots = g_stats.active_slots;
    }
    (void)pthread_mutex_unlock(&g_pool_lock);
    return VELAROS_BUFFER_OK;
  }

  ++g_stats.exhausted;
  (void)pthread_mutex_unlock(&g_pool_lock);
  return VELAROS_BUFFER_EXHAUSTED;
}

velaros_buffer_ret_t velaros_buffer_commit(
  velaros_buffer_descriptor_t * descriptor,
  size_t length)
{
  velaros_buffer_slot_t * slot = NULL;
  velaros_buffer_ret_t ret;

  if (descriptor == NULL) {
    return VELAROS_BUFFER_INVALID_ARGUMENT;
  }
  if (pthread_mutex_lock(&g_pool_lock) != 0) {
    return VELAROS_BUFFER_INTERNAL_ERROR;
  }
  velaros_buffer_pool_initialize_locked();
  ret = velaros_validate_locked(descriptor, &slot);
  if (ret != VELAROS_BUFFER_OK) {
    velaros_note_rejection_locked();
  } else if (length > slot->capacity) {
    ret = VELAROS_BUFFER_TOO_LARGE;
    velaros_note_rejection_locked();
  } else {
    slot->length = (uint32_t)length;
    slot->committed = true;
    descriptor->length = (uint32_t)length;
    descriptor->capacity = slot->capacity;
    descriptor->flags =
      VELAROS_BUFFER_FLAG_COMMITTED | VELAROS_BUFFER_FLAG_READ_ONLY;
  }
  (void)pthread_mutex_unlock(&g_pool_lock);
  return ret;
}

velaros_buffer_ret_t velaros_buffer_retain(
  const velaros_buffer_descriptor_t * source,
  uint32_t new_owner,
  velaros_buffer_descriptor_t * retained)
{
  velaros_buffer_slot_t * slot = NULL;
  velaros_buffer_ret_t ret;
  uint32_t lease;

  if (source == NULL || retained == NULL || new_owner == 0) {
    return VELAROS_BUFFER_INVALID_ARGUMENT;
  }
  if (pthread_mutex_lock(&g_pool_lock) != 0) {
    return VELAROS_BUFFER_INTERNAL_ERROR;
  }
  velaros_buffer_pool_initialize_locked();
  ret = velaros_validate_locked(source, &slot);
  if (ret != VELAROS_BUFFER_OK) {
    velaros_note_rejection_locked();
  } else if (!slot->committed) {
    ret = VELAROS_BUFFER_NOT_COMMITTED;
    velaros_note_rejection_locked();
  } else {
    for (lease = 0; lease < CONFIG_VELAROS_BUFFER_POOL_MAX_LEASES; ++lease) {
      uint32_t lease_bit = UINT32_C(1) << lease;

      if ((slot->lease_mask & lease_bit) != 0) {
        continue;
      }
      slot->lease_mask |= lease_bit;
      slot->owners[lease] = new_owner;
      *retained = *source;
      retained->lease = lease;
      retained->owner = new_owner;
      retained->length = slot->length;
      retained->capacity = slot->capacity;
      retained->flags =
        VELAROS_BUFFER_FLAG_COMMITTED | VELAROS_BUFFER_FLAG_READ_ONLY;
      ++g_stats.active_leases;
      ++g_stats.retains;
      (void)pthread_mutex_unlock(&g_pool_lock);
      return VELAROS_BUFFER_OK;
    }
    ret = VELAROS_BUFFER_LEASE_EXHAUSTED;
    velaros_note_rejection_locked();
  }
  (void)pthread_mutex_unlock(&g_pool_lock);
  return ret;
}

velaros_buffer_ret_t velaros_buffer_map(
  const velaros_buffer_descriptor_t * descriptor,
  const void ** payload,
  size_t * length)
{
  velaros_buffer_slot_t * slot = NULL;
  velaros_buffer_ret_t ret;

  if (descriptor == NULL || payload == NULL || length == NULL) {
    return VELAROS_BUFFER_INVALID_ARGUMENT;
  }
  if (pthread_mutex_lock(&g_pool_lock) != 0) {
    return VELAROS_BUFFER_INTERNAL_ERROR;
  }
  velaros_buffer_pool_initialize_locked();
  ret = velaros_validate_locked(descriptor, &slot);
  if (ret != VELAROS_BUFFER_OK) {
    velaros_note_rejection_locked();
  } else if (!slot->committed) {
    ret = VELAROS_BUFFER_NOT_COMMITTED;
    velaros_note_rejection_locked();
  } else if (descriptor->length != slot->length ||
             descriptor->capacity != slot->capacity ||
             (descriptor->flags & VELAROS_BUFFER_FLAG_COMMITTED) == 0) {
    ret = VELAROS_BUFFER_STALE_DESCRIPTOR;
    velaros_note_rejection_locked();
  } else {
    *payload = &g_payloads[descriptor->slot][0];
    *length = slot->length;
  }
  (void)pthread_mutex_unlock(&g_pool_lock);
  return ret;
}

velaros_buffer_ret_t velaros_buffer_release(
  const velaros_buffer_descriptor_t * descriptor)
{
  velaros_buffer_slot_t * slot = NULL;
  velaros_buffer_ret_t ret;
  uint32_t lease_bit;

  if (descriptor == NULL) {
    return VELAROS_BUFFER_INVALID_ARGUMENT;
  }
  if (pthread_mutex_lock(&g_pool_lock) != 0) {
    return VELAROS_BUFFER_INTERNAL_ERROR;
  }
  velaros_buffer_pool_initialize_locked();
  ret = velaros_validate_locked(descriptor, &slot);
  if (ret != VELAROS_BUFFER_OK) {
    velaros_note_rejection_locked();
    (void)pthread_mutex_unlock(&g_pool_lock);
    return ret;
  }

  lease_bit = UINT32_C(1) << descriptor->lease;
  slot->lease_mask &= ~lease_bit;
  slot->owners[descriptor->lease] = 0;
  --g_stats.active_leases;
  ++g_stats.releases;
  if (slot->lease_mask == 0) {
    slot->capacity = 0;
    slot->length = 0;
    slot->committed = false;
    --g_stats.active_slots;
  }
  (void)pthread_mutex_unlock(&g_pool_lock);
  return VELAROS_BUFFER_OK;
}

size_t velaros_buffer_reclaim_owner(uint32_t owner)
{
  size_t reclaimed = 0;
  size_t slot_index;

  if (owner == 0 || pthread_mutex_lock(&g_pool_lock) != 0) {
    return 0;
  }
  velaros_buffer_pool_initialize_locked();
  for (slot_index = 0;
       slot_index < CONFIG_VELAROS_BUFFER_POOL_SLOTS;
       ++slot_index) {
    velaros_buffer_slot_t * slot = &g_slots[slot_index];
    uint32_t lease;

    for (lease = 0; lease < CONFIG_VELAROS_BUFFER_POOL_MAX_LEASES; ++lease) {
      uint32_t lease_bit = UINT32_C(1) << lease;

      if ((slot->lease_mask & lease_bit) != 0 &&
          slot->owners[lease] == owner) {
        slot->lease_mask &= ~lease_bit;
        slot->owners[lease] = 0;
        --g_stats.active_leases;
        ++g_stats.reclaimed;
        ++reclaimed;
      }
    }
    if (slot->lease_mask == 0 && slot->capacity != 0) {
      slot->capacity = 0;
      slot->length = 0;
      slot->committed = false;
      --g_stats.active_slots;
    }
  }
  (void)pthread_mutex_unlock(&g_pool_lock);
  return reclaimed;
}

velaros_buffer_ret_t velaros_buffer_pool_reset(void)
{
  size_t slot_index;

  if (pthread_mutex_lock(&g_pool_lock) != 0) {
    return VELAROS_BUFFER_INTERNAL_ERROR;
  }
  velaros_buffer_pool_initialize_locked();
  for (slot_index = 0;
       slot_index < CONFIG_VELAROS_BUFFER_POOL_SLOTS;
       ++slot_index) {
    if (g_slots[slot_index].lease_mask != 0) {
      (void)pthread_mutex_unlock(&g_pool_lock);
      return VELAROS_BUFFER_BUSY;
    }
  }
  /* Keep each generation monotonically increasing across a test or service
   * reset.  Clearing it would allow an old descriptor to become valid again
   * when the same slot, lease and owner tuple is reused.
   */
  for (slot_index = 0;
       slot_index < CONFIG_VELAROS_BUFFER_POOL_SLOTS;
       ++slot_index) {
    g_slots[slot_index].capacity = 0;
    g_slots[slot_index].length = 0;
    g_slots[slot_index].lease_mask = 0;
    memset(g_slots[slot_index].owners, 0, sizeof(g_slots[slot_index].owners));
    g_slots[slot_index].committed = false;
  }
  memset(&g_stats, 0, sizeof(g_stats));
  g_stats.slot_count = CONFIG_VELAROS_BUFFER_POOL_SLOTS;
  g_stats.slot_size = CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE;
  g_stats.payload_bytes =
    (size_t)CONFIG_VELAROS_BUFFER_POOL_SLOTS *
    (size_t)CONFIG_VELAROS_BUFFER_POOL_SLOT_SIZE;
  (void)pthread_mutex_unlock(&g_pool_lock);
  return VELAROS_BUFFER_OK;
}

void velaros_buffer_pool_get_stats(velaros_buffer_pool_stats_t * stats)
{
  if (stats == NULL) {
    return;
  }
  memset(stats, 0, sizeof(*stats));
  if (pthread_mutex_lock(&g_pool_lock) != 0) {
    return;
  }
  velaros_buffer_pool_initialize_locked();
  *stats = g_stats;
  (void)pthread_mutex_unlock(&g_pool_lock);
}
