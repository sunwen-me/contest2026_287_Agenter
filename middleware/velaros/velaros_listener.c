/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rcl/error_handling.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"
#include "rcl/subscription.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_typesupport_interface/macros.h"
#include "std_msgs/msg/detail/string__functions.h"
#include "velaros_qemu_init.h"

extern const rosidl_message_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c, std_msgs, msg, String)(void);

#define VELAROS_DEFAULT_MESSAGE_COUNT 3
#define VELAROS_RECEIVE_RETRIES 300
#define VELAROS_RETRY_DELAY_US 100000

static void print_rcl_error(const char * step)
{
  fprintf(stderr, "%s failed: %s\n", step, rcl_get_error_string().str);
  rcl_reset_error();
}

static int parse_message_count(int argc, char * argv[])
{
  char * end = NULL;
  long count;

  if (argc < 2) {
    return VELAROS_DEFAULT_MESSAGE_COUNT;
  }

  count = strtol(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0' || count < 1 || count > 1000) {
    fprintf(stderr, "usage: velaros_listener [1..1000]\n");
    return -1;
  }

  return (int)count;
}

int main(int argc, char * argv[])
{
  const int message_count = parse_message_count(argc, argv);
  const int participant_id = velaros_parse_participant_id(argc, argv);
  const rosidl_message_type_support_t * type_support =
    ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
      rosidl_typesupport_fastrtps_c, std_msgs, msg, String)();
  rcl_allocator_t allocator = rcl_get_default_allocator();
  rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
  rcl_context_t context = rcl_get_zero_initialized_context();
  rcl_node_t node = rcl_get_zero_initialized_node();
  rcl_subscription_t subscription = rcl_get_zero_initialized_subscription();
  rcl_node_options_t node_options = rcl_node_get_default_options();
  rcl_subscription_options_t subscription_options =
    rcl_subscription_get_default_options();
  std_msgs__msg__String message;
  bool message_initialized = false;
  int received = 0;
  int result = 1;

  if (message_count < 0 || participant_id < 0 || type_support == NULL) {
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
  if (rcl_init(0, NULL, &init_options, &context) != RCL_RET_OK) {
    print_rcl_error("rcl_init");
    goto cleanup;
  }
  if (rcl_node_init(
      &node, "velaros_listener", "/velaros", &context,
      &node_options) != RCL_RET_OK) {
    print_rcl_error("rcl_node_init");
    goto cleanup;
  }
  if (rcl_subscription_init(
      &subscription, &node, type_support, "/velaros/chatter",
      &subscription_options) != RCL_RET_OK) {
    print_rcl_error("rcl_subscription_init");
    goto cleanup;
  }
  if (!std_msgs__msg__String__init(&message)) {
    fprintf(stderr, "std_msgs String init failed\n");
    goto cleanup;
  }
  message_initialized = true;

  printf(
    "VelaROS listener ready: /velaros/chatter, expecting %d message(s)\n",
    message_count);
  for (int retry = 0;
       retry < VELAROS_RECEIVE_RETRIES && received < message_count;
       ++retry) {
    const rcl_ret_t take_ret = rcl_take(&subscription, &message, NULL, NULL);
    if (take_ret == RCL_RET_OK) {
      ++received;
      printf("VelaROS RECEIVED: %s\n", message.data.data);
      continue;
    }
    if (take_ret != RCL_RET_SUBSCRIPTION_TAKE_FAILED) {
      print_rcl_error("rcl_take");
      goto cleanup;
    }
    usleep(VELAROS_RETRY_DELAY_US);
  }

  if (received != message_count) {
    fprintf(
      stderr, "receive timeout: expected=%d received=%d\n",
      message_count, received);
    goto cleanup;
  }
  result = 0;

cleanup:
  if (message_initialized) {
    std_msgs__msg__String__fini(&message);
  }
  if (subscription.impl != NULL &&
      rcl_subscription_fini(&subscription, &node) != RCL_RET_OK) {
    print_rcl_error("rcl_subscription_fini");
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
    printf("VelaROS listener complete: RECEIVED=%d\n", received);
  }
  return result;
}
