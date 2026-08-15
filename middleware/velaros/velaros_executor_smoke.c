/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "rcl/error_handling.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"
#include "rcl/publisher.h"
#include "rcl/subscription.h"
#include "rcl/time.h"
#include "rcl/timer.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/string_functions.h"
#include "rosidl_typesupport_interface/macros.h"
#include "std_msgs/msg/detail/string__functions.h"
#include "velaros/executor.h"

extern const rosidl_message_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c, std_msgs, msg, String)(void);

#define VELAROS_EXECUTOR_SAMPLE_COUNT 3
#define VELAROS_EXECUTOR_MATCH_RETRIES 100

typedef struct velaros_executor_smoke_state_s
{
  rcl_publisher_t * publisher;
  std_msgs__msg__String * outgoing;
  int timer_callbacks;
  int subscription_callbacks;
  rcl_ret_t callback_error;
  bool payload_error;
} velaros_executor_smoke_state_t;

static void print_rcl_error(const char * step)
{
  fprintf(stderr, "%s failed: %s\n", step, rcl_get_error_string().str);
  rcl_reset_error();
}

static void executor_timer_callback(
  rcl_timer_t * timer, int64_t time_since_last_call, uintptr_t callback_data)
{
  velaros_executor_smoke_state_t * state =
    (velaros_executor_smoke_state_t *)callback_data;
  char payload[64];
  int length;

  (void)time_since_last_call;
  ++state->timer_callbacks;
  length = snprintf(
    payload, sizeof(payload), "VelaROS executor sample #%d",
    state->timer_callbacks);
  if (length < 0 || (size_t)length >= sizeof(payload) ||
      !rosidl_runtime_c__String__assign(&state->outgoing->data, payload)) {
    state->payload_error = true;
    state->callback_error = rcl_timer_cancel(timer);
    return;
  }

  state->callback_error = rcl_publish(
    state->publisher, state->outgoing, NULL);
  if (state->callback_error != RCL_RET_OK ||
      state->timer_callbacks >= VELAROS_EXECUTOR_SAMPLE_COUNT) {
    const rcl_ret_t cancel_ret = rcl_timer_cancel(timer);
    if (state->callback_error == RCL_RET_OK) {
      state->callback_error = cancel_ret;
    }
  }
}

static void executor_subscription_callback(
  const void * untyped_message, void * user_data)
{
  const std_msgs__msg__String * message =
    (const std_msgs__msg__String *)untyped_message;
  velaros_executor_smoke_state_t * state =
    (velaros_executor_smoke_state_t *)user_data;
  char expected[64];
  int length;

  ++state->subscription_callbacks;
  length = snprintf(
    expected, sizeof(expected), "VelaROS executor sample #%d",
    state->subscription_callbacks);
  if (length < 0 || (size_t)length >= sizeof(expected) ||
      message->data.data == NULL || strcmp(message->data.data, expected) != 0) {
    state->payload_error = true;
  }
}

