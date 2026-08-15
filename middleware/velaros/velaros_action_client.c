/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "action_msgs/msg/detail/goal_status_array__functions.h"
#include "action_msgs/srv/detail/cancel_goal__functions.h"
#include "example_interfaces/action/detail/fibonacci__functions.h"
#include "rcl/error_handling.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"
#include "rcl_action/rcl_action.h"
#include "velaros/action_typesupport.h"
#include "velaros/executor.h"
#include "velaros_qemu_init.h"

#define VELAROS_ACTION_NAME "/velaros/fibonacci"
#define VELAROS_ACTION_DISCOVERY_RETRIES 100

typedef struct velaros_action_client_state_s
{
  rcl_action_client_t * client;
  example_interfaces__action__Fibonacci_SendGoal_Response goal_response;
  example_interfaces__action__Fibonacci_GetResult_Response result_response;
  example_interfaces__action__Fibonacci_FeedbackMessage feedback;
  action_msgs__msg__GoalStatusArray status;
  action_msgs__srv__CancelGoal_Response cancel_response;
  uint8_t goal_id[16];
  int64_t goal_sequence;
  int64_t result_sequence;
  int64_t cancel_sequence;
  int feedback_count;
  bool cancel_after_feedback;
  bool cancel_sent;
  bool result_received;
} velaros_action_client_state_t;

static void print_rcl_error(const char * step)
{
  fprintf(stderr, "%s failed: %s\n", step, rcl_get_error_string().str);
  rcl_reset_error();
}

static bool same_goal_id(const uint8_t lhs[16], const uint8_t rhs[16])
{
  return memcmp(lhs, rhs, 16) == 0;
}

static rcl_ret_t take_goal_response(velaros_action_client_state_t * state)
{
  rmw_request_id_t request_id;
  example_interfaces__action__Fibonacci_GetResult_Request result_request;
  rcl_ret_t ret;

  memset(&request_id, 0, sizeof(request_id));
  memset(&result_request, 0, sizeof(result_request));
  ret = rcl_action_take_goal_response(
    state->client, &request_id, &state->goal_response);
  if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
    return RCL_RET_OK;
  }
  if (ret != RCL_RET_OK) {
    return ret;
  }
  if (request_id.sequence_number != state->goal_sequence) {
    return RCL_RET_OK;
  }
  printf(
    "VelaROS ACTION CLIENT GOAL: accepted=%s\n",
    state->goal_response.accepted ? "yes" : "no");
  if (!state->goal_response.accepted) {
    RCL_SET_ERROR_MSG("host Action server rejected the VelaROS goal");
    return RCL_RET_ERROR;
  }
  memcpy(result_request.goal_id.uuid, state->goal_id, 16);
  return rcl_action_send_result_request(
    state->client, &result_request, &state->result_sequence);
}

static rcl_ret_t take_feedback(velaros_action_client_state_t * state)
{
  rcl_ret_t ret = rcl_action_take_feedback(state->client, &state->feedback);

  if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
    return RCL_RET_OK;
  }
  if (ret != RCL_RET_OK) {
    return ret;
  }
  if (!same_goal_id(state->feedback.goal_id.uuid, state->goal_id)) {
    return RCL_RET_OK;
  }
  ++state->feedback_count;
  printf(
    "VelaROS ACTION CLIENT FEEDBACK: values=%zu\n",
    state->feedback.feedback.sequence.size);
  if (state->cancel_after_feedback && !state->cancel_sent) {
    action_msgs__srv__CancelGoal_Request cancel_request;

    memset(&cancel_request, 0, sizeof(cancel_request));
    memcpy(cancel_request.goal_info.goal_id.uuid, state->goal_id, 16);
    if (rcl_action_send_cancel_request(
        state->client, &cancel_request, &state->cancel_sequence) != RCL_RET_OK) {
      return RCL_RET_ERROR;
    }
    state->cancel_sent = true;
    printf("VelaROS ACTION CLIENT CANCEL SENT\n");
  }
  return RCL_RET_OK;
}

static rcl_ret_t take_status(velaros_action_client_state_t * state)
{
  rcl_ret_t ret = rcl_action_take_status(state->client, &state->status);

  if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
    return RCL_RET_OK;
  }
  if (ret != RCL_RET_OK) {
    return ret;
  }
  printf(
    "VelaROS ACTION CLIENT STATUS: goals=%zu\n",
    state->status.status_list.size);
  return RCL_RET_OK;
}

