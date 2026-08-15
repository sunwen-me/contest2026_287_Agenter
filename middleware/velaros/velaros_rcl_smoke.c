/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "rcl/error_handling.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"
#include "rcl/time.h"
#include "rcl/timer.h"
#include "rcl/wait.h"

static void print_rcl_error(const char * step)
{
  fprintf(stderr, "%s failed: %s\n", step, rcl_get_error_string().str);
  rcl_reset_error();
}

static void timer_callback(
  rcl_timer_t * timer, int64_t time_since_last_call, uintptr_t callback_data)
{
  int * callback_count = (int *)callback_data;

  (void)timer;
  (void)time_since_last_call;
  if (callback_count != NULL) {
    ++*callback_count;
  }
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
  rcl_clock_t clock = {0};
  rcl_timer_t timer = rcl_get_zero_initialized_timer();
  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  int timer_callback_count = 0;
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

  if (rcl_clock_init(RCL_STEADY_TIME, &clock, &allocator) != RCL_RET_OK) {
    print_rcl_error("rcl_clock_init");
    goto cleanup;
  }
  printf("rcl steady clock init: PASS\n");

  if (rcl_timer_init2(
      &timer, &clock, &context, RCL_MS_TO_NS(50), timer_callback,
      allocator, true) != RCL_RET_OK) {
    print_rcl_error("rcl_timer_init2");
    goto cleanup;
  }
  if (rcl_timer_exchange_callback_data(
      &timer, (uintptr_t)&timer_callback_count) != (uintptr_t)NULL) {
    fprintf(stderr, "rcl timer callback data was not initially empty\n");
    goto cleanup;
  }

  if (rcl_wait_set_init(
      &wait_set, 0, 0, 1, 0, 0, 0, &context, allocator) != RCL_RET_OK) {
    print_rcl_error("rcl_wait_set_init");
    goto cleanup;
  }
  if (rcl_wait_set_add_timer(&wait_set, &timer, NULL) != RCL_RET_OK) {
    print_rcl_error("rcl_wait_set_add_timer");
    goto cleanup;
  }
  if (rcl_wait(&wait_set, RCL_S_TO_NS(1)) != RCL_RET_OK ||
      wait_set.timers[0] != &timer) {
    print_rcl_error("rcl_wait timer readiness");
    goto cleanup;
  }
  if (rcl_timer_call(&timer) != RCL_RET_OK || timer_callback_count != 1) {
    print_rcl_error("rcl_timer_call");
    goto cleanup;
  }
  printf("rcl timer/wait set trigger: PASS\n");

  if (rcl_wait_set_fini(&wait_set) != RCL_RET_OK) {
    print_rcl_error("rcl_wait_set_fini");
    goto cleanup;
  }
  printf("rcl wait set fini: PASS\n");

  if (rcl_timer_fini(&timer) != RCL_RET_OK) {
    print_rcl_error("rcl_timer_fini");
    goto cleanup;
  }
  if (rcl_clock_fini(&clock) != RCL_RET_OK) {
    print_rcl_error("rcl_clock_fini");
    goto cleanup;
  }
  printf("rcl timer/clock fini: PASS\n");

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
  if (wait_set.impl != NULL) {
    const rcl_ret_t cleanup_ret = rcl_wait_set_fini(&wait_set);
    if (cleanup_ret != RCL_RET_OK) {
      print_rcl_error("cleanup rcl_wait_set_fini");
    }
  }
  if (timer.impl != NULL) {
    const rcl_ret_t cleanup_ret = rcl_timer_fini(&timer);
    if (cleanup_ret != RCL_RET_OK) {
      print_rcl_error("cleanup rcl_timer_fini");
    }
  }
  if (rcl_clock_valid(&clock)) {
    const rcl_ret_t cleanup_ret = rcl_clock_fini(&clock);
    if (cleanup_ret != RCL_RET_OK) {
      print_rcl_error("cleanup rcl_clock_fini");
    }
  }
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
