/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "action_msgs/msg/detail/goal_status__struct.h"
#include "example_interfaces/action/detail/fibonacci__functions.h"
#include "rcl/error_handling.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"
#include "rcl_action/rcl_action.h"
#include "velaros/action_typesupport.h"
#include "velaros/executor.h"
#include "velaros_qemu_init.h"

#ifndef CONFIG_VELAROS_ACTION_MAX_GOALS
#define CONFIG_VELAROS_ACTION_MAX_GOALS 2
#endif

#ifndef CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY
#define CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY 32
#endif

#define VELAROS_ACTION_NAME "/velaros/fibonacci"
#define VELAROS_ACTION_STEP_NS RCL_MS_TO_NS(150)

typedef struct velaros_goal_slot_s
{
  bool used;
  bool result_request_pending;
  bool result_sent;
  uint8_t goal_id[16];
  rcl_action_goal_handle_t * handle;
  rmw_request_id_t result_request_id;
  int32_t order;
  int32_t sequence[CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY];
  size_t sequence_size;
  int64_t next_step_ns;
} velaros_goal_slot_t;

typedef struct velaros_action_server_state_s
{
  rcl_action_server_t * server;
  velaros_goal_slot_t goals[CONFIG_VELAROS_ACTION_MAX_GOALS];
  int result_responses;
} velaros_action_server_state_t;

static void print_rcl_error(const char * step)
{
  fprintf(stderr, "%s failed: %s\n", step, rcl_get_error_string().str);
  rcl_reset_error();
}

static int64_t monotonic_now_ns(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return ((int64_t)now.tv_sec * RCL_S_TO_NS(1)) + now.tv_nsec;
}

static bool same_goal_id(const uint8_t lhs[16], const uint8_t rhs[16])
{
  return memcmp(lhs, rhs, 16) == 0;
}

static velaros_goal_slot_t * find_goal(
  velaros_action_server_state_t * state, const uint8_t goal_id[16])
{
  for (size_t index = 0; index < CONFIG_VELAROS_ACTION_MAX_GOALS; ++index) {
    if (state->goals[index].used &&
        same_goal_id(state->goals[index].goal_id, goal_id)) {
      return &state->goals[index];
    }
  }
  return NULL;
}

static velaros_goal_slot_t * free_goal(velaros_action_server_state_t * state)
{
  for (size_t index = 0; index < CONFIG_VELAROS_ACTION_MAX_GOALS; ++index) {
    if (!state->goals[index].used) {
      return &state->goals[index];
    }
  }
  return NULL;
}

static rcl_ret_t publish_status(velaros_action_server_state_t * state)
{
  rcl_action_goal_status_array_t status =
    rcl_action_get_zero_initialized_goal_status_array();
  rcl_ret_t ret = rcl_action_get_goal_status_array(state->server, &status);

  if (ret == RCL_RET_OK) {
    ret = rcl_action_publish_status(state->server, &status.msg);
  }
  if (status.msg.status_list.data != NULL &&
      rcl_action_goal_status_array_fini(&status) != RCL_RET_OK &&
      ret == RCL_RET_OK) {
    ret = RCL_RET_ERROR;
  }
  return ret;
}

