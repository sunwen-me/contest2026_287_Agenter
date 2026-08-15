/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * VelaROS static rclcpp_action profile.
 *
 * This is a source-level, bounded subset of ROS 2 Lyrical rclcpp_action.  It
 * intentionally replaces futures, shared goal ownership, per-goal threads,
 * and runtime Action type loading with one caller-thread executor, fixed goal
 * slots, function-pointer callbacks, and a compile-time Action whitelist.
 */

#ifndef VELAROS__RCLCPP_ACTION__RCLCPP_ACTION_HPP_
#define VELAROS__RCLCPP_ACTION__RCLCPP_ACTION_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <time.h>

#include "action_msgs/msg/detail/goal_status__struct.h"
#include "action_msgs/msg/detail/goal_status_array__functions.h"
#include "action_msgs/srv/detail/cancel_goal__functions.h"
#ifdef CONFIG_VELAROS_ACTION_FIBONACCI
#include "example_interfaces/action/detail/fibonacci__functions.h"
#endif
#include "velaros_interfaces/action/detail/move_relative__functions.h"
#include "rcl/error_handling.h"
#include "rcl/time.h"
#include "rcl_action/rcl_action.h"
#include "rclcpp/rclcpp.hpp"
#include "velaros/action_typesupport.h"

#ifndef CONFIG_VELAROS_ACTION_MAX_GOALS
#define CONFIG_VELAROS_ACTION_MAX_GOALS 2
#endif

#if defined(CONFIG_VELAROS_ACTION_FIBONACCI) && \
  !defined(CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY)
#define CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY 32
#endif

namespace velaros
{
namespace action
{

/* Development-only interoperability type.  Product builds omit both these
 * traits and the generated Fibonacci functions/type support. */
#ifdef CONFIG_VELAROS_ACTION_FIBONACCI
struct Fibonacci
{
  using Goal = example_interfaces__action__Fibonacci_Goal;
  using Result = example_interfaces__action__Fibonacci_Result;
  using Feedback = example_interfaces__action__Fibonacci_Feedback;
  using SendGoalRequest = example_interfaces__action__Fibonacci_SendGoal_Request;
  using SendGoalResponse = example_interfaces__action__Fibonacci_SendGoal_Response;
  using GetResultRequest = example_interfaces__action__Fibonacci_GetResult_Request;
  using GetResultResponse = example_interfaces__action__Fibonacci_GetResult_Response;
  using FeedbackMessage = example_interfaces__action__Fibonacci_FeedbackMessage;

  static constexpr size_t result_capacity =
    CONFIG_VELAROS_ACTION_SEQUENCE_CAPACITY;

  static const rosidl_action_type_support_t * type_support() noexcept
  {
    return velaros_fibonacci_action_typesupport();
  }

  static bool init_send_goal_response(SendGoalResponse * message) noexcept
  {
    return example_interfaces__action__Fibonacci_SendGoal_Response__init(message);
  }

  static void fini_send_goal_response(SendGoalResponse * message) noexcept
  {
    example_interfaces__action__Fibonacci_SendGoal_Response__fini(message);
  }

  static bool init_result_response(GetResultResponse * message) noexcept
  {
    return example_interfaces__action__Fibonacci_GetResult_Response__init(message);
  }

  static void fini_result_response(GetResultResponse * message) noexcept
  {
    example_interfaces__action__Fibonacci_GetResult_Response__fini(message);
  }

  static bool init_feedback_message(FeedbackMessage * message) noexcept
  {
    return example_interfaces__action__Fibonacci_FeedbackMessage__init(message);
  }

  static void fini_feedback_message(FeedbackMessage * message) noexcept
  {
    example_interfaces__action__Fibonacci_FeedbackMessage__fini(message);
  }

  static void bind_result(
    Result & result, int32_t * storage, size_t capacity) noexcept
  {
    result.sequence.data = storage;
    result.sequence.size = 0;
    result.sequence.capacity = capacity;
  }

  static bool copy_result(const Result & source, Result & destination) noexcept
  {
    if (source.sequence.size > destination.sequence.capacity ||
        (source.sequence.size > 0 && source.sequence.data == nullptr)) {
      return false;
    }
    if (source.sequence.size > 0) {
      std::memcpy(
        destination.sequence.data, source.sequence.data,
        source.sequence.size * sizeof(destination.sequence.data[0]));
    }
    destination.sequence.size = source.sequence.size;
    return true;
  }
};
#endif

/* Fixed-wire product Action.  Result and feedback contain no sequences, so
 * goal slots need no per-goal payload arena beyond the message itself. */
struct MoveRelative
{
  using Goal = velaros_interfaces__action__MoveRelative_Goal;
  using Result = velaros_interfaces__action__MoveRelative_Result;
  using Feedback = velaros_interfaces__action__MoveRelative_Feedback;
  using SendGoalRequest = velaros_interfaces__action__MoveRelative_SendGoal_Request;
  using SendGoalResponse = velaros_interfaces__action__MoveRelative_SendGoal_Response;
  using GetResultRequest = velaros_interfaces__action__MoveRelative_GetResult_Request;
  using GetResultResponse = velaros_interfaces__action__MoveRelative_GetResult_Response;
  using FeedbackMessage = velaros_interfaces__action__MoveRelative_FeedbackMessage;

