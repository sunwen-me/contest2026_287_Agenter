/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/config.h"

#include <errno.h>
#include <kvdb.h>
#include <stddef.h>

#define VELAROS_DEFAULT_DOMAIN_ID 0
#define VELAROS_DEFAULT_PARTICIPANT_ID 0
#define VELAROS_DEFAULT_HEARTBEAT_PERIOD_MS 1000

static bool velaros_value_in_range(
  int32_t value, int32_t minimum, int32_t maximum)
{
  return value >= minimum && value <= maximum;
}

static int velaros_commit_if_needed(bool commit)
{
  return commit ? property_commit() : 0;
}

void velaros_config_get_defaults(velaros_config_t * config)
{
  if (config == NULL) {
    return;
  }

  config->bridge_enabled = true;
  config->domain_id = VELAROS_DEFAULT_DOMAIN_ID;
  config->participant_id = VELAROS_DEFAULT_PARTICIPANT_ID;
  config->heartbeat_period_ms = VELAROS_DEFAULT_HEARTBEAT_PERIOD_MS;
}

int velaros_config_load(
  velaros_config_t * config, bool initialize_missing)
{
  velaros_config_t defaults;
  int8_t bridge_enabled;
  int32_t value;
  bool dirty = false;
  int ret;

  if (config == NULL) {
    return -EINVAL;
  }

  velaros_config_get_defaults(&defaults);
  *config = defaults;

  ret = property_get_bool_with_err(
    VELAROS_CONFIG_KEY_BRIDGE_ENABLED, &bridge_enabled);
  if (ret == 0) {
    config->bridge_enabled = bridge_enabled != 0;
  } else if (initialize_missing) {
    ret = property_set_bool(
      VELAROS_CONFIG_KEY_BRIDGE_ENABLED, defaults.bridge_enabled);
    if (ret < 0) {
      return ret;
    }
    dirty = true;
  }

  ret = property_get_int32_with_err(VELAROS_CONFIG_KEY_DOMAIN_ID, &value);
  if (ret == 0 && velaros_value_in_range(
      value, VELAROS_DOMAIN_ID_MIN, VELAROS_DOMAIN_ID_MAX)) {
    config->domain_id = value;
  } else if (initialize_missing) {
    ret = property_set_int32(
      VELAROS_CONFIG_KEY_DOMAIN_ID, defaults.domain_id);
    if (ret < 0) {
      return ret;
    }
    dirty = true;
  }

  ret = property_get_int32_with_err(
    VELAROS_CONFIG_KEY_PARTICIPANT_ID, &value);
  if (ret == 0 && velaros_value_in_range(
      value, VELAROS_PARTICIPANT_ID_MIN, VELAROS_PARTICIPANT_ID_MAX)) {
    config->participant_id = value;
  } else if (initialize_missing) {
    ret = property_set_int32(
      VELAROS_CONFIG_KEY_PARTICIPANT_ID, defaults.participant_id);
    if (ret < 0) {
      return ret;
    }
    dirty = true;
  }

  ret = property_get_int32_with_err(
    VELAROS_CONFIG_KEY_HEARTBEAT_PERIOD_MS, &value);
  if (ret == 0 && velaros_value_in_range(
      value, VELAROS_HEARTBEAT_PERIOD_MS_MIN,
      VELAROS_HEARTBEAT_PERIOD_MS_MAX)) {
    config->heartbeat_period_ms = value;
  } else if (initialize_missing) {
    ret = property_set_int32(
      VELAROS_CONFIG_KEY_HEARTBEAT_PERIOD_MS,
      defaults.heartbeat_period_ms);
    if (ret < 0) {
      return ret;
    }
    dirty = true;
  }

  return velaros_commit_if_needed(dirty);
}

int velaros_config_store_bridge_enabled(bool enabled)
{
  int ret = property_set_bool(VELAROS_CONFIG_KEY_BRIDGE_ENABLED, enabled);

  return ret < 0 ? ret : property_commit();
}

int velaros_config_store_domain_id(int32_t domain_id)
{
  int ret;

  if (!velaros_value_in_range(
      domain_id, VELAROS_DOMAIN_ID_MIN, VELAROS_DOMAIN_ID_MAX)) {
    return -ERANGE;
  }
  ret = property_set_int32(VELAROS_CONFIG_KEY_DOMAIN_ID, domain_id);
  return ret < 0 ? ret : property_commit();
}

int velaros_config_store_participant_id(int32_t participant_id)
{
  int ret;

  if (!velaros_value_in_range(
      participant_id, VELAROS_PARTICIPANT_ID_MIN,
      VELAROS_PARTICIPANT_ID_MAX)) {
    return -ERANGE;
  }
  ret = property_set_int32(
    VELAROS_CONFIG_KEY_PARTICIPANT_ID, participant_id);
  return ret < 0 ? ret : property_commit();
}

int velaros_config_store_heartbeat_period_ms(int32_t period_ms)
{
  int ret;

  if (!velaros_value_in_range(
      period_ms, VELAROS_HEARTBEAT_PERIOD_MS_MIN,
      VELAROS_HEARTBEAT_PERIOD_MS_MAX)) {
    return -ERANGE;
  }
  ret = property_set_int32(
    VELAROS_CONFIG_KEY_HEARTBEAT_PERIOD_MS, period_ms);
  return ret < 0 ? ret : property_commit();
}

int velaros_runtime_set_state(velaros_runtime_state_t state)
{
  if (state < VELAROS_RUNTIME_STOPPED || state > VELAROS_RUNTIME_FAULT) {
    return -ERANGE;
  }
  return property_set_int32(VELAROS_RUNTIME_KEY_STATE, state);
}

int velaros_runtime_get_state(velaros_runtime_state_t * state)
{
  int32_t value;
  int ret;

  if (state == NULL) {
    return -EINVAL;
  }
  ret = property_get_int32_with_err(VELAROS_RUNTIME_KEY_STATE, &value);
  if (ret < 0) {
    return ret;
  }
  if (value < VELAROS_RUNTIME_STOPPED || value > VELAROS_RUNTIME_FAULT) {
    return -ERANGE;
  }
  *state = (velaros_runtime_state_t)value;
  return 0;
}
