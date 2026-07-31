/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "rcl/error_handling.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"

static void print_rcl_error(const char * step)
{
  fprintf(stderr, "%s failed: %s\n", step, rcl_get_error_string().str);
  rcl_reset_error();
}

int main(int argc, char * argv[])
{
  (void)argc;
  (void)argv;

  rcl_allocator_t allocator = rcl_get_default_allocator();
  rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
  rcl_context_t context = rcl_get_zero_initialized_context();
  rcl_node_t node = rcl_get_zero_initialized_node();
  rcl_node_options_t node_options = rcl_node_get_default_options();
  int result = 1;

  node_options.use_global_arguments = false;
  node_options.enable_rosout = false;

  if (rcl_init_options_init(&init_options, allocator) != RCL_RET_OK) {
    print_rcl_error("rcl_init_options_init");
    goto cleanup;
  }

  if (rcl_init(0, NULL, &init_options, &context) != RCL_RET_OK) {
    print_rcl_error("rcl_init");
    goto cleanup;
  }
  printf("rcl context init: PASS\n");

  if (rcl_node_init(
      &node, "velaros_rcl_node", "/velaros", &context, &node_options) != RCL_RET_OK) {
    print_rcl_error("rcl_node_init");
    goto cleanup;
  }

  if (strcmp(rcl_node_get_name(&node), "velaros_rcl_node") != 0 ||
      strcmp(rcl_node_get_namespace(&node), "/velaros") != 0 ||
      !rcl_node_is_valid(&node)) {
    fprintf(stderr, "rcl node identity validation failed\n");
    goto cleanup;
  }
  printf("rcl Fast DDS node create: PASS\n");

  if (rcl_node_fini(&node) != RCL_RET_OK) {
    print_rcl_error("rcl_node_fini");
    goto cleanup;
  }
  printf("rcl node fini: PASS\n");

  if (rcl_shutdown(&context) != RCL_RET_OK) {
    print_rcl_error("rcl_shutdown");
    goto cleanup;
  }
  printf("rcl shutdown: PASS\n");

  if (rcl_context_fini(&context) != RCL_RET_OK) {
    print_rcl_error("rcl_context_fini");
    goto cleanup;
  }
  printf("rcl context fini: PASS\n");

  if (rcl_init_options_fini(&init_options) != RCL_RET_OK) {
    print_rcl_error("rcl_init_options_fini");
    goto cleanup;
  }

  printf("VelaROS rcl minimal lifecycle smoke: PASS\n");
  return 0;

cleanup:
  if (node.impl != NULL) {
    const rcl_ret_t cleanup_ret = rcl_node_fini(&node);
    if (cleanup_ret != RCL_RET_OK) {
      print_rcl_error("cleanup rcl_node_fini");
    }
  }
  if (context.impl != NULL) {
    if (rcl_context_is_valid(&context)) {
      const rcl_ret_t cleanup_ret = rcl_shutdown(&context);
      if (cleanup_ret != RCL_RET_OK) {
        print_rcl_error("cleanup rcl_shutdown");
      }
    }
    const rcl_ret_t cleanup_ret = rcl_context_fini(&context);
    if (cleanup_ret != RCL_RET_OK) {
      print_rcl_error("cleanup rcl_context_fini");
    }
  }
  if (init_options.impl != NULL) {
    const rcl_ret_t cleanup_ret = rcl_init_options_fini(&init_options);
    if (cleanup_ret != RCL_RET_OK) {
      print_rcl_error("cleanup rcl_init_options_fini");
    }
  }
  return result;
}
