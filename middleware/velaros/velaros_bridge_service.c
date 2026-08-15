/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "rcl/error_handling.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"
#include "rcl/service.h"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_runtime_c/string_functions.h"
#include "rosidl_typesupport_interface/macros.h"
#include "std_srvs/srv/detail/set_bool__functions.h"
#include "velaros/config.h"
#include "velaros/executor.h"
#include "velaros_qemu_init.h"

extern const rosidl_service_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c, std_srvs, srv, SetBool)(void);

#define VELAROS_DEFAULT_REQUEST_COUNT 1

typedef struct velaros_service_state_s
{
  int handled;
} velaros_service_state_t;

static void print_rcl_error(const char * step)
{
  fprintf(stderr, "%s failed: %s\n", step, rcl_get_error_string().str);
  rcl_reset_error();
}

static int parse_request_count(int argc, char * argv[])
{
  char * end = NULL;
  long count;

  if (argc < 2) {
    return VELAROS_DEFAULT_REQUEST_COUNT;
  }
  count = strtol(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0' || count < 0 || count > 1000) {
    fprintf(
      stderr,
      "usage: velaros_bridge_service [requests 0..1000] "
      "[participant-id 0..119]\n");
    return -1;
  }
  return (int)count;
}

static rcl_ret_t set_bridge_callback(
  const void * untyped_request,
  void * untyped_response,
  void * user_data)
{
  const std_srvs__srv__SetBool_Request * request = untyped_request;
  std_srvs__srv__SetBool_Response * response = untyped_response;
  velaros_service_state_t * state = user_data;
  const int ret = velaros_config_store_bridge_enabled(request->data);
  const char * message;

  response->success = ret == 0;
  if (ret == 0) {
    message = request->data ? "VelaROS bridge enabled" : "VelaROS bridge disabled";
  } else {
    message = "openVela KVDB update failed";
  }
  if (!rosidl_runtime_c__String__assign(&response->message, message)) {
    RCL_SET_ERROR_MSG("SetBool response string allocation failed");
    return RCL_RET_BAD_ALLOC;
  }
  ++state->handled;
  printf(
    "VelaROS SERVICE REQUEST: bridge=%s result=%s\n",
    request->data ? "enabled" : "disabled",
    response->success ? "success" : "failure");
  return RCL_RET_OK;
}

int main(int argc, char * argv[])
{
  const int request_count = parse_request_count(argc, argv);
  const int participant_id = velaros_parse_participant_id(argc, argv);
  const rosidl_service_type_support_t * type_support =
    ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
      rosidl_typesupport_fastrtps_c, std_srvs, srv, SetBool)();
  rcl_allocator_t allocator = rcl_get_default_allocator();
  rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
  rcl_context_t context = rcl_get_zero_initialized_context();
  rcl_node_t node = rcl_get_zero_initialized_node();
  rcl_service_t service = rcl_get_zero_initialized_service();
  rcl_node_options_t node_options = rcl_node_get_default_options();
  rcl_service_options_t service_options = rcl_service_get_default_options();
  velaros_executor_t executor = velaros_executor_get_zero_initialized();
  std_srvs__srv__SetBool_Request request;
  std_srvs__srv__SetBool_Response response;
  velaros_service_state_t state = {0};
  bool request_initialized = false;
  bool response_initialized = false;
  int result = 1;

  if (request_count < 0 || participant_id < 0 || type_support == NULL) {
    return 1;
  }
  node_options.use_global_arguments = false;
  node_options.enable_rosout = false;

  if (rcl_init_options_init(&init_options, allocator) != RCL_RET_OK) {
    print_rcl_error("rcl_init_options_init");
    goto cleanup;
  }
  if (velaros_configure_qemu_interop(&init_options, participant_id) != RCL_RET_OK) {
    print_rcl_error("velaros_configure_qemu_interop");
    goto cleanup;
  }
  if (rcl_init(0, NULL, &init_options, &context) != RCL_RET_OK ||
      rcl_node_init(
        &node, "velaros_runtime", "/velaros", &context,
        &node_options) != RCL_RET_OK ||
      rcl_service_init(
        &service, &node, type_support, "/velaros/runtime/set_bridge",
        &service_options) != RCL_RET_OK) {
    print_rcl_error("VelaROS service init");
    goto cleanup;
  }
  if (!std_srvs__srv__SetBool_Request__init(&request)) {
    fprintf(stderr, "SetBool request initialization failed\n");
    goto cleanup;
  }
  request_initialized = true;
  if (!std_srvs__srv__SetBool_Response__init(&response)) {
    fprintf(stderr, "SetBool response initialization failed\n");
    goto cleanup;
  }
  response_initialized = true;
  if (velaros_executor_init(
      &executor, &context, 0, 0, 0, 1, allocator) != RCL_RET_OK ||
      velaros_executor_add_service(
        &executor, &service, &request, &response,
        set_bridge_callback, &state) != RCL_RET_OK) {
    print_rcl_error("VelaROS service executor init");
    goto cleanup;
  }

  printf(
    "VelaROS service ready: /velaros/runtime/set_bridge (%s)\n",
    request_count == 0 ? "continuous" : "bounded");
  while (request_count == 0 || state.handled < request_count) {
    const rcl_ret_t spin_ret = velaros_executor_spin_once(
      &executor, RCL_MS_TO_NS(1000));
    if (spin_ret == RCL_RET_TIMEOUT) {
      continue;
    }
    if (spin_ret != RCL_RET_OK) {
      print_rcl_error("velaros_executor_spin_once");
      goto cleanup;
    }
  }
  result = 0;

cleanup:
  if (velaros_executor_fini(&executor) != RCL_RET_OK) {
    print_rcl_error("velaros_executor_fini");
    result = 1;
  }
  if (response_initialized) {
    std_srvs__srv__SetBool_Response__fini(&response);
  }
  if (request_initialized) {
    std_srvs__srv__SetBool_Request__fini(&request);
  }
  if (service.impl != NULL &&
      rcl_service_fini(&service, &node) != RCL_RET_OK) {
    print_rcl_error("rcl_service_fini");
    result = 1;
  }
  if (node.impl != NULL && rcl_node_fini(&node) != RCL_RET_OK) {
    print_rcl_error("rcl_node_fini");
    result = 1;
  }
  if (context.impl != NULL) {
    if (rcl_context_is_valid(&context) &&
        rcl_shutdown(&context) != RCL_RET_OK) {
      print_rcl_error("rcl_shutdown");
      result = 1;
    }
    if (rcl_context_fini(&context) != RCL_RET_OK) {
      print_rcl_error("rcl_context_fini");
      result = 1;
    }
  }
  if (init_options.impl != NULL &&
      rcl_init_options_fini(&init_options) != RCL_RET_OK) {
    print_rcl_error("rcl_init_options_fini");
    result = 1;
  }

  if (result == 0) {
    printf("VelaROS service complete: REQUESTS=%d\n", state.handled);
  }
  return result;
}