static rcl_ret_t take_cancel_response(velaros_action_client_state_t * state)
{
  rmw_request_id_t request_id;
  rcl_ret_t ret;

  memset(&request_id, 0, sizeof(request_id));
  ret = rcl_action_take_cancel_response(
    state->client, &request_id, &state->cancel_response);
  if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
    return RCL_RET_OK;
  }
  if (ret != RCL_RET_OK) {
    return ret;
  }
  if (request_id.sequence_number == state->cancel_sequence) {
    printf(
      "VelaROS ACTION CLIENT CANCEL RESPONSE: code=%d goals=%zu\n",
      (int)state->cancel_response.return_code,
      state->cancel_response.goals_canceling.size);
  }
  return RCL_RET_OK;
}

static rcl_ret_t take_result_response(velaros_action_client_state_t * state)
{
  rmw_request_id_t request_id;
  rcl_ret_t ret;

  memset(&request_id, 0, sizeof(request_id));
  ret = rcl_action_take_result_response(
    state->client, &request_id, &state->result_response);
  if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
    return RCL_RET_OK;
  }
  if (ret != RCL_RET_OK) {
    return ret;
  }
  if (request_id.sequence_number != state->result_sequence) {
    return RCL_RET_OK;
  }
  state->result_received = true;
  printf(
    "VelaROS ACTION CLIENT RESULT: status=%d values=%zu\n",
    (int)state->result_response.status,
    state->result_response.result.sequence.size);
  return RCL_RET_OK;
}

