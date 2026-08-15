/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * No-board AMP smoke. It validates the fixed wire frame, the openvela-side
 * endpoint state machine, the persistent motion-command uORB boundary, and
 * timeout-to-stop behavior. It is not a K1 RPMsg, hart, IRQ, or cache test.
 */

#include "velaros/amp_service.h"

#include <stdio.h>

static int RunAmpSmoke(void)
{
  velaros_amp_service_t service;
  velaros_amp_frame_t frame;
  velaros_amp_frame_t status_frame;
  const uint64_t command_time = 1000000U;
  const uint64_t heartbeat_time = 1050000U;
  const velaros_amp_motion_command_t command = {
    .linear_x_mps = 0.35F,
    .angular_z_rps = -0.20F,
    .timeout_ms = 250U,
    .command_flags = 0U};

  if (!velaros_amp_service_init(&service)) {
    fprintf(stderr, "VelaROS AMP smoke: service init failed\n");
    return 1;
  }

  const bool command_frame = velaros_amp_make_motion_command(
    &frame, 1U, command_time, &command);
  const bool command_accepted = command_frame &&
    velaros_amp_service_handle_frame(&service, &frame, command_time);
  const struct velaros_motion_command_s *motion =
    velaros_amp_service_last_motion(&service);
  const bool command_mapped = command_accepted && motion != NULL &&
    motion->sequence == 1U && motion->linear_x_mps == command.linear_x_mps &&
    motion->angular_z_rps == command.angular_z_rps;

  const bool heartbeat_frame = velaros_amp_make_heartbeat(
    &frame, 2U, heartbeat_time, 250U, 1U);
  const bool heartbeat_accepted = heartbeat_frame &&
    velaros_amp_service_handle_frame(&service, &frame, heartbeat_time);
  const bool status_frame_created = velaros_amp_make_status(
    &status_frame, 1U, heartbeat_time, &service.endpoint) &&
    velaros_amp_frame_valid(&status_frame);

  const bool timeout_transition = velaros_amp_service_tick(
    &service, command_time + 300000U);
  motion = velaros_amp_service_last_motion(&service);
  const struct velaros_amp_status_s *status =
    velaros_amp_service_last_status(&service);
  const bool timeout_stop = timeout_transition && motion != NULL &&
    status != NULL && motion->linear_x_mps == 0.0F &&
    motion->angular_z_rps == 0.0F &&
    status->fault == VELAROS_AMP_FAULT_COMMAND_TIMEOUT &&
    status->emergency_stop != 0U;

  velaros_amp_service_fini(&service);

  if (!command_mapped || !heartbeat_accepted || !status_frame_created ||
      !timeout_stop) {
    fprintf(stderr,
            "VelaROS AMP smoke: FAIL command=%d heartbeat=%d status=%d "
            "timeout_stop=%d\n",
            command_mapped ? 1 : 0, heartbeat_accepted ? 1 : 0,
            status_frame_created ? 1 : 0, timeout_stop ? 1 : 0);
    return 1;
  }

  printf("VelaROS AMP smoke: PASS command=1 heartbeat=1 status=1 "
         "timeout_stop=1\n");
  return 0;
}

int main(int argc, char *argv[])
{
#if defined(__NuttX__)
  (void)argc;
  (void)argv;
#endif
  return RunAmpSmoke();
}
