/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rclcpp/rclcpp.hpp"

#include <utility>

#include "rcl/error_handling.h"
#include "velaros_qemu_init.h"

namespace rclcpp
{

Error::Error(
  rcl_ret_t code, const std::string & operation, const std::string & detail)
: std::runtime_error(
    detail.empty() ? operation : operation + ": " + detail),
  code_(code)
{
}

namespace detail
{

void throw_if_error(rcl_ret_t ret, const char * operation)
{
  if (ret == RCL_RET_OK) {
    return;
  }

  const rcl_error_string_t error = rcl_get_error_string();
  const std::string message(error.str);
  rcl_reset_error();
  throw Error(ret, operation == nullptr ? "rcl operation" : operation, message);
}

}  // namespace detail

QoS::QoS(size_t depth) noexcept
: profile_(rmw_qos_profile_default)
{
  profile_.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  profile_.depth = depth;
}

QoS & QoS::best_effort() noexcept
{
  profile_.reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  return *this;
}

QoS & QoS::reliable() noexcept
{
  profile_.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
  return *this;
}

QoS & QoS::durability_volatile() noexcept
{
  profile_.durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
  return *this;
}

QoS & QoS::transient_local() noexcept
{
  profile_.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
  return *this;
}

Context::Context(const ContextOptions & options)
: Context(0, nullptr, options)
{
}

Context::Context(
  int argc, const char * const * argv, const ContextOptions & options)
: allocator_(rcl_get_default_allocator()),
  init_options_(rcl_get_zero_initialized_init_options()),
  context_(rcl_get_zero_initialized_context()),
  init_options_initialized_(false),
  context_initialized_(false)
{
  detail::throw_if_error(
    rcl_init_options_init(&init_options_, allocator_),
    "rcl_init_options_init");
  init_options_initialized_ = true;

  try {
    if (options.qemu_interop) {
      detail::throw_if_error(
        velaros_configure_qemu_interop(
          &init_options_, options.qemu_participant_id),
        "velaros_configure_qemu_interop");
    }
    if (options.domain_id != RCL_DEFAULT_DOMAIN_ID) {
      detail::throw_if_error(
        rcl_init_options_set_domain_id(&init_options_, options.domain_id),
        "rcl_init_options_set_domain_id");
    }
    detail::throw_if_error(
      rcl_init(argc, argv, &init_options_, &context_), "rcl_init");
    context_initialized_ = true;
  } catch (...) {
    (void)close();
    throw;
  }
}

Context::~Context() noexcept
{
  (void)close();
}

bool Context::ok() const noexcept
{
  return context_initialized_ && rcl_context_is_valid(&context_);
}

rcl_ret_t Context::close() noexcept
{
  rcl_ret_t first_error = RCL_RET_OK;
  if (context_initialized_) {
    if (rcl_context_is_valid(&context_)) {
      const rcl_ret_t ret = rcl_shutdown(&context_);
      if (ret != RCL_RET_OK && first_error == RCL_RET_OK) {
        first_error = ret;
      }
    }
    const rcl_ret_t ret = rcl_context_fini(&context_);
    if (ret != RCL_RET_OK && first_error == RCL_RET_OK) {
      first_error = ret;
    }
    context_ = rcl_get_zero_initialized_context();
    context_initialized_ = false;
  }
  if (init_options_initialized_) {
    const rcl_ret_t ret = rcl_init_options_fini(&init_options_);
    if (ret != RCL_RET_OK && first_error == RCL_RET_OK) {
      first_error = ret;
    }
    init_options_ = rcl_get_zero_initialized_init_options();
    init_options_initialized_ = false;
  }
  return first_error;
}

Node::Node(
  Context & context, const char * node_name, const char * node_namespace)
: context_(context), node_(rcl_get_zero_initialized_node())
{
  rcl_node_options_t options = rcl_node_get_default_options();
  options.use_global_arguments = false;
  options.enable_rosout = false;
  detail::throw_if_error(
    rcl_node_init(
      &node_, node_name, node_namespace, context_.native_handle(), &options),
    "rcl_node_init");
}

Node::~Node() noexcept
{
  (void)close();
}

const char * Node::get_name() const noexcept
{
  return node_.impl == nullptr ? "" : rcl_node_get_name(&node_);
}

const char * Node::get_namespace() const noexcept
{
  return node_.impl == nullptr ? "" : rcl_node_get_namespace(&node_);
}

rcl_ret_t Node::close() noexcept
{
  if (node_.impl == nullptr) {
    return RCL_RET_OK;
  }
  const rcl_ret_t ret = rcl_node_fini(&node_);
  node_ = rcl_get_zero_initialized_node();
  return ret;
}

WallTimer::WallTimer(
  Context & context,
  std::chrono::nanoseconds period,
  Callback callback,
  void * user_data,
  bool autostart)
: allocator_(rcl_get_default_allocator()),
  clock_(),
  timer_(rcl_get_zero_initialized_timer()),
  callback_(callback),
  user_data_(user_data),
  clock_initialized_(false)
{
  if (callback_ == nullptr || period.count() <= 0) {
    throw std::invalid_argument("VelaROS wall timer callback or period is invalid");
  }

  detail::throw_if_error(
    rcl_clock_init(RCL_STEADY_TIME, &clock_, &allocator_), "rcl_clock_init");
  clock_initialized_ = true;
  try {
    detail::throw_if_error(
      rcl_timer_init2(
        &timer_, &clock_, context.native_handle(), period.count(),
        &WallTimer::dispatch, allocator_, autostart),
      "rcl_timer_init2");
    const uintptr_t previous_callback_data = rcl_timer_exchange_callback_data(
      &timer_, reinterpret_cast<uintptr_t>(this));
    (void)previous_callback_data;
  } catch (...) {
    (void)close();
    throw;
  }
}

WallTimer::~WallTimer() noexcept
{
  (void)close();
}

void WallTimer::dispatch(rcl_timer_t *, int64_t, uintptr_t callback_data)
{
  auto * self = reinterpret_cast<WallTimer *>(callback_data);
  self->callback_(self->user_data_);
}

void WallTimer::cancel()
{
  detail::throw_if_error(rcl_timer_cancel(&timer_), "rcl_timer_cancel");
}

void WallTimer::reset()
{
  detail::throw_if_error(rcl_timer_reset(&timer_), "rcl_timer_reset");
}

rcl_ret_t WallTimer::close() noexcept
{
  rcl_ret_t first_error = RCL_RET_OK;
  if (timer_.impl != nullptr) {
    first_error = rcl_timer_fini(&timer_);
    timer_ = rcl_get_zero_initialized_timer();
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

namespace executors
{

SingleThreadedExecutor::SingleThreadedExecutor(
  Context & context, const ExecutorOptions & options)
: executor_(velaros_executor_get_zero_initialized())
{
#ifdef CONFIG_VELAROS_ACTIONS
  detail::throw_if_error(
    velaros_executor_init_with_actions(
      &executor_, context.native_handle(), options.subscriptions,
      options.timers, options.clients, options.services,
      options.action_clients, options.action_servers,
      rcl_get_default_allocator()),
    "velaros_executor_init_with_actions");
#else
  detail::throw_if_error(
    velaros_executor_init(
      &executor_, context.native_handle(), options.subscriptions,
      options.timers, options.clients, options.services,
      rcl_get_default_allocator()),
    "velaros_executor_init");
#endif
}

SingleThreadedExecutor::~SingleThreadedExecutor() noexcept
{
  (void)close();
}

void SingleThreadedExecutor::add_timer(WallTimer & timer)
{
  detail::throw_if_error(
    velaros_executor_add_timer(&executor_, timer.native_handle()),
    "velaros_executor_add_timer");
}

bool SingleThreadedExecutor::spin_once(std::chrono::nanoseconds timeout)
{
  const rcl_ret_t ret = velaros_executor_spin_once(&executor_, timeout.count());
  if (ret == RCL_RET_TIMEOUT) {
    return false;
  }
  detail::throw_if_error(ret, "velaros_executor_spin_once");
  return true;
}

rcl_ret_t SingleThreadedExecutor::close() noexcept
{
  return velaros_executor_fini(&executor_);
}

}  // namespace executors

}  // namespace rclcpp
