/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdio>
#include <cstring>

#include "rcutils/allocator.h"
#include "rcutils/error_handling.h"
#include "rcutils/types/string_array.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/rmw.h"

static void print_rmw_error(const char * step)
{
  const rmw_error_string_t error = rmw_get_error_string();
  std::printf("%s failed: %s\n", step, error.str);
  rmw_reset_error();
}

#ifdef __NuttX__
extern "C"
#endif
int main(int argc, char * argv[])
{
  (void)argc;
  (void)argv;
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_init_options_t options = rmw_get_zero_initialized_init_options();
  rmw_context_t context = rmw_get_zero_initialized_context();
  rmw_node_t * node = nullptr;
  rcutils_string_array_t names = rcutils_get_zero_initialized_string_array();
  rcutils_string_array_t namespaces = rcutils_get_zero_initialized_string_array();
  bool options_initialized = false;
  bool context_initialized = false;
  bool graph_arrays_initialized = false;
  bool node_found = false;
  int result = 1;

  if (rmw_init_options_init(&options, allocator) != RMW_RET_OK) {
    print_rmw_error("rmw_init_options_init");
    goto cleanup;
  }
  options_initialized = true;

  if (rmw_enclave_options_copy("/", &allocator, &options.enclave) != RMW_RET_OK) {
    print_rmw_error("rmw_enclave_options_copy");
    goto cleanup;
  }

  if (rmw_init(&options, &context) != RMW_RET_OK) {
    print_rmw_error("rmw_init");
    goto cleanup;
  }
  context_initialized = true;

  if (std::strcmp(rmw_get_implementation_identifier(), "rmw_fastrtps_cpp") != 0) {
    std::printf("unexpected RMW implementation: %s\n",
      rmw_get_implementation_identifier());
    goto cleanup;
  }
  std::printf("rmw_fastrtps context init: PASS\n");

  node = rmw_create_node(&context, "velaros_rmw_smoke", "/velaros");
  if (node == nullptr) {
    print_rmw_error("rmw_create_node");
    goto cleanup;
  }
  std::printf("rmw_fastrtps Fast DDS node create: PASS\n");

  if (rmw_get_node_names(node, &names, &namespaces) != RMW_RET_OK) {
    print_rmw_error("rmw_get_node_names");
    goto cleanup;
  }
  graph_arrays_initialized = true;
  for (size_t i = 0; i < names.size && i < namespaces.size; ++i) {
    if (names.data[i] != nullptr && namespaces.data[i] != nullptr &&
      std::strcmp(names.data[i], "velaros_rmw_smoke") == 0 &&
      std::strcmp(namespaces.data[i], "/velaros") == 0)
    {
      node_found = true;
      break;
    }
  }
  if (!node_found) {
    std::printf("local node missing from ROS graph\n");
    goto cleanup;
  }
  std::printf("rmw_fastrtps ROS graph query: PASS\n");
  result = 0;

cleanup:
  if (graph_arrays_initialized) {
    std::printf("rmw_fastrtps graph result cleanup: START\n");
    if (rcutils_string_array_fini(&names) != RCUTILS_RET_OK) {
      std::printf("node names string array fini failed\n");
      result = 1;
    }
    if (rcutils_string_array_fini(&namespaces) != RCUTILS_RET_OK) {
      std::printf("node namespaces string array fini failed\n");
      result = 1;
    }
    std::printf("rmw_fastrtps graph result cleanup: PASS\n");
  }
  if (node != nullptr) {
    std::printf("rmw_fastrtps node destroy: START\n");
    if (rmw_destroy_node(node) != RMW_RET_OK) {
      print_rmw_error("rmw_destroy_node");
      result = 1;
    } else {
      std::printf("rmw_fastrtps node destroy: PASS\n");
    }
  }
  if (context_initialized) {
    std::printf("rmw_fastrtps context shutdown: START\n");
    if (rmw_shutdown(&context) != RMW_RET_OK) {
      print_rmw_error("rmw_shutdown");
      result = 1;
    } else {
      std::printf("rmw_fastrtps context shutdown: PASS\n");
    }
    std::printf("rmw_fastrtps context fini: START\n");
    if (rmw_context_fini(&context) != RMW_RET_OK) {
      print_rmw_error("rmw_context_fini");
      result = 1;
    } else {
      std::printf("rmw_fastrtps context fini: PASS\n");
    }
  }
  if (options_initialized) {
    std::printf("rmw_fastrtps init options fini: START\n");
    if (rmw_init_options_fini(&options) != RMW_RET_OK) {
      print_rmw_error("rmw_init_options_fini");
      result = 1;
    } else {
      std::printf("rmw_fastrtps init options fini: PASS\n");
    }
  }

  if (result == 0) {
    std::printf("VelaROS rmw_fastrtps lifecycle smoke: PASS\n");
  }
  return result;
}
