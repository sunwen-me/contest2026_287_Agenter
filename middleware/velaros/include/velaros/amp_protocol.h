/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__AMP_PROTOCOL_H
#define VELAROS__AMP_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define VELAROS_AMP_MAGIC 0x56414d50U /* "VAMP" */
#define VELAROS_AMP_VERSION 1U
#define VELAROS_AMP_MAX_PAYLOAD 64U
#define VELAROS_AMP_DEFAULT_COMMAND_TIMEOUT_MS 250U
#define VELAROS_AMP_DEFAULT_HEARTBEAT_TIMEOUT_MS 1000U

enum velaros_amp_message_type
{
  VELAROS_AMP_HEARTBEAT = 1,
  VELAROS_AMP_MOTION_COMMAND = 2,
  VELAROS_AMP_STATUS = 3,
  VELAROS_AMP_EMERGENCY_STOP = 4
};

enum velaros_amp_message_flags
{
  VELAROS_AMP_FLAG_ACK_REQUEST = 1U,
  VELAROS_AMP_FLAG_STOP = 2U
};

enum velaros_amp_state
{
  VELAROS_AMP_STATE_DISCONNECTED = 0,
  VELAROS_AMP_STATE_READY = 1,
  VELAROS_AMP_STATE_ACTIVE = 2,
  VELAROS_AMP_STATE_EMERGENCY_STOP = 3,
  VELAROS_AMP_STATE_FAULT = 4
};

enum velaros_amp_fault
{
  VELAROS_AMP_FAULT_NONE = 0,
  VELAROS_AMP_FAULT_BAD_FRAME = 1,
  VELAROS_AMP_FAULT_BAD_SEQUENCE = 2,
  VELAROS_AMP_FAULT_COMMAND_TIMEOUT = 3,
  VELAROS_AMP_FAULT_PEER_TIMEOUT = 4,
  VELAROS_AMP_FAULT_REMOTE_EMERGENCY_STOP = 5
};

typedef struct
{
  float linear_x_mps;
  float angular_z_rps;
  uint32_t timeout_ms;
  uint32_t command_flags;
} velaros_amp_motion_command_t;

typedef struct
{
  uint32_t heartbeat_period_ms;
  uint32_t capabilities;
} velaros_amp_heartbeat_t;

typedef struct
{
  uint32_t state;
  uint32_t fault;
  uint32_t last_rx_sequence;
  uint32_t last_command_sequence;
  uint64_t last_rx_timestamp_us;
  uint64_t last_command_timestamp_us;
} velaros_amp_status_t;

typedef struct
{
  uint32_t reason;
} velaros_amp_emergency_stop_t;

/* The wire format is packed little-endian. K1 and the Linux gateway are
 * little-endian targets; the transport replacement must preserve this frame
 * byte-for-byte when it is moved to RPMsg/OpenAMP.
 */
typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t sequence;
  uint64_t timestamp_us;
  uint16_t payload_length;
  uint16_t flags;
  uint8_t payload[VELAROS_AMP_MAX_PAYLOAD];
  uint32_t crc32;
} velaros_amp_frame_t;

typedef struct
{
  uint32_t state;
  uint32_t fault;
  uint32_t last_rx_sequence;
  uint32_t last_command_sequence;
  uint64_t last_rx_timestamp_us;
  uint64_t last_command_timestamp_us;
  uint32_t command_timeout_ms;
  uint32_t heartbeat_timeout_ms;
  bool peer_alive;
  bool emergency_stop;
  velaros_amp_motion_command_t command;
} velaros_amp_endpoint_t;

_Static_assert(sizeof(velaros_amp_frame_t) == 92,
               "VelaROS AMP frame layout changed");

uint32_t velaros_amp_crc32(const void *data, size_t length);

bool velaros_amp_frame_init(velaros_amp_frame_t *frame,
                            uint16_t type,
                            uint16_t flags,
                            uint32_t sequence,
                            uint64_t timestamp_us,
                            const void *payload,
                            uint16_t payload_length);

bool velaros_amp_frame_valid(const velaros_amp_frame_t *frame);

bool velaros_amp_make_heartbeat(velaros_amp_frame_t *frame,
                                uint32_t sequence,
                                uint64_t timestamp_us,
                                uint32_t heartbeat_period_ms,
                                uint32_t capabilities);

bool velaros_amp_make_motion_command(velaros_amp_frame_t *frame,
                                     uint32_t sequence,
                                     uint64_t timestamp_us,
                                     const velaros_amp_motion_command_t *command);

bool velaros_amp_make_emergency_stop(velaros_amp_frame_t *frame,
                                     uint32_t sequence,
                                     uint64_t timestamp_us,
                                     uint32_t reason);

bool velaros_amp_make_status(velaros_amp_frame_t *frame,
                             uint32_t sequence,
                             uint64_t timestamp_us,
                             const velaros_amp_endpoint_t *endpoint);

void velaros_amp_endpoint_init(velaros_amp_endpoint_t *endpoint);

bool velaros_amp_endpoint_receive(velaros_amp_endpoint_t *endpoint,
                                  const velaros_amp_frame_t *frame,
                                  uint64_t now_us);

bool velaros_amp_endpoint_tick(velaros_amp_endpoint_t *endpoint,
                               uint64_t now_us);

const velaros_amp_motion_command_t *
velaros_amp_endpoint_command(const velaros_amp_endpoint_t *endpoint);

#ifdef __cplusplus
}
#endif

#endif /* VELAROS__AMP_PROTOCOL_H */
