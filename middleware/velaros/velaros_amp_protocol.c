/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/amp_protocol.h"

#include <string.h>

static bool IsKnownType(uint16_t type)
{
  return type == VELAROS_AMP_HEARTBEAT ||
         type == VELAROS_AMP_MOTION_COMMAND ||
         type == VELAROS_AMP_STATUS ||
         type == VELAROS_AMP_EMERGENCY_STOP;
}

uint32_t velaros_amp_crc32(const void *data, size_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xffffffffU;

  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (unsigned int bit = 0; bit < 8U; ++bit) {
      const uint32_t mask = -(crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

bool velaros_amp_frame_init(velaros_amp_frame_t *frame,
                            uint16_t type,
                            uint16_t flags,
                            uint32_t sequence,
                            uint64_t timestamp_us,
                            const void *payload,
                            uint16_t payload_length)
{
  if (frame == NULL || sequence == 0U || !IsKnownType(type) ||
      payload_length > VELAROS_AMP_MAX_PAYLOAD ||
      (payload_length != 0U && payload == NULL)) {
    return false;
  }

  memset(frame, 0, sizeof(*frame));
  frame->magic = VELAROS_AMP_MAGIC;
  frame->version = VELAROS_AMP_VERSION;
  frame->type = type;
  frame->sequence = sequence;
  frame->timestamp_us = timestamp_us;
  frame->payload_length = payload_length;
  frame->flags = flags;
  if (payload_length != 0U) {
    memcpy(frame->payload, payload, payload_length);
  }
  frame->crc32 = velaros_amp_crc32(frame, offsetof(velaros_amp_frame_t,
                                                   crc32));
  return true;
}

bool velaros_amp_frame_valid(const velaros_amp_frame_t *frame)
{
  if (frame == NULL || frame->magic != VELAROS_AMP_MAGIC ||
      frame->version != VELAROS_AMP_VERSION || frame->sequence == 0U ||
      !IsKnownType(frame->type) ||
      frame->payload_length > VELAROS_AMP_MAX_PAYLOAD) {
    return false;
  }

  return frame->crc32 == velaros_amp_crc32(
    frame, offsetof(velaros_amp_frame_t, crc32));
}

bool velaros_amp_make_heartbeat(velaros_amp_frame_t *frame,
                                uint32_t sequence,
                                uint64_t timestamp_us,
                                uint32_t heartbeat_period_ms,
                                uint32_t capabilities)
{
  const velaros_amp_heartbeat_t heartbeat = {
    heartbeat_period_ms, capabilities};
  return velaros_amp_frame_init(frame, VELAROS_AMP_HEARTBEAT, 0U,
                                sequence, timestamp_us, &heartbeat,
                                sizeof(heartbeat));
}

bool velaros_amp_make_motion_command(velaros_amp_frame_t *frame,
                                     uint32_t sequence,
                                     uint64_t timestamp_us,
                                     const velaros_amp_motion_command_t *command)
{
  if (command == NULL) {
    return false;
  }
  const uint16_t flags =
    (command->command_flags & VELAROS_AMP_FLAG_STOP) != 0U ?
      VELAROS_AMP_FLAG_STOP : 0U;
  return velaros_amp_frame_init(frame, VELAROS_AMP_MOTION_COMMAND, flags,
                                sequence, timestamp_us, command,
                                sizeof(*command));
}

bool velaros_amp_make_emergency_stop(velaros_amp_frame_t *frame,
                                     uint32_t sequence,
                                     uint64_t timestamp_us,
                                     uint32_t reason)
{
  const velaros_amp_emergency_stop_t stop = {reason};
  return velaros_amp_frame_init(frame, VELAROS_AMP_EMERGENCY_STOP,
                                VELAROS_AMP_FLAG_STOP, sequence,
                                timestamp_us, &stop, sizeof(stop));
}

bool velaros_amp_make_status(velaros_amp_frame_t *frame,
                             uint32_t sequence,
                             uint64_t timestamp_us,
                             const velaros_amp_endpoint_t *endpoint)
{
  if (endpoint == NULL) {
    return false;
  }
  const velaros_amp_status_t status = {
    endpoint->state,
    endpoint->fault,
    endpoint->last_rx_sequence,
    endpoint->last_command_sequence,
    endpoint->last_rx_timestamp_us,
    endpoint->last_command_timestamp_us};
  return velaros_amp_frame_init(frame, VELAROS_AMP_STATUS, 0U, sequence,
                                timestamp_us, &status, sizeof(status));
}

void velaros_amp_endpoint_init(velaros_amp_endpoint_t *endpoint)
{
  if (endpoint == NULL) {
    return;
  }
  memset(endpoint, 0, sizeof(*endpoint));
  endpoint->state = VELAROS_AMP_STATE_DISCONNECTED;
  endpoint->fault = VELAROS_AMP_FAULT_NONE;
  endpoint->command_timeout_ms = VELAROS_AMP_DEFAULT_COMMAND_TIMEOUT_MS;
  endpoint->heartbeat_timeout_ms = VELAROS_AMP_DEFAULT_HEARTBEAT_TIMEOUT_MS;
}

static void StopEndpoint(velaros_amp_endpoint_t *endpoint,
                         uint32_t fault)
{
  endpoint->state = fault == VELAROS_AMP_FAULT_NONE ?
    VELAROS_AMP_STATE_EMERGENCY_STOP : VELAROS_AMP_STATE_EMERGENCY_STOP;
  endpoint->fault = fault;
  endpoint->emergency_stop = true;
  endpoint->command.linear_x_mps = 0.0F;
  endpoint->command.angular_z_rps = 0.0F;
  endpoint->command.command_flags |= VELAROS_AMP_FLAG_STOP;
}

bool velaros_amp_endpoint_receive(velaros_amp_endpoint_t *endpoint,
                                  const velaros_amp_frame_t *frame,
                                  uint64_t now_us)
{
  if (endpoint == NULL || !velaros_amp_frame_valid(frame)) {
    if (endpoint != NULL) {
      endpoint->fault = VELAROS_AMP_FAULT_BAD_FRAME;
      endpoint->state = VELAROS_AMP_STATE_FAULT;
    }
    return false;
  }

  if (endpoint->last_rx_sequence != 0U &&
      frame->sequence <= endpoint->last_rx_sequence) {
    endpoint->fault = VELAROS_AMP_FAULT_BAD_SEQUENCE;
    endpoint->state = VELAROS_AMP_STATE_FAULT;
    StopEndpoint(endpoint, VELAROS_AMP_FAULT_BAD_SEQUENCE);
    return false;
  }

  endpoint->last_rx_sequence = frame->sequence;
  endpoint->last_rx_timestamp_us = now_us;
  endpoint->peer_alive = true;

  switch (frame->type) {
    case VELAROS_AMP_HEARTBEAT: {
      if (frame->payload_length != sizeof(velaros_amp_heartbeat_t)) {
        endpoint->fault = VELAROS_AMP_FAULT_BAD_FRAME;
        return false;
      }
      velaros_amp_heartbeat_t heartbeat;
      memcpy(&heartbeat, frame->payload, sizeof(heartbeat));
      if (heartbeat.heartbeat_period_ms != 0U) {
        uint64_t timeout_ms =
          (uint64_t)heartbeat.heartbeat_period_ms * 3U;
        if (timeout_ms < VELAROS_AMP_DEFAULT_HEARTBEAT_TIMEOUT_MS) {
          timeout_ms = VELAROS_AMP_DEFAULT_HEARTBEAT_TIMEOUT_MS;
        }
        endpoint->heartbeat_timeout_ms =
          timeout_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)timeout_ms;
      }
      if (!endpoint->emergency_stop &&
          endpoint->state != VELAROS_AMP_STATE_ACTIVE) {
        endpoint->state = VELAROS_AMP_STATE_READY;
        endpoint->fault = VELAROS_AMP_FAULT_NONE;
      }
      return true;
    }

    case VELAROS_AMP_MOTION_COMMAND: {
      if (frame->payload_length != sizeof(velaros_amp_motion_command_t)) {
        endpoint->fault = VELAROS_AMP_FAULT_BAD_FRAME;
        return false;
      }
      velaros_amp_motion_command_t command;
      memcpy(&command, frame->payload, sizeof(command));
      endpoint->last_command_sequence = frame->sequence;
      endpoint->last_command_timestamp_us = now_us;
      endpoint->command_timeout_ms = command.timeout_ms != 0U ?
        command.timeout_ms : VELAROS_AMP_DEFAULT_COMMAND_TIMEOUT_MS;
      endpoint->command = command;
      if ((frame->flags & VELAROS_AMP_FLAG_STOP) != 0U ||
          (command.command_flags & VELAROS_AMP_FLAG_STOP) != 0U) {
        StopEndpoint(endpoint, VELAROS_AMP_FAULT_NONE);
      } else if (!endpoint->emergency_stop) {
        endpoint->state = VELAROS_AMP_STATE_ACTIVE;
        endpoint->fault = VELAROS_AMP_FAULT_NONE;
      }
      return true;
    }

    case VELAROS_AMP_EMERGENCY_STOP:
      if (frame->payload_length != sizeof(velaros_amp_emergency_stop_t)) {
        endpoint->fault = VELAROS_AMP_FAULT_BAD_FRAME;
        return false;
      }
      StopEndpoint(endpoint, VELAROS_AMP_FAULT_REMOTE_EMERGENCY_STOP);
      return true;

    case VELAROS_AMP_STATUS:
      return false;

    default:
      return false;
  }
}

bool velaros_amp_endpoint_tick(velaros_amp_endpoint_t *endpoint,
                               uint64_t now_us)
{
  if (endpoint == NULL) {
    return false;
  }

  if (endpoint->last_command_timestamp_us != 0U &&
      now_us > endpoint->last_command_timestamp_us &&
      now_us - endpoint->last_command_timestamp_us >
        (uint64_t)endpoint->command_timeout_ms * 1000U &&
      endpoint->state == VELAROS_AMP_STATE_ACTIVE) {
    StopEndpoint(endpoint, VELAROS_AMP_FAULT_COMMAND_TIMEOUT);
    return true;
  }

  if (endpoint->last_rx_timestamp_us != 0U &&
      now_us > endpoint->last_rx_timestamp_us &&
      now_us - endpoint->last_rx_timestamp_us >
        (uint64_t)endpoint->heartbeat_timeout_ms * 1000U) {
    endpoint->peer_alive = false;
    if (endpoint->state == VELAROS_AMP_STATE_ACTIVE ||
        endpoint->state == VELAROS_AMP_STATE_READY) {
      StopEndpoint(endpoint, VELAROS_AMP_FAULT_PEER_TIMEOUT);
      return true;
    }
  }
  return false;
}

const velaros_amp_motion_command_t *
velaros_amp_endpoint_command(const velaros_amp_endpoint_t *endpoint)
{
  return endpoint == NULL ? NULL : &endpoint->command;
}
