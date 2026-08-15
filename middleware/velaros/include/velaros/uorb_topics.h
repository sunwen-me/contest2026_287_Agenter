/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__UORB_TOPICS_H
#define VELAROS__UORB_TOPICS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

#include "uORB/uORB.h"
#include "velaros/buffer_pool.h"

struct velaros_control_setpoint_s
{
  uint64_t timestamp;
  double value;
};

ORB_DECLARE(velaros_control_setpoint);

/* Shared control boundary for both ROS /cmd_vel and product Actions. */
struct velaros_motion_command_s
{
  uint64_t timestamp;
  float linear_x_mps;
  float angular_z_rps;
  uint32_t timeout_ms;
  uint32_t sequence;
};

ORB_DECLARE(velaros_motion_command);

/* AMP peer state and fault boundary. The payload is intentionally small so
 * Linux/openvela status can use the same uORB/RPMsg path as control. */
struct velaros_amp_status_s
{
  uint64_t timestamp;
  uint32_t state;
  uint32_t fault;
  uint32_t last_rx_sequence;
  uint32_t last_command_sequence;
  uint8_t peer_alive;
  uint8_t emergency_stop;
  uint16_t reserved;
};

ORB_DECLARE(velaros_amp_status);

/* Payload stays in the fixed VelaROS pool; uORB copies only this bounded
 * descriptor between local tasks (and later through uORB/RPMsg for AMP).
 */
struct velaros_buffer_transfer_s
{
  uint64_t timestamp;
  velaros_buffer_descriptor_t descriptor;
};

ORB_DECLARE(velaros_buffer_transfer);

#ifdef __cplusplus
}
#endif

#endif  /* VELAROS__UORB_TOPICS_H */