int main(int argc, char * argv[])
{
  const rosidl_message_type_support_t * type_support =
    ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
      rosidl_typesupport_fastrtps_c, std_msgs, msg, String)();
  rcl_allocator_t allocator = rcl_get_default_allocator();
  rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
  rcl_context_t context = rcl_get_zero_initialized_context();
  rcl_node_t node = rcl_get_zero_initialized_node();
  rcl_publisher_t publisher = rcl_get_zero_initialized_publisher();
  rcl_subscription_t subscription = rcl_get_zero_initialized_subscription();
  rcl_clock_t clock = {0};
  rcl_timer_t timer = rcl_get_zero_initialized_timer();
  velaros_executor_t executor = velaros_executor_get_zero_initialized();
  rcl_node_options_t node_options = rcl_node_get_default_options();
  rcl_publisher_options_t publisher_options =
    rcl_publisher_get_default_options();
  rcl_subscription_options_t subscription_options =
    rcl_subscription_get_default_options();
  std_msgs__msg__String outgoing;
  std_msgs__msg__String incoming;
  velaros_executor_smoke_state_t state = {0};
  bool outgoing_initialized = false;
  bool incoming_initialized = false;
  bool matched = false;
  int result = 1;

  (void)argc;
  (void)argv;
  if (type_support == NULL) {
    fprintf(stderr, "std_msgs/String type support is unavailable\n");
    return 1;
  }

  node_options.use_global_arguments = false;
  node_options.enable_rosout = false;
  if (rcl_init_options_init(&init_options, allocator) != RCL_RET_OK ||
      rcl_init(0, NULL, &init_options, &context) != RCL_RET_OK ||
      rcl_node_init(
        &node, "velaros_executor_smoke", "/velaros", &context,
        &node_options) != RCL_RET_OK) {
    print_rcl_error("executor context/node init");
    goto cleanup;
  }
  if (rcl_publisher_init(
      &publisher, &node, type_support, "/velaros/executor_smoke",
      &publisher_options) != RCL_RET_OK ||
      rcl_subscription_init(
        &subscription, &node, type_support, "/velaros/executor_smoke",
        &subscription_options) != RCL_RET_OK) {
    print_rcl_error("executor publisher/subscription init");
    goto cleanup;
  }
  if (!std_msgs__msg__String__init(&outgoing)) {
    fprintf(stderr, "executor outgoing std_msgs/String init failed\n");
    goto cleanup;
  }
  outgoing_initialized = true;
  if (!std_msgs__msg__String__init(&incoming)) {
    fprintf(stderr, "executor incoming std_msgs/String init failed\n");
    goto cleanup;
  }
  incoming_initialized = true;

  for (int retry = 0; retry < VELAROS_EXECUTOR_MATCH_RETRIES; ++retry) {
    size_t subscriptions = 0;
    if (rcl_publisher_get_subscription_count(
        &publisher, &subscriptions) != RCL_RET_OK) {
      print_rcl_error("executor subscription count");
      goto cleanup;
    }
    if (subscriptions > 0) {
      matched = true;
      break;
    }
    usleep(50000);
  }
  if (!matched) {
    fprintf(stderr, "executor local endpoint match timed out\n");
    goto cleanup;
  }

  if (rcl_clock_init(RCL_STEADY_TIME, &clock, &allocator) != RCL_RET_OK ||
      rcl_timer_init2(
        &timer, &clock, &context, RCL_MS_TO_NS(100),
        executor_timer_callback, allocator, true) != RCL_RET_OK) {
    print_rcl_error("executor clock/timer init");
    goto cleanup;
  }

  state.publisher = &publisher;
  state.outgoing = &outgoing;
  if (rcl_timer_exchange_callback_data(
      &timer, (uintptr_t)&state) != (uintptr_t)NULL) {
    fprintf(stderr, "executor timer callback data was not initially empty\n");
    goto cleanup;
  }
  if (velaros_executor_init(
      &executor, &context, 1, 1, 0, 0, allocator) != RCL_RET_OK ||
      velaros_executor_add_subscription(
        &executor, &subscription, &incoming,
        executor_subscription_callback, &state) != RCL_RET_OK ||
      velaros_executor_add_timer(&executor, &timer) != RCL_RET_OK) {
    print_rcl_error("VelaROS executor init/register");
    goto cleanup;
  }

  for (int iteration = 0;
       iteration < 30 &&
       state.subscription_callbacks < VELAROS_EXECUTOR_SAMPLE_COUNT;
       ++iteration) {
    const rcl_ret_t spin_ret = velaros_executor_spin_once(
      &executor, RCL_MS_TO_NS(500));
    if (spin_ret != RCL_RET_OK && spin_ret != RCL_RET_TIMEOUT) {
      print_rcl_error("velaros_executor_spin_once");
      goto cleanup;
    }
    if (state.callback_error != RCL_RET_OK || state.payload_error) {
      fprintf(stderr, "executor callback validation failed\n");
      goto cleanup;
    }
  }

  if (state.timer_callbacks != VELAROS_EXECUTOR_SAMPLE_COUNT ||
      state.subscription_callbacks != VELAROS_EXECUTOR_SAMPLE_COUNT) {
    fprintf(
      stderr, "executor count mismatch: timer=%d subscription=%d\n",
      state.timer_callbacks, state.subscription_callbacks);
    goto cleanup;
  }
  printf("VelaROS executor timer callbacks: %d\n", state.timer_callbacks);
  printf(
    "VelaROS executor subscription callbacks: %d\n",
    state.subscription_callbacks);
  result = 0;

cleanup:
  if (velaros_executor_fini(&executor) != RCL_RET_OK) {
    print_rcl_error("velaros_executor_fini");
    result = 1;
  }
  if (timer.impl != NULL && rcl_timer_fini(&timer) != RCL_RET_OK) {
    print_rcl_error("executor rcl_timer_fini");
    result = 1;
  }
  if (rcl_clock_valid(&clock) && rcl_clock_fini(&clock) != RCL_RET_OK) {
    print_rcl_error("executor rcl_clock_fini");
    result = 1;
  }
  if (incoming_initialized) {
    std_msgs__msg__String__fini(&incoming);
  }
  if (outgoing_initialized) {
    std_msgs__msg__String__fini(&outgoing);
  }
  if (subscription.impl != NULL &&
      rcl_subscription_fini(&subscription, &node) != RCL_RET_OK) {
    print_rcl_error("executor rcl_subscription_fini");
    result = 1;
  }
  if (publisher.impl != NULL &&
      rcl_publisher_fini(&publisher, &node) != RCL_RET_OK) {
    print_rcl_error("executor rcl_publisher_fini");
    result = 1;
  }
  if (node.impl != NULL && rcl_node_fini(&node) != RCL_RET_OK) {
    print_rcl_error("executor rcl_node_fini");
    result = 1;
  }
  if (context.impl != NULL) {
    if (rcl_context_is_valid(&context) && rcl_shutdown(&context) != RCL_RET_OK) {
      print_rcl_error("executor rcl_shutdown");
      result = 1;
    }
    if (rcl_context_fini(&context) != RCL_RET_OK) {
      print_rcl_error("executor rcl_context_fini");
      result = 1;
    }
  }
  if (init_options.impl != NULL &&
      rcl_init_options_fini(&init_options) != RCL_RET_OK) {
    print_rcl_error("executor rcl_init_options_fini");
    result = 1;
  }

  if (result == 0) {
    printf("VelaROS minimal single-thread executor smoke: PASS\n");
  }
  return result;
}
