/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <chrono>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "std_srvs/srv/set_bool.hpp"

namespace
{

constexpr int kSampleCount = 3;
constexpr int kMatchRetries = 100;

struct SmokeState
{
  rclcpp::Publisher<std_msgs::msg::String> * publisher = nullptr;
  rclcpp::WallTimer * timer = nullptr;
  int timer_callbacks = 0;
  int subscription_callbacks = 0;
  int service_callbacks = 0;
  int client_callbacks = 0;
  int64_t expected_sequence = 0;
  bool failed = false;
};

void on_timer(void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  try {
    ++state.timer_callbacks;
    std_msgs::msg::String message;
    message.data = "VelaROS rclcpp sample #" +
      std::to_string(state.timer_callbacks);
    state.publisher->publish(message);
    if (state.timer_callbacks >= kSampleCount) {
      state.timer->cancel();
    }
  } catch (...) {
    state.failed = true;
  }
}

void on_message(const std_msgs::msg::String & message, void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  ++state.subscription_callbacks;
  const std::string expected = "VelaROS rclcpp sample #" +
    std::to_string(state.subscription_callbacks);
  if (message.data != expected) {
    state.failed = true;
  }
}

rcl_ret_t on_service(
  const std_srvs::srv::SetBool::Request & request,
  std_srvs::srv::SetBool::Response & response,
  void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  ++state.service_callbacks;
  response.success = request.data;
  response.message = request.data ? "bridge enabled" : "bridge disabled";
  return RCL_RET_OK;
}

void on_response(
  const rmw_request_id_t & request_id,
  const std_srvs::srv::SetBool::Response & response,
  void * user_data)
{
  auto & state = *static_cast<SmokeState *>(user_data);
  ++state.client_callbacks;
  if (request_id.sequence_number != state.expected_sequence ||
      !response.success || response.message != "bridge enabled") {
    state.failed = true;
  }
}

int run_smoke()
{
  std::printf("VelaROS rclcpp context init: START\n");
  rclcpp::Context context;
  std::printf("VelaROS rclcpp context init: PASS\n");
  rclcpp::Node node(context, "velaros_rclcpp_smoke", "/velaros");
  std::printf("VelaROS rclcpp node init: PASS\n");
  auto publisher = node.create_publisher<std_msgs::msg::String>(
    "/velaros/rclcpp_smoke", rclcpp::QoS(4).reliable());
  std::printf("VelaROS rclcpp publisher init: PASS\n");
  SmokeState state;
  auto subscription = node.create_subscription<std_msgs::msg::String>(
    "/velaros/rclcpp_smoke", rclcpp::QoS(4).reliable(), on_message, &state);
  std::printf("VelaROS rclcpp subscription init: PASS\n");
  auto service = node.create_service<std_srvs::srv::SetBool>(
    "/velaros/rclcpp_set_bool", on_service, &state);
  std::printf("VelaROS rclcpp service init: PASS\n");
  auto client = node.create_client<std_srvs::srv::SetBool>(
    "/velaros/rclcpp_set_bool", on_response, &state);
  std::printf("VelaROS rclcpp client init: PASS\n");

  bool matched = false;
  for (int retry = 0; retry < kMatchRetries; ++retry) {
    if (publisher->subscription_count() > 0) {
      matched = true;
      break;
    }
    usleep(50000);
  }
  if (!matched) {
    std::fprintf(stderr, "VelaROS rclcpp local endpoint match timed out\n");
    return 1;
  }

  bool service_ready = false;
  for (int retry = 0; retry < kMatchRetries; ++retry) {
    if (client->service_is_ready()) {
      service_ready = true;
      break;
    }
    usleep(50000);
  }
  if (!service_ready) {
    std::fprintf(stderr, "VelaROS rclcpp local service match timed out\n");
    return 1;
  }

  rclcpp::WallTimer timer(
    context, std::chrono::milliseconds(100), on_timer, &state);
  state.publisher = publisher.get();
  state.timer = &timer;

  rclcpp::ExecutorOptions executor_options;
  executor_options.subscriptions = 1;
  executor_options.timers = 1;
  executor_options.clients = 1;
  executor_options.services = 1;
  rclcpp::executors::SingleThreadedExecutor executor(
    context, executor_options);
  executor.add_subscription(*subscription);
  executor.add_timer(timer);
  executor.add_client(*client);
  executor.add_service(*service);

  std_srvs::srv::SetBool::Request request;
  request.data = true;
  state.expected_sequence = client->async_send_request(request);

  for (int iteration = 0;
       iteration < 30 &&
       (state.subscription_callbacks < kSampleCount ||
       state.client_callbacks < 1);
       ++iteration) {
    (void)executor.spin_once(std::chrono::milliseconds(500));
    if (state.failed) {
      std::fprintf(stderr, "VelaROS rclcpp callback validation failed\n");
      return 1;
    }
  }

  if (state.timer_callbacks != kSampleCount ||
      state.subscription_callbacks != kSampleCount ||
      state.service_callbacks != 1 || state.client_callbacks != 1) {
    std::fprintf(
      stderr,
      "VelaROS rclcpp count mismatch: timer=%d subscription=%d service=%d client=%d\n",
      state.timer_callbacks, state.subscription_callbacks,
      state.service_callbacks, state.client_callbacks);
    return 1;
  }

  std::printf(
    "VelaROS rclcpp node: %s/%s\n",
    node.get_namespace(), node.get_name());
  std::printf("VelaROS rclcpp timer callbacks: %d\n", state.timer_callbacks);
  std::printf(
    "VelaROS rclcpp subscription callbacks: %d\n",
    state.subscription_callbacks);
  std::printf("VelaROS rclcpp service callbacks: %d\n", state.service_callbacks);
  std::printf("VelaROS rclcpp client callbacks: %d\n", state.client_callbacks);
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
      std::printf("VelaROS static rclcpp RAII smoke: PASS\n");
    }
    return result;
  } catch (const std::exception & error) {
    std::fprintf(stderr, "VelaROS rclcpp smoke failed: %s\n", error.what());
    return 1;
  }
}