static rcl_ret_t action_ready_callback(
  rcl_action_client_t * client,
  bool feedback_ready,
  bool status_ready,
  bool goal_response_ready,
  bool cancel_response_ready,
  bool result_response_ready,
  void * user_data)
{
  velaros_action_client_state_t * state = user_data;

  (void)client;
  if (goal_response_ready && take_goal_response(state) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  if (feedback_ready && take_feedback(state) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  if (status_ready && take_status(state) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  if (cancel_response_ready && take_cancel_response(state) != RCL_RET_OK) {
    return RCL_RET_ERROR;
  }
  return result_response_ready ? take_result_response(state) : RCL_RET_OK;
}

static int parse_order(int argc, char * argv[])
{
  char * end = NULL;
  long order = argc > 1 ? strtol(argv[1], &end, 10) : 8;

  if ((argc > 1 && (end == argv[1] || *end != '\0')) || order < 1 || order > 30) {
    fprintf(stderr, "usage: velaros_action_client [order 1..30] [cancel 0|1] [participant-id]\n");
    return -1;
  }
  return (int)order;
}

static int parse_cancel(int argc, char * argv[])
{
  if (argc < 3 || strcmp(argv[2], "0") == 0) {
    return 0;
  }
  if (strcmp(argv[2], "1") == 0) {
    return 1;
  }
  return -1;
}

static int parse_client_participant_id(int argc, char * argv[])
{
  char * adjusted[3] = {argv[0], NULL, NULL};
  int adjusted_argc = 1;

  if (argc >= 4) {
    adjusted[1] = argv[3];
    adjusted_argc = 2;
  }
  return velaros_parse_participant_id(adjusted_argc, adjusted);
}

int main(int argc, char * argv[])
{
  const int order = parse_order(argc, argv);
  const int cancel = parse_cancel(argc, argv);
  const int participant_id = parse_client_participant_id(argc, argv);
  rcl_allocator_t allocator = rcl_get_default_allocator();
  rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
  rcl_context_t context = rcl_get_zero_initialized_context();
  rcl_node_t node = rcl_get_zero_initialized_node();
  rcl_node_options_t node_options = rcl_node_get_default_options();
  rcl_action_client_t client = rcl_action_get_zero_initialized_client();
  rcl_action_client_options_t client_options = rcl_action_client_get_default_options();
  velaros_executor_t executor = velaros_executor_get_zero_initialized();
  velaros_action_client_state_t state;
  example_interfaces__action__Fibonacci_SendGoal_Request goal_request;
  bool messages_initialized = false;
  int result = 1;

  memset(&state, 0, sizeof(state));
  memset(&goal_request, 0, sizeof(goal_request));
  state.client = &client;
  state.cancel_after_feedback = cancel == 1;
  node_options.use_global_arguments = false;
  node_options.enable_rosout = false;
  if (order < 0 || cancel < 0 || participant_id < 0 ||
      velaros_fibonacci_action_typesupport() == NULL) {
    return 1;
  }
  messages_initialized = true;
  if (!example_interfaces__action__Fibonacci_SendGoal_Response__init(&state.goal_response) ||
      !example_interfaces__action__Fibonacci_GetResult_Response__init(&state.result_response) ||
      !example_interfaces__action__Fibonacci_FeedbackMessage__init(&state.feedback) ||
      !action_msgs__msg__GoalStatusArray__init(&state.status) ||
      !action_msgs__srv__CancelGoal_Response__init(&state.cancel_response)) {
    fprintf(stderr, "VelaROS Action client message init failed\n");
    goto cleanup;
  }
  if (rcl_init_options_init(&init_options, allocator) != RCL_RET_OK ||
      velaros_configure_qemu_interop(&init_options, participant_id) != RCL_RET_OK ||
      rcl_init(0, NULL, &init_options, &context) != RCL_RET_OK ||
      rcl_node_init(&node, "action_client", "/velaros", &context, &node_options) != RCL_RET_OK ||
      rcl_action_client_init(
        &client, &node, velaros_fibonacci_action_typesupport(),
        VELAROS_ACTION_NAME, &client_options) != RCL_RET_OK ||
      velaros_executor_init_with_actions(
        &executor, &context, 0, 0, 0, 0, 1, 0, allocator) != RCL_RET_OK ||
      velaros_executor_add_action_client(
        &executor, &client, action_ready_callback, &state) != RCL_RET_OK) {
    print_rcl_error("VelaROS Action client init");
    goto cleanup;
  }

  printf("VelaROS Action client ready: %s\n", VELAROS_ACTION_NAME);
  for (int retry = 0; retry < VELAROS_ACTION_DISCOVERY_RETRIES; ++retry) {
    bool available = false;
    if (rcl_action_server_is_available(&node, &client, &available) != RCL_RET_OK) {
      print_rcl_error("rcl_action_server_is_available");
      goto cleanup;
    }
    if (available) {
      break;
    }
    usleep(100000);
    if (retry + 1 == VELAROS_ACTION_DISCOVERY_RETRIES) {
      fprintf(stderr, "VelaROS Action server discovery timeout\n");
      goto cleanup;
    }
  }

  for (size_t index = 0; index < sizeof(state.goal_id); ++index) {
    state.goal_id[index] = (uint8_t)(0x40u + index);
  }
  state.goal_id[15] ^= (uint8_t)(order + (cancel ? 0x20 : 0));
  memcpy(goal_request.goal_id.uuid, state.goal_id, 16);
  goal_request.goal.order = order;
  if (rcl_action_send_goal_request(
      &client, &goal_request, &state.goal_sequence) != RCL_RET_OK) {
    print_rcl_error("rcl_action_send_goal_request");
    goto cleanup;
  }
  printf("VelaROS ACTION CLIENT GOAL SENT: order=%d cancel=%d\n", order, cancel);

  for (int spin = 0; spin < 1200 && !state.result_received; ++spin) {
    rcl_ret_t ret = velaros_executor_spin_once(&executor, RCL_MS_TO_NS(50));
    if (ret != RCL_RET_OK && ret != RCL_RET_TIMEOUT) {
      print_rcl_error("VelaROS Action client spin");
      goto cleanup;
    }
  }
  if (!state.result_received) {
    fprintf(stderr, "VelaROS Action result timeout\n");
    goto cleanup;
  }
  result = 0;

cleanup:
  if (velaros_executor_fini(&executor) != RCL_RET_OK) {
    print_rcl_error("velaros_executor_fini");
    result = 1;
  }
  if (client.impl != NULL && rcl_action_client_fini(&client, &node) != RCL_RET_OK) {
    print_rcl_error("rcl_action_client_fini");
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
  if (messages_initialized) {
    action_msgs__srv__CancelGoal_Response__fini(&state.cancel_response);
    action_msgs__msg__GoalStatusArray__fini(&state.status);
    example_interfaces__action__Fibonacci_FeedbackMessage__fini(&state.feedback);
    example_interfaces__action__Fibonacci_GetResult_Response__fini(&state.result_response);
    example_interfaces__action__Fibonacci_SendGoal_Response__fini(&state.goal_response);
  }
  if (result == 0) {
    printf(
      "VelaROS Action client complete: FEEDBACK=%d STATUS=%d\n",
      state.feedback_count, (int)state.result_response.status);
  }
  return result;
}
