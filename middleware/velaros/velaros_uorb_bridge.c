/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/uorb_bridge.h"

#include <string.h>

velaros_uorb_to_ros_bridge_t
velaros_uorb_to_ros_bridge_get_zero_initialized(void)
{
  velaros_uorb_to_ros_bridge_t bridge;

  memset(&bridge, 0, sizeof(bridge));
  bridge.subscription_fd = -1;
  bridge.last_rcl_ret = RCL_RET_OK;
  return bridge;
}

velaros_bridge_ret_t velaros_uorb_to_ros_bridge_init(
  velaros_uorb_to_ros_bridge_t * bridge,
  const struct orb_metadata * metadata,
  rcl_publisher_t * publisher,
  void * uorb_message,
  void * ros_message,
  velaros_uorb_to_ros_convert_t convert,
  void * user_data)
{
  if (bridge == NULL || metadata == NULL || publisher == NULL ||
      uorb_message == NULL || ros_message == NULL || convert == NULL ||
      bridge->initialized || !rcl_publisher_is_valid(publisher)) {
    return VELAROS_BRIDGE_INVALID_ARGUMENT;
  }

  *bridge = velaros_uorb_to_ros_bridge_get_zero_initialized();
  bridge->subscription_fd = orb_subscribe(metadata);
  if (bridge->subscription_fd < 0) {
    return VELAROS_BRIDGE_UORB_ERROR;
  }

  bridge->metadata = metadata;
  bridge->publisher = publisher;
  bridge->uorb_message = uorb_message;
  bridge->ros_message = ros_message;
  bridge->convert = convert;
  bridge->user_data = user_data;
  bridge->initialized = true;
  return VELAROS_BRIDGE_OK;
}

velaros_bridge_ret_t velaros_uorb_to_ros_bridge_pump(
  velaros_uorb_to_ros_bridge_t * bridge)
{
  bool updated = false;

  if (bridge == NULL || !bridge->initialized) {
    return VELAROS_BRIDGE_INVALID_ARGUMENT;
  }
  if (orb_check(bridge->subscription_fd, &updated) < 0) {
    ++bridge->errors;
    return VELAROS_BRIDGE_UORB_ERROR;
  }
  if (!updated) {
    return VELAROS_BRIDGE_NO_DATA;
  }
  if (orb_copy(
      bridge->metadata, bridge->subscription_fd,
      bridge->uorb_message) < 0) {
    ++bridge->errors;
    return VELAROS_BRIDGE_UORB_ERROR;
  }
  if (!bridge->convert(
      bridge->uorb_message, bridge->ros_message, bridge->user_data)) {
    ++bridge->errors;
    return VELAROS_BRIDGE_CONVERSION_ERROR;
  }

  bridge->last_rcl_ret = rcl_publish(
    bridge->publisher, bridge->ros_message, NULL);
  if (bridge->last_rcl_ret != RCL_RET_OK) {
    ++bridge->errors;
    return VELAROS_BRIDGE_RCL_ERROR;
  }
  ++bridge->forwarded;
  return VELAROS_BRIDGE_OK;
}

velaros_bridge_ret_t velaros_uorb_to_ros_bridge_fini(
  velaros_uorb_to_ros_bridge_t * bridge)
{
  velaros_bridge_ret_t ret = VELAROS_BRIDGE_OK;

  if (bridge == NULL) {
    return VELAROS_BRIDGE_INVALID_ARGUMENT;
  }
  if (bridge->subscription_fd >= 0 &&
      orb_unsubscribe(bridge->subscription_fd) < 0) {
    ret = VELAROS_BRIDGE_UORB_ERROR;
  }
  *bridge = velaros_uorb_to_ros_bridge_get_zero_initialized();
  return ret;
}

velaros_ros_to_uorb_bridge_t
velaros_ros_to_uorb_bridge_get_zero_initialized(void)
{
  velaros_ros_to_uorb_bridge_t bridge;

  memset(&bridge, 0, sizeof(bridge));
  bridge.advertisement_fd = -1;
  bridge.last_result = VELAROS_BRIDGE_OK;
  return bridge;
}

velaros_bridge_ret_t velaros_ros_to_uorb_bridge_init(
  velaros_ros_to_uorb_bridge_t * bridge,
  const struct orb_metadata * metadata,
  void * uorb_message,
  velaros_ros_to_uorb_convert_t convert,
  void * user_data)
{
  int instance = 0;

  if (bridge == NULL || metadata == NULL || uorb_message == NULL ||
      convert == NULL || bridge->initialized) {
    return VELAROS_BRIDGE_INVALID_ARGUMENT;
  }

  *bridge = velaros_ros_to_uorb_bridge_get_zero_initialized();
  /* A control setpoint is state, not an edge-triggered event. Keep the current
   * value in uORB so a consumer which starts after the bridge can read it.
   */
  bridge->advertisement_fd = orb_advertise_multi_queue_persist(
    metadata, uorb_message, &instance, 1);
  if (bridge->advertisement_fd < 0) {
    return VELAROS_BRIDGE_UORB_ERROR;
  }

  bridge->metadata = metadata;
  bridge->uorb_message = uorb_message;
  bridge->convert = convert;
  bridge->user_data = user_data;
  bridge->initialized = true;
  return VELAROS_BRIDGE_OK;
}

void velaros_ros_to_uorb_bridge_callback(
  const void * ros_message, void * untyped_bridge)
{
  velaros_ros_to_uorb_bridge_t * bridge =
    (velaros_ros_to_uorb_bridge_t *)untyped_bridge;

  if (ros_message == NULL || bridge == NULL || !bridge->initialized) {
    if (bridge != NULL) {
      ++bridge->errors;
      bridge->last_result = VELAROS_BRIDGE_INVALID_ARGUMENT;
    }
    return;
  }
  if (!bridge->convert(
      ros_message, bridge->uorb_message, bridge->user_data)) {
    ++bridge->errors;
    bridge->last_result = VELAROS_BRIDGE_CONVERSION_ERROR;
    return;
  }
  if (orb_publish(
      bridge->metadata, bridge->advertisement_fd,
      bridge->uorb_message) < 0) {
    ++bridge->errors;
    bridge->last_result = VELAROS_BRIDGE_UORB_ERROR;
    return;
  }

  ++bridge->forwarded;
  bridge->last_result = VELAROS_BRIDGE_OK;
}

velaros_bridge_ret_t velaros_ros_to_uorb_bridge_fini(
  velaros_ros_to_uorb_bridge_t * bridge)
{
  velaros_bridge_ret_t ret = VELAROS_BRIDGE_OK;

  if (bridge == NULL) {
    return VELAROS_BRIDGE_INVALID_ARGUMENT;
  }
  if (bridge->advertisement_fd >= 0 &&
      orb_unadvertise(bridge->advertisement_fd) < 0) {
    ret = VELAROS_BRIDGE_UORB_ERROR;
  }
  *bridge = velaros_ros_to_uorb_bridge_get_zero_initialized();
  return ret;
}
