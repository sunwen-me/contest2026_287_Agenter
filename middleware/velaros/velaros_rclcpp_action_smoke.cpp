/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <chrono>
#include <cstdio>
#include <unistd.h>

#include "rclcpp_action/rclcpp_action.hpp"

namespace
{

using Fibonacci = velaros::action::Fibonacci;
using ActionClient = rclcpp_action::Client<Fibonacci>;
using ServerGoalHandle = rclcpp_action::ServerGoalHandle<Fibonacci>;

constexpr char kActionName[] = "/velaros/rclcpp_fibonacci";
constexpr int kDiscoveryRetries = 100;

struct GoalExecution
{
  int32_t sequence[CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY];
  size_t size;
  bool initialized;
};

struct SmokeState
{
  ActionClient * client;
  GoalExecution goals[CONFIG_VELAROS_ACTION_MAX_GOALS];
  int goal_responses;
  int feedback_callbacks;
  int success_results;
  int canceled_results;
  int cancel_responses;
  bool cancel_phase;
  bool cancel_sent;
  bool failed;
};

rclcpp_action::GoalResponse on_goal(
  const rclcpp_action::GoalUUID &,
  const Fibonacci::Goal & goal,
  void *)
{
  return goal.order >= 1 &&
    goal.order < CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY ?
    rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE :
    rclcpp_action::GoalResponse::REJECT;
}

rclcpp_action::CancelResponse on_cancel(ServerGoalHandle &, void *)
{
  return rclcpp_action::CancelResponse::ACCEPT;
}

void on_execute(ServerGoalHandle & handle, void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  GoalExecution & execution = state.goals[handle.slot_index()];
  if (!execution.initialized) {
    execution.sequence[0] = 0;
    execution.sequence[1] = 1;
    execution.size = 2;
    execution.initialized = true;
  }

  Fibonacci::Result result{};
  result.sequence.data = execution.sequence;
  result.sequence.size = execution.size;
  result.sequence.capacity = CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY;
  if (handle.is_canceling()) {
    handle.canceled(result);
    return;
  }

  if (execution.size <= static_cast<size_t>(handle.get_goal().order)) {
    const size_t index = execution.size;
    execution.sequence[index] =
      execution.sequence[index - 1] + execution.sequence[index - 2];
    ++execution.size;
    Fibonacci::Feedback feedback{};
    feedback.sequence.data = execution.sequence;
    feedback.sequence.size = execution.size;
    feedback.sequence.capacity = CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY;
    handle.publish_feedback(feedback);
    return;
  }

  result.sequence.size = execution.size;
  handle.succeed(result);
}

void on_goal_response(
  ActionClient::GoalHandle * goal_handle, void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  if (goal_handle == nullptr) {
    state.failed = true;
    return;
  }
  ++state.goal_responses;
}

void on_feedback(
  ActionClient::GoalHandle &,
  const Fibonacci::Feedback & feedback,
  void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  ++state.feedback_callbacks;
  if (feedback.sequence.size < 3 ||
      feedback.sequence.size > CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY) {
    state.failed = true;
  }
  if (state.cancel_phase && !state.cancel_sent) {
    state.client->async_cancel_goal();
    state.cancel_sent = true;
  }
}

void on_result(const ActionClient::WrappedResult & result, void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  if (result.result.sequence.size < 2 ||
      result.result.sequence.size > CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY) {
    state.failed = true;
  }
  if (result.code == rclcpp_action::ResultCode::SUCCEEDED) {
    ++state.success_results;
  } else if (result.code == rclcpp_action::ResultCode::CANCELED) {
    ++state.canceled_results;
  } else {
    state.failed = true;
  }
}

void on_cancel_response(
  ActionClient::GoalHandle &, bool accepted, void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  if (!accepted) {
    state.failed = true;
    return;
  }
  ++state.cancel_responses;
}

bool spin_until(
  rclcpp::executors::SingleThreadedExecutor & executor,
  bool (*predicate)(const SmokeState &),
  SmokeState & state,
  int iterations)
{
  for (int index = 0; index < iterations && !predicate(state); ++index) {
    (void)executor.spin_once(std::chrono::milliseconds(50));
    if (state.failed) {
      return false;
    }
  }
  return predicate(state) && !state.failed;
}

bool success_complete(const SmokeState & state)
{
  return state.success_results == 1;
}

bool cancel_complete(const SmokeState & state)
{
  return state.canceled_results == 1;
}

int run_smoke()
{
  rclcpp::Context context;
  rclcpp::Node node(context, "velaros_rclcpp_action_smoke", "/velaros");
  SmokeState state{};
  auto server = rclcpp_action::create_server<Fibonacci>(
    node, kActionName, on_goal, on_cancel, on_execute, &state);
  auto client = rclcpp_action::create_client<Fibonacci>(node, kActionName);
  state.client = client.get();

  rclcpp::ExecutorOptions executor_options;
  executor_options.action_clients = 1;
  executor_options.action_servers = 1;
  rclcpp::executors::SingleThreadedExecutor executor(context, executor_options);
  executor.add_action_client(*client);
  executor.add_action_server(*server);

  bool matched = false;
  for (int retry = 0; retry < kDiscoveryRetries; ++retry) {
    if (client->action_server_is_ready()) {
      matched = true;
      break;
    }
    usleep(50000);
  }
  if (!matched) {
    std::fprintf(stderr, "VelaROS rclcpp Action discovery timed out\n");
    return 1;
  }

  ActionClient::SendGoalOptions options;
  options.goal_response_callback = on_goal_response;
  options.feedback_callback = on_feedback;
  options.result_callback = on_result;
  options.cancel_callback = on_cancel_response;
  options.user_data = &state;

  Fibonacci::Goal success_goal{};
  success_goal.order = 6;
  (void)client->async_send_goal(success_goal, options);
  if (!spin_until(executor, success_complete, state, 200)) {
    std::fprintf(stderr, "VelaROS rclcpp Action success path timed out\n");
    return 1;
  }
  std::printf("VelaROS rclcpp Action success result: PASS\n");

  state.cancel_phase = true;
  Fibonacci::Goal cancel_goal{};
  cancel_goal.order = 20;
  (void)client->async_send_goal(cancel_goal, options);
  if (!spin_until(executor, cancel_complete, state, 200)) {
    std::fprintf(stderr, "VelaROS rclcpp Action cancel path timed out\n");
    return 1;
  }
  std::printf("VelaROS rclcpp Action cancel result: PASS\n");

  if (state.goal_responses != 2 || state.success_results != 1 ||
      state.canceled_results != 1 || state.cancel_responses != 1 ||
      state.feedback_callbacks < 2 || !state.cancel_sent) {
    std::fprintf(
      stderr,
      "VelaROS rclcpp Action count mismatch: goals=%d feedback=%d "
      "success=%d canceled=%d cancel_responses=%d\n",
      state.goal_responses, state.feedback_callbacks,
      state.success_results, state.canceled_results,
      state.cancel_responses);
    return 1;
  }

  std::printf("VelaROS rclcpp Action goal callbacks: %d\n", state.goal_responses);
  std::printf("VelaROS rclcpp Action feedback callbacks: %d\n", state.feedback_callbacks);
  std::printf("VelaROS rclcpp Action cancel callbacks: %d\n", state.cancel_responses);
  return 0;
}

}  // namespace

#if defined(__NuttX__)
extern "C"
#endif
int main(int argc, char * argv[])
{
  (void)argc;
  (void)argv;
  try {
    const int result = run_smoke();
    if (result == 0) {
      std::printf("VelaROS static rclcpp Action RAII smoke: PASS\n");
    }
    return result;
  } catch (const std::exception & error) {
    std::fprintf(stderr, "VelaROS rclcpp Action smoke failed: %s\n", error.what());
    return 1;
  }
}
