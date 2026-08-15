/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/amp_service.h"

#include <string.h>

static bool PublishMotion(velaros_amp_service_t *service)
{
  const velaros_amp_motion_command_t *command =
    velaros_amp_endpoint_command(&service->endpoint);
  struct velaros_motion_command_s motion = {
    .timestamp = service->endpoint.last_command_timestamp_us,
    .linear_x_mps = 0.0F,
    .angular_z_rps = 0.0F,
    .timeout_ms = service->endpoint.command_timeout_ms,
    .sequence = service->endpoint.last_command_sequence};

  if (!service->endpoint.emergency_stop &&
      service->endpoint.state == VELAROS_AMP_STATE_ACTIVE &&
      command != NULL) {
    motion.linear_x_mps = command->linear_x_mps;
    motion.angular_z_rps = command->angular_z_rps;
  }

  if (orb_publish(ORB_ID(velaros_motion_command),
                  service->motion_advertisement_fd, &motion) < 0) {
    return false;
  }
  service->last_motion = motion;
  return true;
}

static bool PublishStatus(velaros_amp_service_t *service,
                          uint64_t timestamp_us)
{
  struct velaros_amp_status_s status = {
    .timestamp = timestamp_us,
    .state = service->endpoint.state,
    .fault = service->endpoint.fault,
    .last_rx_sequence = service->endpoint.last_rx_sequence,
    .last_command_sequence = service->endpoint.last_command_sequence,
    .peer_alive = service->endpoint.peer_alive ? 1U : 0U,
    .emergency_stop = service->endpoint.emergency_stop ? 1U : 0U};

  if (orb_publish(ORB_ID(velaros_amp_status),
                  service->status_advertisement_fd, &status) < 0) {
    return false;
  }
  service->last_status = status;
  return true;
}

bool velaros_amp_service_init(velaros_amp_service_t *service)
{
  if (service == NULL) {
    return false;
  }

  memset(service, 0, sizeof(*service));
  service->motion_advertisement_fd = -1;
  service->status_advertisement_fd = -1;
  velaros_amp_endpoint_init(&service->endpoint);

  int motion_instance = 0;
  struct velaros_motion_command_s zero_motion = {};
  service->motion_advertisement_fd = orb_advertise_multi_queue_persist(
    ORB_ID(velaros_motion_command), &zero_motion, &motion_instance, 1);
  if (service->motion_advertisement_fd < 0) {
    velaros_amp_service_fini(service);
    return false;
  }

  int status_instance = 0;
  struct velaros_amp_status_s zero_status = {};
  service->status_advertisement_fd = orb_advertise_multi_queue_persist(
    ORB_ID(velaros_amp_status), &zero_status, &status_instance, 1);
  if (service->status_advertisement_fd < 0) {
    velaros_amp_service_fini(service);
    return false;
  }

  return PublishStatus(service, 0U);
}

void velaros_amp_service_fini(velaros_amp_service_t *service)
{
  if (service == NULL) {
    return;
  }
  if (service->motion_advertisement_fd >= 0) {
    (void)orb_unadvertise(service->motion_advertisement_fd);
  }
  if (service->status_advertisement_fd >= 0) {
    (void)orb_unadvertise(service->status_advertisement_fd);
  }
  service->motion_advertisement_fd = -1;
  service->status_advertisement_fd = -1;
}

bool velaros_amp_service_handle_frame(velaros_amp_service_t *service,
                                      const velaros_amp_frame_t *frame,
                                      uint64_t now_us)
{
  if (service == NULL || !velaros_amp_endpoint_receive(
                           &service->endpoint, frame, now_us)) {
    if (service != NULL) {
      (void)PublishMotion(service);
      (void)PublishStatus(service, now_us);
    }
    return false;
  }

  if (frame->type == VELAROS_AMP_MOTION_COMMAND ||
      frame->type == VELAROS_AMP_EMERGENCY_STOP) {
    if (!PublishMotion(service)) {
      return false;
    }
  }
  return PublishStatus(service, now_us);
}

bool velaros_amp_service_tick(velaros_amp_service_t *service,
                              uint64_t now_us)
{
  if (service == NULL) {
    return false;
  }
  const bool stopped = velaros_amp_endpoint_tick(&service->endpoint, now_us);
  if (stopped && !PublishMotion(service)) {
    return false;
  }
  return PublishStatus(service, now_us);
}

const struct velaros_motion_command_s *
velaros_amp_service_last_motion(const velaros_amp_service_t *service)
{
  return service == NULL ? NULL : &service->last_motion;
}

const struct velaros_amp_status_s *
velaros_amp_service_last_status(const velaros_amp_service_t *service)
{
  return service == NULL ? NULL : &service->last_status;
}
