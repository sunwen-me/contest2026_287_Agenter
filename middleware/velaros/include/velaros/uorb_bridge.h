/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__UORB_BRIDGE_H
#define VELAROS__UORB_BRIDGE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stddef.h>

#include "rcl/publisher.h"
#include "rcl/types.h"
#include "uORB/uORB.h"

/* The bridge deliberately has no runtime type registry. Every mapping is a
 * compile-time callback selected by the application.
 */
typedef bool (*velaros_uorb_to_ros_convert_t)(
  const void * uorb_message, void * ros_message, void * user_data);

typedef bool (*velaros_ros_to_uorb_convert_t)(
  const void * ros_message, void * uorb_message, void * user_data);

typedef enum velaros_bridge_ret_e
{
  VELAROS_BRIDGE_OK = 0,
  VELAROS_BRIDGE_NO_DATA = 1,
  VELAROS_BRIDGE_INVALID_ARGUMENT = -1,
  VELAROS_BRIDGE_UORB_ERROR = -2,
  VELAROS_BRIDGE_CONVERSION_ERROR = -3,
  VELAROS_BRIDGE_RCL_ERROR = -4
} velaros_bridge_ret_t;

typedef struct velaros_uorb_to_ros_bridge_s
{
  const struct orb_metadata * metadata;
  int subscription_fd;
  rcl_publisher_t * publisher;
  void * uorb_message;
  void * ros_message;
  velaros_uorb_to_ros_convert_t convert;
  void * user_data;
  size_t forwarded;
  size_t errors;
  rcl_ret_t last_rcl_ret;
  bool initialized;
} velaros_uorb_to_ros_bridge_t;

typedef struct velaros_ros_to_uorb_bridge_s
{
  const struct orb_metadata * metadata;
  int advertisement_fd;
  void * uorb_message;
  velaros_ros_to_uorb_convert_t convert;
  void * user_data;
  size_t forwarded;
  size_t errors;
  velaros_bridge_ret_t last_result;
  bool initialized;
} velaros_ros_to_uorb_bridge_t;

velaros_uorb_to_ros_bridge_t
velaros_uorb_to_ros_bridge_get_zero_initialized(void);

velaros_bridge_ret_t velaros_uorb_to_ros_bridge_init(
  velaros_uorb_to_ros_bridge_t * bridge,
  const struct orb_metadata * metadata,
  rcl_publisher_t * publisher,
  void * uorb_message,
  void * ros_message,
  velaros_uorb_to_ros_convert_t convert,
  void * user_data);

/* Non-blocking: copy and publish at most the newest pending uORB sample. */
velaros_bridge_ret_t velaros_uorb_to_ros_bridge_pump(
  velaros_uorb_to_ros_bridge_t * bridge);

velaros_bridge_ret_t velaros_uorb_to_ros_bridge_fini(
  velaros_uorb_to_ros_bridge_t * bridge);

velaros_ros_to_uorb_bridge_t
velaros_ros_to_uorb_bridge_get_zero_initialized(void);

velaros_bridge_ret_t velaros_ros_to_uorb_bridge_init(
  velaros_ros_to_uorb_bridge_t * bridge,
  const struct orb_metadata * metadata,
  void * uorb_message,
  velaros_ros_to_uorb_convert_t convert,
  void * user_data);

/* Signature matches velaros_subscription_callback_t. */
void velaros_ros_to_uorb_bridge_callback(
  const void * ros_message, void * bridge);

velaros_bridge_ret_t velaros_ros_to_uorb_bridge_fini(
  velaros_ros_to_uorb_bridge_t * bridge);

#ifdef __cplusplus
}
#endif

#endif  /* VELAROS__UORB_BRIDGE_H */
