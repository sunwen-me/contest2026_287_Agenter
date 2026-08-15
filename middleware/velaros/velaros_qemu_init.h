/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS_QEMU_INIT_H
#define VELAROS_QEMU_INIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rcl/init_options.h"
#include "rmw/discovery_options.h"
#include "rmw/init_options.h"
#ifdef CONFIG_VELAROS_PLATFORM_CONFIG
#  include "velaros/config.h"
#endif

static inline int velaros_parse_participant_id(int argc, char * argv[])
{
  char * end = NULL;
  long participant_id;

  if (argc < 3) {
#ifdef CONFIG_VELAROS_PLATFORM_CONFIG
    velaros_config_t config;

    if (velaros_config_load(&config, false) == 0) {
      return config.participant_id;
    }
#endif
    return 0;
  }

  participant_id = strtol(argv[2], &end, 10);
  if (end == argv[2] || *end != '\0' || participant_id < 0 ||
      participant_id > 119) {
    fprintf(stderr, "usage: %s [1..1000] [participant-id 0..119]\n", argv[0]);
    return -1;
  }

  return (int)participant_id;
}

static inline rcl_ret_t velaros_configure_qemu_interop(
  rcl_init_options_t * init_options, int participant_id)
{
  char participant_id_text[12];
  rmw_init_options_t * rmw_options =
    rcl_init_options_get_rmw_init_options(init_options);

  if (rmw_options == NULL) {
    return RCL_RET_ERROR;
  }
  if (rmw_discovery_options_init(
      &rmw_options->discovery_options, 0,
      &rmw_options->allocator) != RMW_RET_OK) {
    return RCL_RET_ERROR;
  }

  rmw_options->discovery_options.automatic_discovery_range =
    RMW_AUTOMATIC_DISCOVERY_RANGE_SYSTEM_DEFAULT;

#ifdef CONFIG_VELAROS_PLATFORM_CONFIG
  {
    velaros_config_t config;

    if (velaros_config_load(&config, false) < 0 ||
        rcl_init_options_set_domain_id(
          init_options, (size_t)config.domain_id) != RCL_RET_OK) {
      return RCL_RET_ERROR;
    }
  }
#endif

  if (snprintf(
      participant_id_text, sizeof(participant_id_text), "%d", participant_id) < 0 ||
      setenv("VELAROS_DDS_QEMU_INTEROP", "1", 1) != 0 ||
      setenv("VELAROS_DDS_QEMU_PARTICIPANT_ID", participant_id_text, 1) != 0) {
    return RCL_RET_ERROR;
  }
  return RCL_RET_OK;
}

#endif
