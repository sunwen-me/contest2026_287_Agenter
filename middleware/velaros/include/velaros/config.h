/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS_CONFIG_H
#define VELAROS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VELAROS_CONFIG_KEY_BRIDGE_ENABLED \
  "persist.velaros.bridge"
#define VELAROS_CONFIG_KEY_DOMAIN_ID \
  "persist.velaros.domain"
#define VELAROS_CONFIG_KEY_PARTICIPANT_ID \
  "persist.velaros.participant"
#define VELAROS_CONFIG_KEY_HEARTBEAT_PERIOD_MS \
  "persist.velaros.heartbeat_ms"
#define VELAROS_RUNTIME_KEY_STATE "velaros.runtime.state"

#define VELAROS_DOMAIN_ID_MIN 0
#define VELAROS_DOMAIN_ID_MAX 232
#define VELAROS_PARTICIPANT_ID_MIN 0
#define VELAROS_PARTICIPANT_ID_MAX 119
#define VELAROS_HEARTBEAT_PERIOD_MS_MIN 10
#define VELAROS_HEARTBEAT_PERIOD_MS_MAX 60000

typedef enum velaros_runtime_state_e
{
  VELAROS_RUNTIME_STOPPED = 0,
  VELAROS_RUNTIME_STARTING = 1,
  VELAROS_RUNTIME_RUNNING = 2,
  VELAROS_RUNTIME_STOPPING = 3,
  VELAROS_RUNTIME_FAULT = 4,
} velaros_runtime_state_t;

typedef struct velaros_config_s
{
  bool bridge_enabled;
  int32_t domain_id;
  int32_t participant_id;
  int32_t heartbeat_period_ms;
} velaros_config_t;

void velaros_config_get_defaults(velaros_config_t * config);

/* Load the persistent openVela KVDB configuration.  When initialize_missing
 * is true, missing or invalid entries are replaced atomically with defaults.
 */
int velaros_config_load(
  velaros_config_t * config, bool initialize_missing);

int velaros_config_store_bridge_enabled(bool enabled);
int velaros_config_store_domain_id(int32_t domain_id);
int velaros_config_store_participant_id(int32_t participant_id);
int velaros_config_store_heartbeat_period_ms(int32_t period_ms);

int velaros_runtime_set_state(velaros_runtime_state_t state);
int velaros_runtime_get_state(velaros_runtime_state_t * state);

#ifdef __cplusplus
}
#endif

#endif