  static constexpr size_t result_capacity = 0;

  static const rosidl_action_type_support_t * type_support() noexcept
  {
    return velaros_move_relative_action_typesupport();
  }

  static bool init_send_goal_response(SendGoalResponse * message) noexcept
  {
    return velaros_interfaces__action__MoveRelative_SendGoal_Response__init(message);
  }

  static void fini_send_goal_response(SendGoalResponse * message) noexcept
  {
    velaros_interfaces__action__MoveRelative_SendGoal_Response__fini(message);
  }

  static bool init_result_response(GetResultResponse * message) noexcept
  {
    return velaros_interfaces__action__MoveRelative_GetResult_Response__init(message);
  }

  static void fini_result_response(GetResultResponse * message) noexcept
  {
    velaros_interfaces__action__MoveRelative_GetResult_Response__fini(message);
  }

  static bool init_feedback_message(FeedbackMessage * message) noexcept
  {
    return velaros_interfaces__action__MoveRelative_FeedbackMessage__init(message);
  }

  static void fini_feedback_message(FeedbackMessage * message) noexcept
  {
    velaros_interfaces__action__MoveRelative_FeedbackMessage__fini(message);
  }

  static void bind_result(Result &, int32_t *, size_t) noexcept {}

  static bool copy_result(const Result & source, Result & destination) noexcept
  {
    destination = source;
    return true;
  }
};

}  // namespace action
}  // namespace velaros

namespace rclcpp_action
{

using GoalUUID = std::array<uint8_t, 16>;

enum class GoalResponse
{
  REJECT,
  ACCEPT_AND_EXECUTE,
};

enum class CancelResponse
{
  REJECT,
  ACCEPT,
};

enum class ResultCode
{
  UNKNOWN,
  SUCCEEDED,
  ABORTED,
  CANCELED,
};

template<typename ActionT>
class Client;

template<typename ActionT>
class Server;

template<typename ActionT>
class ClientGoalHandle
{
public:
  const GoalUUID & get_goal_id() const noexcept
  {
    return goal_id_;
  }

  bool is_active() const noexcept
  {
    return client_ != nullptr && client_->goal_is_active(goal_id_);
  }

private:
  friend class Client<ActionT>;

  ClientGoalHandle() : client_(nullptr), goal_id_{} {}

  Client<ActionT> * client_;
  GoalUUID goal_id_;
};

template<typename ActionT>
class Client
{
public:
  using Goal = typename ActionT::Goal;
  using Result = typename ActionT::Result;
  using Feedback = typename ActionT::Feedback;
  using GoalHandle = ClientGoalHandle<ActionT>;

  struct WrappedResult
  {
    GoalUUID goal_id;
    ResultCode code;
    const Result & result;
  };

  using GoalResponseCallback = void (*)(GoalHandle * goal_handle, void * user_data);
  using FeedbackCallback = void (*)(
    GoalHandle & goal_handle, const Feedback & feedback, void * user_data);
  using ResultCallback = void (*)(const WrappedResult & result, void * user_data);
  using CancelCallback = void (*)(
    GoalHandle & goal_handle, bool accepted, void * user_data);

  struct SendGoalOptions
  {
    GoalResponseCallback goal_response_callback = nullptr;
    FeedbackCallback feedback_callback = nullptr;
    ResultCallback result_callback = nullptr;
    CancelCallback cancel_callback = nullptr;
    void * user_data = nullptr;
  };

  Client(rclcpp::Node & node, const char * action_name)
  : node_(node),
    client_(rcl_action_get_zero_initialized_client()),
    goal_response_{},
    result_response_{},
    feedback_message_{},
    status_{},
    cancel_response_{},
    handle_{},
    options_{},
    goal_sequence_(0),
    result_sequence_(0),
    cancel_sequence_(0),
    generation_(0),
    active_(false),
    accepted_(false),
    cancel_pending_(false),
    messages_initialized_(false)
  {
    if (action_name == nullptr || ActionT::type_support() == nullptr) {
      throw std::invalid_argument("VelaROS Action client type or name is invalid");
    }
    initialize_messages();
    try {
      rcl_action_client_options_t action_options =
        rcl_action_client_get_default_options();
      rclcpp::detail::throw_if_error(
        rcl_action_client_init(
          &client_, node_.native_handle(), ActionT::type_support(),
          action_name, &action_options),
        "rcl_action_client_init");
    } catch (...) {
      (void)close();
      finalize_messages();
      throw;
    }
  }

  ~Client() noexcept
  {
    (void)close();
    finalize_messages();
  }

  Client(const Client &) = delete;
  Client & operator=(const Client &) = delete;
  Client(Client &&) = delete;
  Client & operator=(Client &&) = delete;

  bool action_server_is_ready()
  {
    bool available = false;
    rclcpp::detail::throw_if_error(
      rcl_action_server_is_available(
        node_.native_handle(), &client_, &available),
      "rcl_action_server_is_available");
    rcl_reset_error();
    return available;
  }

