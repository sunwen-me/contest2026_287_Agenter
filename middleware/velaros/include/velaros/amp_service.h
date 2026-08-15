/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__AMP_SERVICE_H
#define VELAROS__AMP_SERVICE_H

#include "velaros/amp_protocol.h"
#include "velaros/uorb_topics.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
  velaros_amp_endpoint_t endpoint;
  int motion_advertisement_fd;
  int status_advertisement_fd;
  uint32_t tx_sequence;
  struct velaros_motion_command_s last_motion;
  struct velaros_amp_status_s last_status;
} velaros_amp_service_t;

bool velaros_amp_service_init(velaros_amp_service_t *service);
void velaros_amp_service_fini(velaros_amp_service_t *service);
bool velaros_amp_service_handle_frame(velaros_amp_service_t *service,
                                      const velaros_amp_frame_t *frame,
                                      uint64_t now_us);
bool velaros_amp_service_tick(velaros_amp_service_t *service,
                              uint64_t now_us);
const struct velaros_motion_command_s *
velaros_amp_service_last_motion(const velaros_amp_service_t *service);
const struct velaros_amp_status_s *
velaros_amp_service_last_status(const velaros_amp_service_t *service);

#ifdef __cplusplus
}
#endif

#endif /* VELAROS__AMP_SERVICE_H */
