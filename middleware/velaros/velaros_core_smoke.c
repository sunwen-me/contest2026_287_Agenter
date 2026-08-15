// SPDX-License-Identifier: Apache-2.0

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include <rcutils/allocator.h>
#include <rmw/ret_types.h>
#include <rmw/validate_node_name.h>

#ifdef CONFIG_VELAROS_FASTRTPS_TYPESUPPORT
#  include <rosidl_dynamic_typesupport_fastrtps/identifier.h>
#endif

int main(int argc, FAR char *argv[])
{
  rcutils_allocator_t allocator;
  int validation_result = -1;
  rmw_ret_t ret;

  (void)argc;
  (void)argv;

  allocator = rcutils_get_default_allocator();
  if (!rcutils_allocator_is_valid(&allocator))
    {
      fprintf(stderr, "VelaROS core smoke: invalid rcutils allocator\n");
      return 1;
    }

  ret = rmw_validate_node_name("velaros", &validation_result, NULL);
  if (ret != RMW_RET_OK || validation_result != RMW_NODE_NAME_VALID)
    {
      fprintf(stderr,
              "VelaROS core smoke: rmw validation failed (%d, %d)\n",
              (int)ret, validation_result);
      return 2;
    }

  printf("VelaROS ROS 2 Lyrical core smoke: PASS\n");
  printf("rcutils allocator: PASS\n");
  printf("rmw node-name validation: PASS\n");
#ifdef CONFIG_VELAROS_FASTRTPS_TYPESUPPORT
  if (strcmp(fastdds_serialization_support_library_identifier, "fastcdr") != 0)
    {
      fprintf(stderr, "VelaROS core smoke: Fast DDS typesupport mismatch\n");
      return 3;
    }

  printf("Fast DDS dynamic typesupport identifier: PASS\n");
#endif
  return 0;
}