  const GoalHandle & async_send_goal(
    const Goal & goal, const SendGoalOptions & options = SendGoalOptions())
  {
    if (active_) {
      throw std::logic_error("VelaROS Action client goal capacity exhausted");
    }
    reset_messages();
    options_ = options;
    make_goal_id(handle_.goal_id_);
    handle_.client_ = this;

    typename ActionT::SendGoalRequest request{};
    std::memcpy(request.goal_id.uuid, handle_.goal_id_.data(), handle_.goal_id_.size());
    request.goal = goal;
    rclcpp::detail::throw_if_error(
      rcl_action_send_goal_request(&client_, &request, &goal_sequence_),
      "rcl_action_send_goal_request");
    rcl_reset_error();
    active_ = true;
    accepted_ = false;
    cancel_pending_ = false;
    return handle_;
  }

  void async_cancel_goal()
  {
    if (!active_ || !accepted_ || cancel_pending_) {
      throw std::logic_error("VelaROS Action goal is not cancelable");
    }
    action_msgs__srv__CancelGoal_Request request{};
    std::memcpy(
      request.goal_info.goal_id.uuid, handle_.goal_id_.data(),
      handle_.goal_id_.size());
    rclcpp::detail::throw_if_error(
      rcl_action_send_cancel_request(&client_, &request, &cancel_sequence_),
      "rcl_action_send_cancel_request");
    rcl_reset_error();
    cancel_pending_ = true;
  }

  bool goal_is_active(const GoalUUID & goal_id) const noexcept
  {
    return active_ && same_goal_id(goal_id.data(), handle_.goal_id_.data());
  }

  rcl_ret_t close() noexcept
  {
    if (client_.impl == nullptr) {
      return RCL_RET_OK;
    }
    const rcl_ret_t ret = rcl_action_client_fini(&client_, node_.native_handle());
    client_ = rcl_action_get_zero_initialized_client();
    active_ = false;
    accepted_ = false;
    return ret;
  }

  rcl_action_client_t * native_handle() noexcept
  {
    return &client_;
  }

  static rcl_ret_t dispatch(
    rcl_action_client_t *,
    bool feedback_ready,
    bool status_ready,
    bool goal_response_ready,
    bool cancel_response_ready,
    bool result_response_ready,
    void * instance)
  {
    auto * self = static_cast<Client *>(instance);
    if (self == nullptr) {
      RCL_SET_ERROR_MSG("VelaROS Action client dispatch instance is null");
      return RCL_RET_INVALID_ARGUMENT;
    }
    try {
      if (goal_response_ready) {
        rcl_ret_t ret = self->take_goal_response();
        if (ret != RCL_RET_OK) {
          return ret;
        }
      }
      if (feedback_ready) {
        rcl_ret_t ret = self->take_feedback();
        if (ret != RCL_RET_OK) {
          return ret;
        }
      }
      if (status_ready) {
        rcl_ret_t ret = self->take_status();
        if (ret != RCL_RET_OK) {
          return ret;
        }
      }
      if (cancel_response_ready) {
        rcl_ret_t ret = self->take_cancel_response();
        if (ret != RCL_RET_OK) {
          return ret;
        }
      }
      return result_response_ready ? self->take_result_response() : RCL_RET_OK;
    } catch (...) {
      RCL_SET_ERROR_MSG("VelaROS Action client callback threw an exception");
      return RCL_RET_ERROR;
    }
  }

private:
  static bool same_goal_id(const uint8_t * lhs, const uint8_t * rhs) noexcept
  {
    return std::memcmp(lhs, rhs, GoalUUID{}.size()) == 0;
  }

  static ResultCode result_code(int8_t status) noexcept
  {
    switch (status) {
      case action_msgs__msg__GoalStatus__STATUS_SUCCEEDED:
        return ResultCode::SUCCEEDED;
      case action_msgs__msg__GoalStatus__STATUS_ABORTED:
        return ResultCode::ABORTED;
      case action_msgs__msg__GoalStatus__STATUS_CANCELED:
        return ResultCode::CANCELED;
      default:
        return ResultCode::UNKNOWN;
    }
  }

