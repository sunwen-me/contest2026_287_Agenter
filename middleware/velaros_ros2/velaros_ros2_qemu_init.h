/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS_ROS2_QEMU_INIT_H
#define VELAROS_ROS2_QEMU_INIT_H

#include <stdlib.h>
#include <string.h>

#include "rcl/init_options.h"
#include "rmw/discovery_options.h"
#include "rmw/init_options.h"

static inline rcl_ret_t velaros_configure_qemu_interop(
  rcl_init_options_t * init_options)
{
  static const char host_peer[] = "10.0.2.2";
  rmw_init_options_t * rmw_options =
    rcl_init_options_get_rmw_init_options(init_options);

  if (rmw_options == NULL) {
    return RCL_RET_ERROR;
  }
  if (rmw_discovery_options_init(
      &rmw_options->discovery_options, 1,
      &rmw_options->allocator) != RMW_RET_OK) {
    return RCL_RET_ERROR;
  }

  rmw_options->discovery_options.automatic_discovery_range =
    RMW_AUTOMATIC_DISCOVERY_RANGE_SUBNET;
  memcpy(
    rmw_options->discovery_options.static_peers[0].peer_address,
    host_peer, sizeof(host_peer));

  if (setenv("VELAROS_DDS_QEMU_INTEROP", "1", 1) != 0) {
    return RCL_RET_ERROR;
  }
  return RCL_RET_OK;
}

#endif
