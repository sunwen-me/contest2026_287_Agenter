/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "rcl/error_handling.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"
#include "rcl/publisher.h"
#include "rcl/subscription.h"
#include "rcl/time.h"
#include "rcl/timer.h"
#include "rcutils/logging.h"
#include "rosidl_typesupport_interface/macros.h"
#include "sensor/temp.h"
#include "std_msgs/msg/detail/float64__functions.h"
#include "velaros/executor.h"
#include "velaros/logging.h"
#include "velaros/uorb_bridge.h"
#include "velaros/uorb_topics.h"

extern const rosidl_message_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c, std_msgs, msg, Float64)(void);

#define VELAROS_INTEGRATION_SAMPLE_COUNT 3
#define VELAROS_INTEGRATION_MATCH_RETRIES 100

typedef struct velaros_integration_state_s
{
  rcl_publisher_t * control_publisher;
  std_msgs__msg__Float64 * control_message;
  struct sensor_temp * sensor_message;
  int sensor_advertisement_fd;
  int timer_callbacks;
  int temperature_callbacks;
  rcl_ret_t callback_rcl_error;
  bool callback_uorb_error;
  bool payload_error;
} velaros_integration_state_t;

static void print_rcl_error(const char * step)
{
  fprintf(stderr, "%s failed: %s\n", step, rcl_get_error_string().str);
  rcl_reset_error();
}

static bool sensor_temp_to_float64(
  const void * untyped_uorb_message,
  void * untyped_ros_message,
  void * user_data)
{
  const struct sensor_temp * sensor_message =
    (const struct sensor_temp *)untyped_uorb_message;
  std_msgs__msg__Float64 * ros_message =
    (std_msgs__msg__Float64 *)untyped_ros_message;

  (void)user_data;
  ros_message->data = sensor_message->temperature;
  return true;
}

static bool float64_to_control_setpoint(
  const void * untyped_ros_message,
  void * untyped_uorb_message,
  void * user_data)
{
  const std_msgs__msg__Float64 * ros_message =
    (const std_msgs__msg__Float64 *)untyped_ros_message;
  struct velaros_control_setpoint_s * control_message =
    (struct velaros_control_setpoint_s *)untyped_uorb_message;

  (void)user_data;
  control_message->timestamp = orb_absolute_time();
  control_message->value = ros_message->data;
  return true;
}

static void temperature_observer_callback(
  const void * untyped_message, void * user_data)
{
  const std_msgs__msg__Float64 * message =
    (const std_msgs__msg__Float64 *)untyped_message;
  velaros_integration_state_t * state =
    (velaros_integration_state_t *)user_data;
  double expected;

  ++state->temperature_callbacks;
  expected = 20.0 + state->temperature_callbacks;
  if (message->data != expected) {
    state->payload_error = true;
  }
}

static void integration_timer_callback(
  rcl_timer_t * timer, int64_t time_since_last_call, uintptr_t callback_data)
{
  velaros_integration_state_t * state =
    (velaros_integration_state_t *)callback_data;

  (void)time_since_last_call;
  ++state->timer_callbacks;
  state->sensor_message->timestamp = orb_absolute_time();
  state->sensor_message->temperature = 20.0 + state->timer_callbacks;
  state->control_message->data = 100.0 + state->timer_callbacks;

  if (orb_publish(
      ORB_ID(sensor_temp), state->sensor_advertisement_fd,
      state->sensor_message) < 0) {
    state->callback_uorb_error = true;
  }
  state->callback_rcl_error = rcl_publish(
    state->control_publisher, state->control_message, NULL);

  if (state->timer_callbacks >= VELAROS_INTEGRATION_SAMPLE_COUNT) {
    const rcl_ret_t cancel_ret = rcl_timer_cancel(timer);
    if (state->callback_rcl_error == RCL_RET_OK) {
      state->callback_rcl_error = cancel_ret;
    }
  }
}

static bool wait_for_match(rcl_publisher_t * publisher)
{
  for (int retry = 0; retry < VELAROS_INTEGRATION_MATCH_RETRIES; ++retry) {
    size_t subscriptions = 0;

    if (rcl_publisher_get_subscription_count(
        publisher, &subscriptions) != RCL_RET_OK) {
      return false;
    }
    if (subscriptions > 0) {
      return true;
    }
    usleep(50000);
  }
  return false;
}