  void make_goal_id(GoalUUID & goal_id) noexcept
  {
    struct timespec now{};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t time_bits =
      (static_cast<uint64_t>(now.tv_sec) << 32) ^
      static_cast<uint64_t>(now.tv_nsec);
    const uint64_t unique_bits =
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)) ^ ++generation_;
    std::memcpy(goal_id.data(), &time_bits, sizeof(time_bits));
    std::memcpy(goal_id.data() + sizeof(time_bits), &unique_bits, sizeof(unique_bits));
    goal_id[6] = static_cast<uint8_t>((goal_id[6] & 0x0fu) | 0x40u);
    goal_id[8] = static_cast<uint8_t>((goal_id[8] & 0x3fu) | 0x80u);
  }

  void initialize_messages()
  {
    bool goal_initialized = false;
    bool result_initialized = false;
    bool feedback_initialized = false;
    bool status_initialized = false;

    goal_initialized = ActionT::init_send_goal_response(&goal_response_);
    if (goal_initialized) {
      result_initialized = ActionT::init_result_response(&result_response_);
    }
    if (result_initialized) {
      feedback_initialized = ActionT::init_feedback_message(&feedback_message_);
    }
    if (feedback_initialized) {
      status_initialized = action_msgs__msg__GoalStatusArray__init(&status_);
    }
    if (status_initialized &&
        action_msgs__srv__CancelGoal_Response__init(&cancel_response_)) {
      messages_initialized_ = true;
      return;
    }
    if (status_initialized) {
      action_msgs__msg__GoalStatusArray__fini(&status_);
    }
    if (feedback_initialized) {
      ActionT::fini_feedback_message(&feedback_message_);
    }
    if (result_initialized) {
      ActionT::fini_result_response(&result_response_);
    }
    if (goal_initialized) {
      ActionT::fini_send_goal_response(&goal_response_);
    }
    throw std::bad_alloc();
  }

  void finalize_messages() noexcept
  {
    if (!messages_initialized_) {
      return;
    }
    action_msgs__srv__CancelGoal_Response__fini(&cancel_response_);
    action_msgs__msg__GoalStatusArray__fini(&status_);
    ActionT::fini_feedback_message(&feedback_message_);
    ActionT::fini_result_response(&result_response_);
    ActionT::fini_send_goal_response(&goal_response_);
    messages_initialized_ = false;
  }

  void reset_messages()
  {
    finalize_messages();
    std::memset(&goal_response_, 0, sizeof(goal_response_));
    std::memset(&result_response_, 0, sizeof(result_response_));
    std::memset(&feedback_message_, 0, sizeof(feedback_message_));
    std::memset(&status_, 0, sizeof(status_));
    std::memset(&cancel_response_, 0, sizeof(cancel_response_));
    initialize_messages();
  }

  rcl_ret_t take_goal_response()
  {
    rmw_request_id_t request_id{};
    const rcl_ret_t ret = rcl_action_take_goal_response(
      &client_, &request_id, &goal_response_);
    if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
      return RCL_RET_OK;
    }
    if (ret != RCL_RET_OK || request_id.sequence_number != goal_sequence_) {
      return ret;
    }
    rcl_reset_error();
    if (!goal_response_.accepted) {
      if (options_.goal_response_callback != nullptr) {
        options_.goal_response_callback(nullptr, options_.user_data);
      }
      active_ = false;
      return RCL_RET_OK;
    }

    accepted_ = true;
    typename ActionT::GetResultRequest result_request{};
    std::memcpy(
      result_request.goal_id.uuid, handle_.goal_id_.data(),
      handle_.goal_id_.size());
    rcl_ret_t send_ret = rcl_action_send_result_request(
      &client_, &result_request, &result_sequence_);
    if (send_ret != RCL_RET_OK) {
      return send_ret;
    }
    rcl_reset_error();
    if (options_.goal_response_callback != nullptr) {
      options_.goal_response_callback(&handle_, options_.user_data);
    }
    return RCL_RET_OK;
  }

  rcl_ret_t take_feedback()
  {
    const rcl_ret_t ret = rcl_action_take_feedback(&client_, &feedback_message_);
    if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
      return RCL_RET_OK;
    }
    if (ret != RCL_RET_OK ||
        !same_goal_id(feedback_message_.goal_id.uuid, handle_.goal_id_.data())) {
      return ret;
    }
    rcl_reset_error();
    if (options_.feedback_callback != nullptr) {
      options_.feedback_callback(
        handle_, feedback_message_.feedback, options_.user_data);
    }
    return RCL_RET_OK;
  }

  rcl_ret_t take_status()
  {
    const rcl_ret_t ret = rcl_action_take_status(&client_, &status_);
    if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
      rcl_reset_error();
      return RCL_RET_OK;
    }
    if (ret == RCL_RET_OK) {
      rcl_reset_error();
    }
    return ret;
  }

  rcl_ret_t take_cancel_response()
  {
    rmw_request_id_t request_id{};
    const rcl_ret_t ret = rcl_action_take_cancel_response(
      &client_, &request_id, &cancel_response_);
    if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
      return RCL_RET_OK;
    }
    if (ret != RCL_RET_OK || request_id.sequence_number != cancel_sequence_) {
      return ret;
    }
    rcl_reset_error();
    cancel_pending_ = false;
    const bool accepted =
      cancel_response_.return_code ==
      action_msgs__srv__CancelGoal_Response__ERROR_NONE;
    if (options_.cancel_callback != nullptr) {
      options_.cancel_callback(handle_, accepted, options_.user_data);
    }
    return RCL_RET_OK;
  }

  rcl_ret_t take_result_response()
  {
    rmw_request_id_t request_id{};
    const rcl_ret_t ret = rcl_action_take_result_response(
      &client_, &request_id, &result_response_);
    if (ret == RCL_RET_ACTION_CLIENT_TAKE_FAILED) {
      return RCL_RET_OK;
    }
    if (ret != RCL_RET_OK || request_id.sequence_number != result_sequence_) {
      return ret;
    }
    rcl_reset_error();
    const WrappedResult wrapped{
      handle_.goal_id_, result_code(result_response_.status),
      result_response_.result};
    if (options_.result_callback != nullptr) {
      options_.result_callback(wrapped, options_.user_data);
    }
    active_ = false;
    accepted_ = false;
    cancel_pending_ = false;
    return RCL_RET_OK;
  }

  rclcpp::Node & node_;
  rcl_action_client_t client_;
  typename ActionT::SendGoalResponse goal_response_;
  typename ActionT::GetResultResponse result_response_;
  typename ActionT::FeedbackMessage feedback_message_;
  action_msgs__msg__GoalStatusArray status_;
  action_msgs__srv__CancelGoal_Response cancel_response_;
  GoalHandle handle_;
  SendGoalOptions options_;
  int64_t goal_sequence_;
  int64_t result_sequence_;
  int64_t cancel_sequence_;
  uint64_t generation_;
  bool active_;
  bool accepted_;
  bool cancel_pending_;
  bool messages_initialized_;
};

