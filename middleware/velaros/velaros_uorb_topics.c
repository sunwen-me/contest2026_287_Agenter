/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/uorb_topics.h"

#include <inttypes.h>

#ifdef CONFIG_DEBUG_UORB
static const char velaros_control_setpoint_format[] =
  "timestamp:%" PRIu64 ",value:%f";
#endif

ORB_DEFINE(
  velaros_control_setpoint,
  struct velaros_control_setpoint_s,
  velaros_control_setpoint_format);

#ifdef CONFIG_DEBUG_UORB
static const char velaros_motion_command_format[] =
  "timestamp:%" PRIu64 ",linear_x_mps:%f,angular_z_rps:%f,"
  "timeout_ms:%" PRIu32 ",sequence:%" PRIu32;
#endif

ORB_DEFINE(
  velaros_motion_command,
  struct velaros_motion_command_s,
  velaros_motion_command_format);

#ifdef CONFIG_DEBUG_UORB
static const char velaros_amp_status_format[] =
  "timestamp:%" PRIu64 ",state:%" PRIu32 ",fault:%" PRIu32
  ",last_rx_sequence:%" PRIu32 ",last_command_sequence:%" PRIu32
  ",peer_alive:%" PRIu8 ",emergency_stop:%" PRIu8;
#endif

ORB_DEFINE(
  velaros_amp_status,
  struct velaros_amp_status_s,
  velaros_amp_status_format);

#ifdef CONFIG_DEBUG_UORB
static const char velaros_buffer_transfer_format[] =
  "timestamp:%" PRIu64 ",pool_cookie:%" PRIu64 ",slot:%" PRIu32
  ",generation:%" PRIu32 ",lease:%" PRIu32 ",owner:%" PRIu32
  ",length:%" PRIu32 ",capacity:%" PRIu32 ",flags:%" PRIu32;
#endif

ORB_DEFINE(
  velaros_buffer_transfer,
  struct velaros_buffer_transfer_s,
  velaros_buffer_transfer_format);