int main(int argc, char * argv[])
{
  const rosidl_message_type_support_t * type_support =
    ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
      rosidl_typesupport_fastrtps_c, std_msgs, msg, Float64)();
  rcl_allocator_t allocator = rcl_get_default_allocator();
  rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
  rcl_context_t context = rcl_get_zero_initialized_context();
  rcl_node_t node = rcl_get_zero_initialized_node();
  rcl_publisher_t temperature_publisher = rcl_get_zero_initialized_publisher();
  rcl_subscription_t temperature_subscription =
    rcl_get_zero_initialized_subscription();
  rcl_publisher_t control_publisher = rcl_get_zero_initialized_publisher();
  rcl_subscription_t control_subscription =
    rcl_get_zero_initialized_subscription();
  rcl_node_options_t node_options = rcl_node_get_default_options();
  rcl_publisher_options_t publisher_options =
    rcl_publisher_get_default_options();
  rcl_subscription_options_t subscription_options =
    rcl_subscription_get_default_options();
  rcl_clock_t clock = {0};
  rcl_timer_t timer = rcl_get_zero_initialized_timer();
  velaros_executor_t executor = velaros_executor_get_zero_initialized();
  velaros_uorb_to_ros_bridge_t sensor_bridge =
    velaros_uorb_to_ros_bridge_get_zero_initialized();
  velaros_ros_to_uorb_bridge_t control_bridge =
    velaros_ros_to_uorb_bridge_get_zero_initialized();
  std_msgs__msg__Float64 temperature_outgoing;
  std_msgs__msg__Float64 temperature_incoming;
  std_msgs__msg__Float64 control_outgoing;
  std_msgs__msg__Float64 control_incoming;
  struct sensor_temp sensor_message = {0};
  struct sensor_temp sensor_bridge_buffer = {0};
  struct velaros_control_setpoint_s control_bridge_buffer = {0};
  struct velaros_control_setpoint_s control_observed = {0};
  velaros_integration_state_t state = {0};
  velaros_logging_stats_t logging_stats;
  bool temperature_outgoing_initialized = false;
  bool temperature_incoming_initialized = false;
  bool control_outgoing_initialized = false;
  bool control_incoming_initialized = false;
  bool logging_installed = false;
  int sensor_advertisement_fd = -1;
  int control_subscription_fd = -1;
  int control_samples = 0;
  int result = 1;

  (void)argc;
  (void)argv;
  if (type_support == NULL) {
    fprintf(stderr, "std_msgs/Float64 type support is unavailable\n");
    return 1;
  }

  node_options.use_global_arguments = false;
  node_options.enable_rosout = false;
  if (rcl_init_options_init(&init_options, allocator) != RCL_RET_OK ||
      rcl_init(0, NULL, &init_options, &context) != RCL_RET_OK ||
      rcl_node_init(
        &node, "velaros_openvela_integration", "/velaros", &context,
        &node_options) != RCL_RET_OK) {
    print_rcl_error("integration context/node init");
    goto cleanup;
  }

  if (rcl_publisher_init(
      &temperature_publisher, &node, type_support,
      "/velaros/sensor/temperature", &publisher_options) != RCL_RET_OK ||
      rcl_subscription_init(
        &temperature_subscription, &node, type_support,
        "/velaros/sensor/temperature", &subscription_options) != RCL_RET_OK ||
      rcl_publisher_init(
        &control_publisher, &node, type_support,
        "/velaros/control/setpoint", &publisher_options) != RCL_RET_OK ||
      rcl_subscription_init(
        &control_subscription, &node, type_support,
        "/velaros/control/setpoint", &subscription_options) != RCL_RET_OK) {
    print_rcl_error("integration ROS endpoint init");
    goto cleanup;
  }

  if (!std_msgs__msg__Float64__init(&temperature_outgoing)) {
    fprintf(stderr, "temperature outgoing message init failed\n");
    goto cleanup;
  }
  temperature_outgoing_initialized = true;
  if (!std_msgs__msg__Float64__init(&temperature_incoming)) {
    fprintf(stderr, "temperature incoming message init failed\n");
    goto cleanup;
  }
  temperature_incoming_initialized = true;
  if (!std_msgs__msg__Float64__init(&control_outgoing)) {
    fprintf(stderr, "control outgoing message init failed\n");
    goto cleanup;
  }
  control_outgoing_initialized = true;
  if (!std_msgs__msg__Float64__init(&control_incoming)) {
    fprintf(stderr, "control incoming message init failed\n");
    goto cleanup;
  }
  control_incoming_initialized = true;

  sensor_advertisement_fd = orb_advertise(
    ORB_ID(sensor_temp), &sensor_message);
  if (sensor_advertisement_fd < 0 ||
      velaros_uorb_to_ros_bridge_init(
        &sensor_bridge, ORB_ID(sensor_temp), &temperature_publisher,
        &sensor_bridge_buffer, &temperature_outgoing,
        sensor_temp_to_float64, NULL) != VELAROS_BRIDGE_OK ||
      velaros_ros_to_uorb_bridge_init(
        &control_bridge, ORB_ID(velaros_control_setpoint),
        &control_bridge_buffer, float64_to_control_setpoint,
        NULL) != VELAROS_BRIDGE_OK) {
    fprintf(stderr, "integration uORB bridge init failed\n");
    goto cleanup;
  }
  control_subscription_fd = orb_subscribe(
    ORB_ID(velaros_control_setpoint));
  if (control_subscription_fd < 0 ||
      orb_copy(
        ORB_ID(velaros_control_setpoint), control_subscription_fd,
        &control_observed) < 0) {
    fprintf(stderr, "integration control observer init failed\n");
    goto cleanup;
  }

  if (!wait_for_match(&temperature_publisher) ||
      !wait_for_match(&control_publisher)) {
    print_rcl_error("integration endpoint match");
    goto cleanup;
  }

  if (rcl_clock_init(RCL_STEADY_TIME, &clock, &allocator) != RCL_RET_OK ||
      rcl_timer_init2(
        &timer, &clock, &context, RCL_MS_TO_NS(100),
        integration_timer_callback, allocator, true) != RCL_RET_OK) {
    print_rcl_error("integration clock/timer init");
    goto cleanup;
  }

  state.control_publisher = &control_publisher;
  state.control_message = &control_outgoing;
  state.sensor_message = &sensor_message;
  state.sensor_advertisement_fd = sensor_advertisement_fd;
  if (rcl_timer_exchange_callback_data(
      &timer, (uintptr_t)&state) != (uintptr_t)NULL ||
      velaros_executor_init(
        &executor, &context, 2, 1, 0, 0, allocator) != RCL_RET_OK ||
      velaros_executor_add_subscription(
        &executor, &temperature_subscription, &temperature_incoming,
        temperature_observer_callback, &state) != RCL_RET_OK ||
      velaros_executor_add_subscription(
        &executor, &control_subscription, &control_incoming,
        velaros_ros_to_uorb_bridge_callback, &control_bridge) != RCL_RET_OK ||
      velaros_executor_add_timer(&executor, &timer) != RCL_RET_OK) {
    print_rcl_error("integration executor init/register");
    goto cleanup;
  }

  velaros_logging_install_syslog();
  logging_installed = true;
  rcutils_log(
    NULL, RCUTILS_LOG_SEVERITY_INFO, "velaros.bridge",
    "uORB bridge initialized");
  rcutils_log(
    NULL, RCUTILS_LOG_SEVERITY_WARN, "velaros.bridge",
    "syslog severity mapping smoke");
  logging_stats = velaros_logging_get_stats();
  if (!velaros_logging_is_syslog_installed() || logging_stats.emitted != 2 ||
      logging_stats.truncated != 0 || logging_stats.invalid_severity != 0) {
    fprintf(stderr, "VelaROS syslog adapter validation failed\n");
    goto cleanup;
  }

  for (int iteration = 0;
       iteration < 50 &&
       (state.temperature_callbacks < VELAROS_INTEGRATION_SAMPLE_COUNT ||
        control_samples < VELAROS_INTEGRATION_SAMPLE_COUNT);
       ++iteration) {
    bool updated = false;
    velaros_bridge_ret_t bridge_ret;
    rcl_ret_t spin_ret = velaros_executor_spin_once(
      &executor, RCL_MS_TO_NS(500));

    if (spin_ret != RCL_RET_OK && spin_ret != RCL_RET_TIMEOUT) {
      print_rcl_error("integration executor spin");
      goto cleanup;
    }
    bridge_ret = velaros_uorb_to_ros_bridge_pump(&sensor_bridge);
    if (bridge_ret != VELAROS_BRIDGE_OK &&
        bridge_ret != VELAROS_BRIDGE_NO_DATA) {
      fprintf(stderr, "uORB to ROS bridge pump failed: %d\n", bridge_ret);
      goto cleanup;
    }
    if (orb_check(control_subscription_fd, &updated) < 0) {
      fprintf(stderr, "control uORB check failed\n");
      goto cleanup;
    }
    if (updated) {
      double expected;

      if (orb_copy(
          ORB_ID(velaros_control_setpoint), control_subscription_fd,
          &control_observed) < 0) {
        fprintf(stderr, "control uORB copy failed\n");
        goto cleanup;
      }
      ++control_samples;
      expected = 100.0 + control_samples;
      if (control_observed.value != expected ||
          control_observed.timestamp == 0) {
        fprintf(stderr, "control uORB payload mismatch\n");
        goto cleanup;
      }
    }
    if (state.callback_rcl_error != RCL_RET_OK ||
        state.callback_uorb_error || state.payload_error ||
        control_bridge.last_result != VELAROS_BRIDGE_OK) {
      fprintf(stderr, "integration callback validation failed\n");
      goto cleanup;
    }
  }

  if (sensor_bridge.forwarded != VELAROS_INTEGRATION_SAMPLE_COUNT ||
      sensor_bridge.errors != 0 ||
      state.temperature_callbacks != VELAROS_INTEGRATION_SAMPLE_COUNT ||
      control_bridge.forwarded != VELAROS_INTEGRATION_SAMPLE_COUNT ||
      control_bridge.errors != 0 ||
      control_samples != VELAROS_INTEGRATION_SAMPLE_COUNT) {
    fprintf(
      stderr,
      "integration count mismatch: uorb_to_ros=%zu/%d ros_to_uorb=%zu/%d\n",
      sensor_bridge.forwarded, state.temperature_callbacks,
      control_bridge.forwarded, control_samples);
    goto cleanup;
  }

  printf("VelaROS syslog adapter messages: %" PRIu32 "\n", logging_stats.emitted);
  printf("VelaROS uORB -> ROS samples: %zu\n", sensor_bridge.forwarded);
  printf("VelaROS ROS -> uORB samples: %zu\n", control_bridge.forwarded);
  result = 0;