template<typename ActionT>
class ServerGoalHandle
{
public:
  using Goal = typename ActionT::Goal;
  using Result = typename ActionT::Result;
  using Feedback = typename ActionT::Feedback;

  const GoalUUID & get_goal_id() const noexcept;
  const Goal & get_goal() const noexcept;
  size_t slot_index() const noexcept;
  bool is_canceling() const;
  void publish_feedback(const Feedback & feedback);
  void succeed(const Result & result);
  void abort(const Result & result);
  void canceled(const Result & result);

private:
  friend class Server<ActionT>;

  ServerGoalHandle(Server<ActionT> & server, size_t index)
  : server_(server), index_(index) {}

  Server<ActionT> & server_;
  size_t index_;
};

template<typename ActionT>
class Server
{
public:
  using Goal = typename ActionT::Goal;
  using Result = typename ActionT::Result;
  using Feedback = typename ActionT::Feedback;
  using GoalHandle = ServerGoalHandle<ActionT>;
  using GoalCallback = GoalResponse (*)(
    const GoalUUID & goal_id, const Goal & goal, void * user_data);
  using CancelCallback = CancelResponse (*)(
    GoalHandle & goal_handle, void * user_data);
  using ExecuteCallback = void (*)(GoalHandle & goal_handle, void * user_data);

  struct Options
  {
    int64_t result_timeout_ns = RCL_S_TO_NS(10);
  };

  Server(
    rclcpp::Node & node,
    const char * action_name,
    GoalCallback goal_callback,
    CancelCallback cancel_callback,
    ExecuteCallback execute_callback,
    void * user_data = nullptr,
    const Options & options = Options())
  : node_(node),
    allocator_(rcl_get_default_allocator()),
    clock_{},
    server_(rcl_action_get_zero_initialized_server()),
    slots_{},
    goal_callback_(goal_callback),
    cancel_callback_(cancel_callback),
    execute_callback_(execute_callback),
    user_data_(user_data),
    clock_initialized_(false)
  {
    if (action_name == nullptr || goal_callback_ == nullptr ||
        cancel_callback_ == nullptr || execute_callback_ == nullptr ||
        ActionT::type_support() == nullptr) {
      throw std::invalid_argument("VelaROS Action server callback, type, or name is invalid");
    }
    for (size_t index = 0; index < slots_.size(); ++index) {
      reset_slot(index);
    }
    rclcpp::detail::throw_if_error(
      rcl_clock_init(RCL_STEADY_TIME, &clock_, &allocator_),
      "rcl_clock_init(Action server)");
    clock_initialized_ = true;
    try {
      rcl_action_server_options_t action_options =
        rcl_action_server_get_default_options();
      action_options.result_timeout.nanoseconds = options.result_timeout_ns;
      rclcpp::detail::throw_if_error(
        rcl_action_server_init(
          &server_, node_.native_handle(), &clock_, ActionT::type_support(),
          action_name, &action_options),
        "rcl_action_server_init");
    } catch (...) {
      (void)close();
      throw;
    }
  }

  ~Server() noexcept
  {
    (void)close();
  }

  Server(const Server &) = delete;
  Server & operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server & operator=(Server &&) = delete;

  rcl_ret_t close() noexcept
  {
    rcl_ret_t first_error = RCL_RET_OK;
    if (server_.impl != nullptr) {
      first_error = rcl_action_server_fini(&server_, node_.native_handle());
      server_ = rcl_action_get_zero_initialized_server();
    }
    if (clock_initialized_) {
      const rcl_ret_t ret = rcl_clock_fini(&clock_);
      if (ret != RCL_RET_OK && first_error == RCL_RET_OK) {
        first_error = ret;
      }
      clock_ = {};
      clock_initialized_ = false;
    }
    return first_error;
  }

  rcl_action_server_t * native_handle() noexcept
  {
    return &server_;
  }

  static rcl_ret_t dispatch(
    rcl_action_server_t *,
    bool goal_request_ready,
    bool cancel_request_ready,
    bool result_request_ready,
    bool goal_expired,
    void * instance)
  {
    auto * self = static_cast<Server *>(instance);
    if (self == nullptr) {
      RCL_SET_ERROR_MSG("VelaROS Action server dispatch instance is null");
      return RCL_RET_INVALID_ARGUMENT;
    }
    try {
      if (goal_request_ready) {
        rcl_ret_t ret = self->handle_goal_request();
        if (ret != RCL_RET_OK) {
          return ret;
        }
      }
      if (cancel_request_ready) {
        rcl_ret_t ret = self->handle_cancel_request();
        if (ret != RCL_RET_OK) {
          return ret;
        }
      }
      if (result_request_ready) {
        rcl_ret_t ret = self->handle_result_request();
        if (ret != RCL_RET_OK) {
          return ret;
        }
      }
      if (goal_expired) {
        rcl_ret_t ret = self->handle_expired_goals();
        if (ret != RCL_RET_OK) {
          return ret;
        }
      }
      return self->progress_goals();
    } catch (...) {
      RCL_SET_ERROR_MSG("VelaROS Action server callback threw an exception");
      return RCL_RET_ERROR;
    }
  }

private:
  friend class ServerGoalHandle<ActionT>;