static rcl_ret_t send_result(
  velaros_action_server_state_t * state, velaros_goal_slot_t * slot,
  rmw_request_id_t * request_id)
{
  example_interfaces__action__Fibonacci_GetResult_Response response;
  rcl_action_goal_state_t status = GOAL_STATE_UNKNOWN;

  memset(&response, 0, sizeof(response));
  if (slot != NULL) {
    if (rcl_action_goal_handle_get_status(slot->handle, &status) != RCL_RET_OK) {
      return RCL_RET_ERROR;
    }
    response.result.sequence.data = slot->sequence;
    response.result.sequence.size = slot->sequence_size;
    response.result.sequence.capacity = CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY;
  }
  response.status = status;
  if (rcl_action_send_result_response(
      state->server, request_id, &response) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  if (slot != NULL) {
    slot->result_request_pending = false;
    slot->result_sent = true;
  }
  if (slot != NULL) {
    ++state->result_responses;
  }
  printf(
    "VelaROS ACTION RESULT: status=%d values=%zu\n",
    (int)status, slot == NULL ? 0u : slot->sequence_size);
  return RCL_RET_OK;
}

static rcl_ret_t handle_goal_request(velaros_action_server_state_t * state)
{
  example_interfaces__action__Fibonacci_SendGoal_Request request;
  example_interfaces__action__Fibonacci_SendGoal_Response response;
  rmw_request_id_t request_id;
  velaros_goal_slot_t * slot;
  rcl_action_goal_info_t goal_info = rcl_action_get_zero_initialized_goal_info();

  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  memset(&request_id, 0, sizeof(request_id));
  if (rcl_action_take_goal_request(state->server, &request_id, &request) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }

  slot = free_goal(state);
  if (slot != NULL && request.goal.order >= 1 &&
      request.goal.order < CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY) {
    rcl_action_goal_handle_t * handle;

    memcpy(goal_info.goal_id.uuid, request.goal_id.uuid, 16);
    handle = rcl_action_accept_new_goal(state->server, &goal_info);
    if (handle != NULL) {
      memset(slot, 0, sizeof(*slot));
      slot->used = true;
      slot->handle = handle;
    }
  }

  if (slot != NULL && slot->handle != NULL) {
    rcl_action_goal_info_t accepted_info = rcl_action_get_zero_initialized_goal_info();
    memcpy(slot->goal_id, request.goal_id.uuid, 16);
    slot->order = request.goal.order;
    slot->sequence[0] = 0;
    slot->sequence[1] = 1;
    slot->sequence_size = 2;
    slot->next_step_ns = monotonic_now_ns() + VELAROS_ACTION_STEP_NS;
    if (rcl_action_update_goal_state(slot->handle, GOAL_EVENT_EXECUTE) != RCL_RET_OK ||
        rcl_action_goal_handle_get_info(slot->handle, &accepted_info) != RCL_RET_OK) {
      return RCL_RET_ERROR;
    }
    response.accepted = true;
    response.stamp = accepted_info.stamp;
  }

  if (rcl_action_send_goal_response(
      state->server, &request_id, &response) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  printf(
    "VelaROS ACTION GOAL: order=%ld accepted=%s active=%d/%d\n",
    (long)request.goal.order, response.accepted ? "yes" : "no",
    response.accepted ? 1 : 0, CONFIG_VELAROS_ACTION_MAX_GOALS);
  return response.accepted ? publish_status(state) : RCL_RET_OK;
}

static rcl_ret_t handle_result_request(velaros_action_server_state_t * state)
{
  example_interfaces__action__Fibonacci_GetResult_Request request;
  rmw_request_id_t request_id;
  velaros_goal_slot_t * slot;
  rcl_action_goal_state_t status = GOAL_STATE_UNKNOWN;

  memset(&request, 0, sizeof(request));
  memset(&request_id, 0, sizeof(request_id));
  if (rcl_action_take_result_request(state->server, &request_id, &request) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  slot = find_goal(state, request.goal_id.uuid);
  if (slot == NULL) {
    return send_result(state, NULL, &request_id);
  }
  if (rcl_action_goal_handle_get_status(slot->handle, &status) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  if (status == GOAL_STATE_SUCCEEDED || status == GOAL_STATE_CANCELED ||
      status == GOAL_STATE_ABORTED) {
    return send_result(state, slot, &request_id);
  }
  if (slot->result_request_pending) {
    RCL_SET_ERROR_MSG("bounded Action result waiter capacity exhausted");
    return RCL_RET_ERROR;
  }
  slot->result_request_pending = true;
  slot->result_request_id = request_id;
  return RCL_RET_OK;
}

static rcl_ret_t handle_cancel_request(velaros_action_server_state_t * state)
{
  rcl_action_cancel_request_t request = rcl_action_get_zero_initialized_cancel_request();
  rcl_action_cancel_response_t response = rcl_action_get_zero_initialized_cancel_response();
  rmw_request_id_t request_id;
  rcl_ret_t ret;
  size_t canceled_count = 0;

  memset(&request_id, 0, sizeof(request_id));
  if (rcl_action_take_cancel_request(state->server, &request_id, &request) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  ret = rcl_action_process_cancel_request(state->server, &request, &response);
  if (ret != RCL_RET_OK) {
    return ret;
  }
  canceled_count = response.msg.goals_canceling.size;
  for (size_t index = 0; index < response.msg.goals_canceling.size; ++index) {
    velaros_goal_slot_t * slot = find_goal(
      state, response.msg.goals_canceling.data[index].goal_id.uuid);
    if (slot != NULL &&
        rcl_action_update_goal_state(slot->handle, GOAL_EVENT_CANCEL_GOAL) != RCL_RET_OK) {
      ret = RCL_RET_ERROR;
      break;
    }
  }
  if (ret == RCL_RET_OK) {
    ret = rcl_action_send_cancel_response(state->server, &request_id, &response.msg);
  }
  if (response.msg.goals_canceling.data != NULL &&
      rcl_action_cancel_response_fini(&response) != RCL_RET_OK &&
      ret == RCL_RET_OK) {
    ret = RCL_RET_ERROR;
  }
  if (ret == RCL_RET_OK) {
    printf("VelaROS ACTION CANCEL: goals=%zu\n", canceled_count);
    ret = publish_status(state);
  }
  return ret;
}

static rcl_ret_t handle_expired_goals(velaros_action_server_state_t * state)
{
  rcl_action_goal_info_t expired[CONFIG_VELAROS_ACTION_MAX_GOALS];
  size_t expired_count = 0;
  rcl_ret_t ret = rcl_action_expire_goals(
    state->server, expired, CONFIG_VELAROS_ACTION_MAX_GOALS, &expired_count);

  if (ret != RCL_RET_OK) {
    return ret;
  }
  for (size_t index = 0; index < expired_count; ++index) {
    velaros_goal_slot_t * slot = find_goal(state, expired[index].goal_id.uuid);
    if (slot != NULL) {
      memset(slot, 0, sizeof(*slot));
    }
  }
  return RCL_RET_OK;
}

static rcl_ret_t action_ready_callback(
  rcl_action_server_t * server,
  bool goal_request_ready,
  bool cancel_request_ready,
  bool result_request_ready,
  bool goal_expired,
  void * user_data)
{
  velaros_action_server_state_t * state = user_data;

  (void)server;
  if (goal_request_ready && handle_goal_request(state) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  if (cancel_request_ready && handle_cancel_request(state) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  if (result_request_ready && handle_result_request(state) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  return goal_expired ? handle_expired_goals(state) : RCL_RET_OK;
}

static rcl_ret_t advance_goals(velaros_action_server_state_t * state)
{
  const int64_t now = monotonic_now_ns();

  for (size_t index = 0; index < CONFIG_VELAROS_ACTION_MAX_GOALS; ++index) {
    velaros_goal_slot_t * slot = &state->goals[index];
    rcl_action_goal_state_t status;

    if (!slot->used || slot->handle == NULL || slot->result_sent ||
        rcl_action_goal_handle_get_status(slot->handle, &status) != RCL_RET_OK) {
      continue;
    }
    if (status == GOAL_STATE_CANCELING) {
      if (rcl_action_update_goal_state(slot->handle, GOAL_EVENT_CANCELED) != RCL_RET_OK ||
          rcl_action_notify_goal_done(state->server) != RCL_RET_OK ||
          publish_status(state) != RCL_RET_OK) {
        return RCL_RET_ERROR;
      }
      if (slot->result_request_pending &&
          send_result(state, slot, &slot->result_request_id) != RCL_RET_OK) {
        return RCL_RET_ERROR;
      }
      continue;
    }
    if (status != GOAL_STATE_EXECUTING || now < slot->next_step_ns) {
      continue;
    }

    if (slot->sequence_size <= (size_t)slot->order) {
      example_interfaces__action__Fibonacci_FeedbackMessage feedback;
      const size_t size = slot->sequence_size;

      slot->sequence[size] = slot->sequence[size - 1] + slot->sequence[size - 2];
      ++slot->sequence_size;
      slot->next_step_ns = now + VELAROS_ACTION_STEP_NS;
      memset(&feedback, 0, sizeof(feedback));
      memcpy(feedback.goal_id.uuid, slot->goal_id, 16);
      feedback.feedback.sequence.data = slot->sequence;
      feedback.feedback.sequence.size = slot->sequence_size;
      feedback.feedback.sequence.capacity = CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY;
      if (rcl_action_publish_feedback(state->server, &feedback) != RCL_RET_OK) {
        return RCL_RET_ERROR;
      }
      printf("VelaROS ACTION FEEDBACK: values=%zu\n", slot->sequence_size);
    } else {
      if (rcl_action_update_goal_state(slot->handle, GOAL_EVENT_SUCCEED) != RCL_RET_OK ||
          rcl_action_notify_goal_done(state->server) != RCL_RET_OK ||
          publish_status(state) != RCL_RET_OK) {
        return RCL_RET_ERROR;
      }
      if (slot->result_request_pending &&
          send_result(state, slot, &slot->result_request_id) != RCL_RET_OK) {
        return RCL_RET_ERROR;
      }
    }
  }
  return RCL_RET_OK;
}

static int parse_goal_count(int argc, char * argv[])
{
  char * end = NULL;
  long count = argc > 1 ? strtol(argv[1], &end, 10) : 1;

  if ((argc > 1 && (end == argv[1] || *end != '\0')) || count < 0 || count > 1000) {
    fprintf(stderr, "usage: velaros_action_server [results 0..1000] [participant-id]\n");
    return -1;
  }
  return (int)count;
}

int main(int argc, char * argv[])
{
  const int goal_count = parse_goal_count(argc, argv);
  const int participant_id = velaros_parse_participant_id(argc, argv);
  rcl_allocator_t allocator = rcl_get_default_allocator();
  rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
  rcl_context_t context = rcl_get_zero_initialized_context();
  rcl_node_t node = rcl_get_zero_initialized_node();
  rcl_node_options_t node_options = rcl_node_get_default_options();
  rcl_clock_t clock = {0};
  rcl_action_server_t server = rcl_action_get_zero_initialized_server();
  rcl_action_server_options_t server_options = rcl_action_server_get_default_options();
  velaros_executor_t executor = velaros_executor_get_zero_initialized();
  velaros_action_server_state_t state;
  int result = 1;

  memset(&state, 0, sizeof(state));
  state.server = &server;
  node_options.use_global_arguments = false;
  node_options.enable_rosout = false;
  server_options.result_timeout.nanoseconds = RCL_S_TO_NS(2);
  if (goal_count < 0 || participant_id < 0 ||
      velaros_fibonacci_action_typesupport() == NULL) {
    return 1;
  }
  if (rcl_init_options_init(&init_options, allocator) != RCL_RET_OK ||
      velaros_configure_qemu_interop(&init_options, participant_id) != RCL_RET_OK ||
      rcl_init(0, NULL, &init_options, &context) != RCL_RET_OK ||
      rcl_node_init(&node, "action_server", "/velaros", &context, &node_options) != RCL_RET_OK ||
      rcl_clock_init(RCL_STEADY_TIME, &clock, &allocator) != RCL_RET_OK ||
      rcl_action_server_init(
        &server, &node, &clock, velaros_fibonacci_action_typesupport(),
        VELAROS_ACTION_NAME, &server_options) != RCL_RET_OK ||
      velaros_executor_init_with_actions(
        &executor, &context, 0, 0, 0, 0, 0, 1, allocator) != RCL_RET_OK ||
      velaros_executor_add_action_server(
        &executor, &server, action_ready_callback, &state) != RCL_RET_OK) {
    print_rcl_error("VelaROS Action server init");
    goto cleanup;
  }

  printf(
    "VelaROS Action server ready: %s max_goals=%d sequence=%d\n",
    VELAROS_ACTION_NAME, CONFIG_VELAROS_ACTION_MAX_GOALS,
    CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY);
  while (goal_count == 0 || state.result_responses < goal_count) {
    rcl_ret_t ret = velaros_executor_spin_once(&executor, RCL_MS_TO_NS(50));
    if (ret != RCL_RET_OK && ret != RCL_RET_TIMEOUT) {
      print_rcl_error("VelaROS Action server spin");
      goto cleanup;
    }
    if (advance_goals(&state) != RCL_RET_OK) {
      print_rcl_error("VelaROS Action state machine");
      goto cleanup;
    }
  }
  result = 0;

cleanup:
  if (velaros_executor_fini(&executor) != RCL_RET_OK) {
    print_rcl_error("velaros_executor_fini");
    result = 1;
  }
  if (server.impl != NULL && rcl_action_server_fini(&server, &node) != RCL_RET_OK) {
    print_rcl_error("rcl_action_server_fini");
    result = 1;
  }
  if (rcl_clock_valid(&clock) && rcl_clock_fini(&clock) != RCL_RET_OK) {
    print_rcl_error("rcl_clock_fini");
    result = 1;
  }
  if (node.impl != NULL && rcl_node_fini(&node) != RCL_RET_OK) {
    print_rcl_error("rcl_node_fini");
    result = 1;
  }
  if (context.impl != NULL) {
    if (rcl_context_is_valid(&context) && rcl_shutdown(&context) != RCL_RET_OK) {
      print_rcl_error("rcl_shutdown");
      result = 1;
    }
    if (rcl_context_fini(&context) != RCL_RET_OK) {
      print_rcl_error("rcl_context_fini");
      result = 1;
    }
  }
  if (init_options.impl != NULL && rcl_init_options_fini(&init_options) != RCL_RET_OK) {
    print_rcl_error("rcl_init_options_fini");
    result = 1;
  }
  if (result == 0) {
    printf("VelaROS Action server complete: RESULTS=%d\n", state.result_responses);
  }
  return result;
}