cleanup:
  if (logging_installed) {
    velaros_logging_restore();
  }
  if (velaros_executor_fini(&executor) != RCL_RET_OK) {
    print_rcl_error("integration executor fini");
    result = 1;
  }
  if (timer.impl != NULL && rcl_timer_fini(&timer) != RCL_RET_OK) {
    print_rcl_error("integration timer fini");
    result = 1;
  }
  if (rcl_clock_valid(&clock) && rcl_clock_fini(&clock) != RCL_RET_OK) {
    print_rcl_error("integration clock fini");
    result = 1;
  }
  if (velaros_uorb_to_ros_bridge_fini(&sensor_bridge) != VELAROS_BRIDGE_OK) {
    fprintf(stderr, "sensor bridge fini failed\n");
    result = 1;
  }
  if (velaros_ros_to_uorb_bridge_fini(&control_bridge) != VELAROS_BRIDGE_OK) {
    fprintf(stderr, "control bridge fini failed\n");
    result = 1;
  }
  if (control_subscription_fd >= 0 &&
      orb_unsubscribe(control_subscription_fd) < 0) {
    fprintf(stderr, "control observer unsubscribe failed\n");
    result = 1;
  }
  if (sensor_advertisement_fd >= 0 &&
      orb_unadvertise(sensor_advertisement_fd) < 0) {
    fprintf(stderr, "sensor unadvertise failed\n");
    result = 1;
  }
  if (control_incoming_initialized) {
    std_msgs__msg__Float64__fini(&control_incoming);
  }
  if (control_outgoing_initialized) {
    std_msgs__msg__Float64__fini(&control_outgoing);
  }
  if (temperature_incoming_initialized) {
    std_msgs__msg__Float64__fini(&temperature_incoming);
  }
  if (temperature_outgoing_initialized) {
    std_msgs__msg__Float64__fini(&temperature_outgoing);
  }
  if (control_subscription.impl != NULL &&
      rcl_subscription_fini(&control_subscription, &node) != RCL_RET_OK) {
    print_rcl_error("control ROS subscription fini");
    result = 1;
  }
  if (control_publisher.impl != NULL &&
      rcl_publisher_fini(&control_publisher, &node) != RCL_RET_OK) {
    print_rcl_error("control ROS publisher fini");
    result = 1;
  }
  if (temperature_subscription.impl != NULL &&
      rcl_subscription_fini(&temperature_subscription, &node) != RCL_RET_OK) {
    print_rcl_error("temperature ROS subscription fini");
    result = 1;
  }
  if (temperature_publisher.impl != NULL &&
      rcl_publisher_fini(&temperature_publisher, &node) != RCL_RET_OK) {
    print_rcl_error("temperature ROS publisher fini");
    result = 1;
  }
  if (node.impl != NULL && rcl_node_fini(&node) != RCL_RET_OK) {
    print_rcl_error("integration node fini");
    result = 1;
  }
  if (context.impl != NULL) {
    if (rcl_context_is_valid(&context) && rcl_shutdown(&context) != RCL_RET_OK) {
      print_rcl_error("integration shutdown");
      result = 1;
    }
    if (rcl_context_fini(&context) != RCL_RET_OK) {
      print_rcl_error("integration context fini");
      result = 1;
    }
  }
  if (init_options.impl != NULL &&
      rcl_init_options_fini(&init_options) != RCL_RET_OK) {
    print_rcl_error("integration init options fini");
    result = 1;
  }

  if (result == 0) {
    printf("VelaROS openVela integration smoke: PASS\n");
  }
  return result;
}