  struct GoalSlot
  {
    bool used;
    bool result_request_pending;
    GoalUUID goal_id;
    rcl_action_goal_handle_t * native_handle;
    rmw_request_id_t result_request_id;
    Goal goal;
    Result result;
    std::array<int32_t, ActionT::result_capacity> result_storage;
  };

  static bool same_goal_id(const uint8_t * lhs, const uint8_t * rhs) noexcept
  {
    return std::memcmp(lhs, rhs, GoalUUID{}.size()) == 0;
  }

  GoalSlot * find_goal(const uint8_t goal_id[16]) noexcept
  {
    for (auto & slot : slots_) {
      if (slot.used && same_goal_id(slot.goal_id.data(), goal_id)) {
        return &slot;
      }
    }
    return nullptr;
  }

  size_t slot_index(const GoalSlot & slot) const noexcept
  {
    return static_cast<size_t>(&slot - slots_.data());
  }

  GoalSlot * free_goal() noexcept
  {
    for (auto & slot : slots_) {
      if (!slot.used) {
        return &slot;
      }
    }
    return nullptr;
  }

  void reset_slot(size_t index) noexcept
  {
    GoalSlot & slot = slots_[index];
    slot = GoalSlot{};
    ActionT::bind_result(
      slot.result, slot.result_storage.data(), slot.result_storage.size());
  }

  rcl_ret_t publish_status()
  {
    rcl_action_goal_status_array_t status =
      rcl_action_get_zero_initialized_goal_status_array();
    rcl_ret_t ret = rcl_action_get_goal_status_array(&server_, &status);
    if (ret == RCL_RET_OK) {
      rcl_reset_error();
      ret = rcl_action_publish_status(&server_, &status.msg);
    }
    if (ret == RCL_RET_OK) {
      rcl_reset_error();
    }
    if (status.msg.status_list.data != nullptr) {
      const rcl_ret_t fini_ret = rcl_action_goal_status_array_fini(&status);
      if (ret == RCL_RET_OK && fini_ret != RCL_RET_OK) {
        ret = fini_ret;
      }
    }
    return ret;
  }

  rcl_ret_t handle_goal_request()
  {
    typename ActionT::SendGoalRequest request{};
    typename ActionT::SendGoalResponse response{};
    rmw_request_id_t request_id{};
    rcl_ret_t ret = rcl_action_take_goal_request(
      &server_, &request_id, &request);
    if (ret == RCL_RET_ACTION_SERVER_TAKE_FAILED) {
      return RCL_RET_OK;
    }
    if (ret != RCL_RET_OK) {
      return ret;
    }
    rcl_reset_error();

    GoalUUID goal_id{};
    std::memcpy(goal_id.data(), request.goal_id.uuid, goal_id.size());
    GoalSlot * slot = free_goal();
    const bool accepted = slot != nullptr &&
      goal_callback_(goal_id, request.goal, user_data_) ==
      GoalResponse::ACCEPT_AND_EXECUTE;
    if (accepted) {
      rcl_action_goal_info_t goal_info =
        rcl_action_get_zero_initialized_goal_info();
      std::memcpy(goal_info.goal_id.uuid, goal_id.data(), goal_id.size());
      rcl_action_goal_handle_t * native_handle =
        rcl_action_accept_new_goal(&server_, &goal_info);
      if (native_handle != nullptr) {
        rcl_reset_error();
        const size_t index = slot_index(*slot);
        reset_slot(index);
        slot = &slots_[index];
        slot->used = true;
        slot->goal_id = goal_id;
        slot->native_handle = native_handle;
        slot->goal = request.goal;
        rcl_action_goal_info_t accepted_info =
          rcl_action_get_zero_initialized_goal_info();
        ret = rcl_action_update_goal_state(native_handle, GOAL_EVENT_EXECUTE);
        if (ret == RCL_RET_OK) {
          rcl_reset_error();
          ret = rcl_action_goal_handle_get_info(native_handle, &accepted_info);
        }
        if (ret != RCL_RET_OK) {
          return ret;
        }
        rcl_reset_error();
        response.accepted = true;
        response.stamp = accepted_info.stamp;
      }
    }

    ret = rcl_action_send_goal_response(&server_, &request_id, &response);
    if (ret == RCL_RET_OK && response.accepted) {
      rcl_reset_error();
      ret = publish_status();
    }
    return ret;
  }

  rcl_ret_t handle_cancel_request()
  {
    rcl_action_cancel_request_t request =
      rcl_action_get_zero_initialized_cancel_request();
    rcl_action_cancel_response_t response =
      rcl_action_get_zero_initialized_cancel_response();
    rmw_request_id_t request_id{};
    rcl_ret_t ret = rcl_action_take_cancel_request(
      &server_, &request_id, &request);
    if (ret == RCL_RET_ACTION_SERVER_TAKE_FAILED) {
      return RCL_RET_OK;
    }
    if (ret != RCL_RET_OK) {
      return ret;
    }
    rcl_reset_error();
    ret = rcl_action_process_cancel_request(&server_, &request, &response);
    if (ret != RCL_RET_OK) {
      return ret;
    }
    rcl_reset_error();

    size_t accepted_count = 0;
    for (size_t index = 0; index < response.msg.goals_canceling.size; ++index) {
      GoalSlot * slot = find_goal(
        response.msg.goals_canceling.data[index].goal_id.uuid);
      if (slot == nullptr) {
        continue;
      }
      GoalHandle handle(*this, slot_index(*slot));
      if (cancel_callback_(handle, user_data_) != CancelResponse::ACCEPT) {
        continue;
      }
      ret = rcl_action_update_goal_state(
        slot->native_handle, GOAL_EVENT_CANCEL_GOAL);
      if (ret != RCL_RET_OK) {
        break;
      }
      rcl_reset_error();
      if (accepted_count != index) {
        response.msg.goals_canceling.data[accepted_count] =
          response.msg.goals_canceling.data[index];
      }
      ++accepted_count;
    }
    response.msg.goals_canceling.size = accepted_count;
    if (accepted_count == 0) {
      response.msg.return_code =
        action_msgs__srv__CancelGoal_Response__ERROR_REJECTED;
    }
    if (ret == RCL_RET_OK) {
      ret = rcl_action_send_cancel_response(
        &server_, &request_id, &response.msg);
    }
    if (ret == RCL_RET_OK) {
      rcl_reset_error();
    }
    const rcl_ret_t fini_ret = rcl_action_cancel_response_fini(&response);
    if (ret == RCL_RET_OK && fini_ret != RCL_RET_OK) {
      ret = fini_ret;
    }
    if (ret == RCL_RET_OK && accepted_count > 0) {
      ret = publish_status();
    }
    return ret;
  }

  rcl_ret_t handle_result_request()
  {
    typename ActionT::GetResultRequest request{};
    rmw_request_id_t request_id{};
    rcl_ret_t ret = rcl_action_take_result_request(
      &server_, &request_id, &request);
    if (ret == RCL_RET_ACTION_SERVER_TAKE_FAILED) {
      return RCL_RET_OK;
    }
    if (ret != RCL_RET_OK) {
      return ret;
    }
    rcl_reset_error();
    GoalSlot * slot = find_goal(request.goal_id.uuid);
    if (slot == nullptr) {
      return send_result(nullptr, request_id);
    }
    rcl_action_goal_state_t status = GOAL_STATE_UNKNOWN;
    ret = rcl_action_goal_handle_get_status(slot->native_handle, &status);
    if (ret != RCL_RET_OK) {
      return ret;
    }
    rcl_reset_error();
    if (status == GOAL_STATE_SUCCEEDED || status == GOAL_STATE_CANCELED ||
        status == GOAL_STATE_ABORTED) {
      return send_result(slot, request_id);
    }
    if (slot->result_request_pending) {
      RCL_SET_ERROR_MSG("VelaROS Action result waiter capacity exhausted");
      return RCL_RET_WAIT_SET_FULL;
    }
    slot->result_request_pending = true;
    slot->result_request_id = request_id;
    return RCL_RET_OK;
  }

  rcl_ret_t send_result(GoalSlot * slot, rmw_request_id_t request_id)
  {
    typename ActionT::GetResultResponse response{};
    rcl_action_goal_state_t status = GOAL_STATE_UNKNOWN;
    if (slot != nullptr) {
      rcl_ret_t ret = rcl_action_goal_handle_get_status(
        slot->native_handle, &status);
      if (ret != RCL_RET_OK) {
        return ret;
      }
      rcl_reset_error();
      response.result = slot->result;
    }
    response.status = static_cast<int8_t>(status);
    rcl_ret_t ret = rcl_action_send_result_response(
      &server_, &request_id, &response);
    if (ret == RCL_RET_OK && slot != nullptr) {
      rcl_reset_error();
      slot->result_request_pending = false;
    }
    return ret;
  }

  rcl_ret_t handle_expired_goals()
  {
    std::array<rcl_action_goal_info_t, CONFIG_VELAROS_ACTION_MAX_GOALS> expired{};
    size_t expired_count = 0;
    rcl_ret_t ret = rcl_action_expire_goals(
      &server_, expired.data(), expired.size(), &expired_count);
    if (ret != RCL_RET_OK) {
      return ret;
    }
    rcl_reset_error();
    for (size_t index = 0; index < expired_count; ++index) {
      GoalSlot * slot = find_goal(expired[index].goal_id.uuid);
      if (slot != nullptr) {
        reset_slot(slot_index(*slot));
      }
    }
    return RCL_RET_OK;
  }

  rcl_ret_t progress_goals()
  {
    for (size_t index = 0; index < slots_.size(); ++index) {
      GoalSlot & slot = slots_[index];
      if (!slot.used || slot.native_handle == nullptr) {
        continue;
      }
      rcl_action_goal_state_t status = GOAL_STATE_UNKNOWN;
      rcl_ret_t ret = rcl_action_goal_handle_get_status(
        slot.native_handle, &status);
      if (ret != RCL_RET_OK) {
        return ret;
      }
      rcl_reset_error();
      if (status == GOAL_STATE_EXECUTING || status == GOAL_STATE_CANCELING) {
        GoalHandle handle(*this, index);
        execute_callback_(handle, user_data_);
      }
    }
    return RCL_RET_OK;
  }

  const GoalUUID & goal_id(size_t index) const noexcept
  {
    return slots_[index].goal_id;
  }

  const Goal & goal(size_t index) const noexcept
  {
    return slots_[index].goal;
  }

  bool is_canceling(size_t index) const
  {
    rcl_action_goal_state_t status = GOAL_STATE_UNKNOWN;
    rclcpp::detail::throw_if_error(
      rcl_action_goal_handle_get_status(
        slots_[index].native_handle, &status),
      "rcl_action_goal_handle_get_status");
    rcl_reset_error();
    return status == GOAL_STATE_CANCELING;
  }

  void publish_feedback(size_t index, const Feedback & feedback)
  {
    typename ActionT::FeedbackMessage message{};
    std::memcpy(
      message.goal_id.uuid, slots_[index].goal_id.data(),
      slots_[index].goal_id.size());
    message.feedback = feedback;
    rclcpp::detail::throw_if_error(
      rcl_action_publish_feedback(&server_, &message),
      "rcl_action_publish_feedback");
    rcl_reset_error();
  }

  void complete(size_t index, const Result & result, rcl_action_goal_event_t event)
  {
    GoalSlot & slot = slots_[index];
    if (!ActionT::copy_result(result, slot.result)) {
      throw std::length_error("VelaROS Action result exceeds static capacity");
    }
    rclcpp::detail::throw_if_error(
      rcl_action_update_goal_state(slot.native_handle, event),
      "rcl_action_update_goal_state");
    rcl_reset_error();
    rclcpp::detail::throw_if_error(
      rcl_action_notify_goal_done(&server_), "rcl_action_notify_goal_done");
    rcl_reset_error();
    rclcpp::detail::throw_if_error(publish_status(), "rcl_action_publish_status");
    rcl_reset_error();
    if (slot.result_request_pending) {
      rclcpp::detail::throw_if_error(
        send_result(&slot, slot.result_request_id),
        "rcl_action_send_result_response");
    }
  }

  rclcpp::Node & node_;
  rcl_allocator_t allocator_;
  rcl_clock_t clock_;
  rcl_action_server_t server_;
  std::array<GoalSlot, CONFIG_VELAROS_ACTION_MAX_GOALS> slots_;
  GoalCallback goal_callback_;
  CancelCallback cancel_callback_;
  ExecuteCallback execute_callback_;
  void * user_data_;
  bool clock_initialized_;
};

template<typename ActionT>
const GoalUUID & ServerGoalHandle<ActionT>::get_goal_id() const noexcept
{
  return server_.goal_id(index_);
}

template<typename ActionT>
const typename ActionT::Goal & ServerGoalHandle<ActionT>::get_goal() const noexcept
{
  return server_.goal(index_);
}

template<typename ActionT>
size_t ServerGoalHandle<ActionT>::slot_index() const noexcept
{
  return index_;
}

template<typename ActionT>
bool ServerGoalHandle<ActionT>::is_canceling() const
{
  return server_.is_canceling(index_);
}

template<typename ActionT>
void ServerGoalHandle<ActionT>::publish_feedback(const Feedback & feedback)
{
  server_.publish_feedback(index_, feedback);
}

template<typename ActionT>
void ServerGoalHandle<ActionT>::succeed(const Result & result)
{
  server_.complete(index_, result, GOAL_EVENT_SUCCEED);
}

template<typename ActionT>
void ServerGoalHandle<ActionT>::abort(const Result & result)
{
  server_.complete(index_, result, GOAL_EVENT_ABORT);
}

template<typename ActionT>
void ServerGoalHandle<ActionT>::canceled(const Result & result)
{
  server_.complete(index_, result, GOAL_EVENT_CANCELED);
}

template<typename ActionT>
std::unique_ptr<Client<ActionT>> create_client(
  rclcpp::Node & node, const char * action_name)
{
  return std::unique_ptr<Client<ActionT>>(
    new Client<ActionT>(node, action_name));
}

template<typename ActionT>
std::unique_ptr<Server<ActionT>> create_server(
  rclcpp::Node & node,
  const char * action_name,
  typename Server<ActionT>::GoalCallback goal_callback,
  typename Server<ActionT>::CancelCallback cancel_callback,
  typename Server<ActionT>::ExecuteCallback execute_callback,
  void * user_data = nullptr,
  const typename Server<ActionT>::Options & options =
    typename Server<ActionT>::Options())
{
  return std::unique_ptr<Server<ActionT>>(
    new Server<ActionT>(
      node, action_name, goal_callback, cancel_callback,
      execute_callback, user_data, options));
}

}  // namespace rclcpp_action

#endif  // VELAROS__RCLCPP_ACTION__RCLCPP_ACTION_HPP_
