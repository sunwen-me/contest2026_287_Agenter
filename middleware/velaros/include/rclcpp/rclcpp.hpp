/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * VelaROS static rclcpp profile.
 *
 * This is an intentionally small, source-level subset of the ROS 2 Lyrical
 * rclcpp 32.0.0 API.  It keeps deterministic single-thread execution and
 * direct rcl ownership; it is not ABI compatible with the complete upstream
 * rclcpp library.
 */

#ifndef VELAROS__RCLCPP__RCLCPP_HPP_
#define VELAROS__RCLCPP__RCLCPP_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "rcl/allocator.h"
#include "rcl/client.h"
#include "rcl/context.h"
#include "rcl/domain_id.h"
#include "rcl/graph.h"
#include "rcl/init.h"
#include "rcl/init_options.h"
#include "rcl/node.h"
#include "rcl/publisher.h"
#include "rcl/service.h"
#include "rcl/subscription.h"
#include "rcl/time.h"
#include "rcl/timer.h"
#include "rcl/types.h"
#include "rmw/qos_profiles.h"
#include "rosidl_typesupport_fastrtps_cpp/message_type_support_decl.hpp"
#include "rosidl_typesupport_fastrtps_cpp/service_type_support_decl.hpp"
#include "velaros/executor.h"

namespace rclcpp
{

class Error : public std::runtime_error
{
public:
  Error(rcl_ret_t code, const std::string & operation, const std::string & detail);

  rcl_ret_t code() const noexcept
  {
    return code_;
  }

private:
  rcl_ret_t code_;
};

namespace detail
{

void throw_if_error(rcl_ret_t ret, const char * operation);

template<typename MessageT>
const rosidl_message_type_support_t * message_type_support()
{
  return rosidl_typesupport_fastrtps_cpp::
    get_message_type_support_handle<MessageT>();
}

template<typename ServiceT>
const rosidl_service_type_support_t * service_type_support()
{
  return rosidl_typesupport_fastrtps_cpp::
    get_service_type_support_handle<ServiceT>();
}

}  // namespace detail

class QoS
{
public:
  explicit QoS(size_t depth = 10) noexcept;

  QoS & best_effort() noexcept;
  QoS & reliable() noexcept;
  QoS & durability_volatile() noexcept;
  QoS & transient_local() noexcept;

  const rmw_qos_profile_t & get_rmw_qos_profile() const noexcept
  {
    return profile_;
  }

private:
  rmw_qos_profile_t profile_;
};

struct ContextOptions
{
  size_t domain_id = RCL_DEFAULT_DOMAIN_ID;
  bool qemu_interop = false;
  int qemu_participant_id = 0;
};

class Context
{
public:
  explicit Context(const ContextOptions & options = ContextOptions());
  Context(int argc, const char * const * argv, const ContextOptions & options = ContextOptions());
  ~Context() noexcept;

  Context(const Context &) = delete;
  Context & operator=(const Context &) = delete;
  Context(Context &&) = delete;
  Context & operator=(Context &&) = delete;

  bool ok() const noexcept;
  rcl_ret_t close() noexcept;

  rcl_context_t * native_handle() noexcept
  {
    return &context_;
  }

private:
  rcl_allocator_t allocator_;
  rcl_init_options_t init_options_;
  rcl_context_t context_;
  bool init_options_initialized_;
  bool context_initialized_;
};

template<typename MessageT>
class Publisher;

template<typename MessageT>
class Subscription;

template<typename ServiceT>
class Client;

template<typename ServiceT>
class Service;

class Node
{
public:
  Node(Context & context, const char * node_name, const char * node_namespace = "");
  ~Node() noexcept;

  Node(const Node &) = delete;
  Node & operator=(const Node &) = delete;
  Node(Node &&) = delete;
  Node & operator=(Node &&) = delete;

  const char * get_name() const noexcept;
  const char * get_namespace() const noexcept;
  rcl_ret_t close() noexcept;

  Context & context() noexcept
  {
    return context_;
  }

  rcl_node_t * native_handle() noexcept
  {
    return &node_;
  }

  template<typename MessageT>
  std::unique_ptr<Publisher<MessageT>> create_publisher(
    const char * topic_name, const QoS & qos = QoS());

  template<typename MessageT>
  std::unique_ptr<Subscription<MessageT>> create_subscription(
    const char * topic_name,
    const QoS & qos,
    typename Subscription<MessageT>::Callback callback,
    void * user_data = nullptr);

  template<typename ServiceT>
  std::unique_ptr<Client<ServiceT>> create_client(
    const char * service_name,
    typename Client<ServiceT>::Callback callback = nullptr,
    void * user_data = nullptr);

  template<typename ServiceT>
  std::unique_ptr<Service<ServiceT>> create_service(
    const char * service_name,
    typename Service<ServiceT>::Callback callback,
    void * user_data = nullptr);

private:
  Context & context_;
  rcl_node_t node_;
};

template<typename MessageT>
class Publisher
{
public:
  Publisher(Node & node, const char * topic_name, const QoS & qos = QoS())
  : node_(node), publisher_(rcl_get_zero_initialized_publisher())
  {
    rcl_publisher_options_t options = rcl_publisher_get_default_options();
    options.qos = qos.get_rmw_qos_profile();
    detail::throw_if_error(
      rcl_publisher_init(
        &publisher_, node_.native_handle(),
        detail::message_type_support<MessageT>(), topic_name, &options),
      "rcl_publisher_init");
  }

  ~Publisher() noexcept
  {
    (void)close();
  }

  Publisher(const Publisher &) = delete;
  Publisher & operator=(const Publisher &) = delete;
  Publisher(Publisher &&) = delete;
  Publisher & operator=(Publisher &&) = delete;

  void publish(const MessageT & message)
  {
    detail::throw_if_error(
      rcl_publish(&publisher_, &message, nullptr), "rcl_publish");
  }

  size_t subscription_count() const
  {
    size_t count = 0;
    detail::throw_if_error(
      rcl_publisher_get_subscription_count(&publisher_, &count),
      "rcl_publisher_get_subscription_count");
    return count;
  }

  rcl_ret_t close() noexcept
  {
    if (publisher_.impl == nullptr) {
      return RCL_RET_OK;
    }
    const rcl_ret_t ret = rcl_publisher_fini(&publisher_, node_.native_handle());
    publisher_ = rcl_get_zero_initialized_publisher();
    return ret;
  }

  rcl_publisher_t * native_handle() noexcept
  {
    return &publisher_;
  }

private:
  Node & node_;
  rcl_publisher_t publisher_;
};

template<typename MessageT>
class Subscription
{
public:
  using Callback = void (*)(const MessageT & message, void * user_data);

  Subscription(
    Node & node,
    const char * topic_name,
    const QoS & qos,
    Callback callback,
    void * user_data = nullptr)
  : node_(node),
    subscription_(rcl_get_zero_initialized_subscription()),
    message_(),
    callback_(callback),
    user_data_(user_data)
  {
    if (callback_ == nullptr) {
      throw std::invalid_argument("VelaROS subscription callback is null");
    }
    rcl_subscription_options_t options = rcl_subscription_get_default_options();
    options.qos = qos.get_rmw_qos_profile();
    detail::throw_if_error(
      rcl_subscription_init(
        &subscription_, node_.native_handle(),
        detail::message_type_support<MessageT>(), topic_name, &options),
      "rcl_subscription_init");
  }

  ~Subscription() noexcept
  {
    (void)close();
  }

  Subscription(const Subscription &) = delete;
  Subscription & operator=(const Subscription &) = delete;
  Subscription(Subscription &&) = delete;
  Subscription & operator=(Subscription &&) = delete;

  rcl_ret_t close() noexcept
  {
    if (subscription_.impl == nullptr) {
      return RCL_RET_OK;
    }
    const rcl_ret_t ret = rcl_subscription_fini(&subscription_, node_.native_handle());
    subscription_ = rcl_get_zero_initialized_subscription();
    return ret;
  }

  rcl_subscription_t * native_handle() noexcept
  {
    return &subscription_;
  }

  void * message_storage() noexcept
  {
    return &message_;
  }

  static void dispatch(const void * message, void * instance)
  {
    auto * self = static_cast<Subscription *>(instance);
    self->callback_(*static_cast<const MessageT *>(message), self->user_data_);
  }

private:
  Node & node_;
  rcl_subscription_t subscription_;
  MessageT message_;
  Callback callback_;
  void * user_data_;
};

template<typename ServiceT>
class Client
{
public:
  using Request = typename ServiceT::Request;
  using Response = typename ServiceT::Response;
  using Callback = void (*)(
    const rmw_request_id_t & request_id,
    const Response & response,
    void * user_data);

  Client(
    Node & node,
    const char * service_name,
    Callback callback = nullptr,
    void * user_data = nullptr)
  : node_(node),
    client_(rcl_get_zero_initialized_client()),
    response_(),
    callback_(callback),
    user_data_(user_data)
  {
    rcl_client_options_t options = rcl_client_get_default_options();
    detail::throw_if_error(
      rcl_client_init(
        &client_, node_.native_handle(),
        detail::service_type_support<ServiceT>(), service_name, &options),
      "rcl_client_init");
  }

  ~Client() noexcept
  {
    (void)close();
  }

  Client(const Client &) = delete;
  Client & operator=(const Client &) = delete;
  Client(Client &&) = delete;
  Client & operator=(Client &&) = delete;

  int64_t async_send_request(const Request & request)
  {
    int64_t sequence_number = 0;
    detail::throw_if_error(
      rcl_send_request(&client_, &request, &sequence_number),
      "rcl_send_request");
    return sequence_number;
  }

  bool service_is_ready()
  {
    bool available = false;
    detail::throw_if_error(
      rcl_service_server_is_available(
        node_.native_handle(), &client_, &available),
      "rcl_service_server_is_available");
    return available;
  }

  rcl_ret_t close() noexcept
  {
    if (client_.impl == nullptr) {
      return RCL_RET_OK;
    }
    const rcl_ret_t ret = rcl_client_fini(&client_, node_.native_handle());
    client_ = rcl_get_zero_initialized_client();
    return ret;
  }

  rcl_client_t * native_handle() noexcept
  {
    return &client_;
  }

  void * response_storage() noexcept
  {
    return &response_;
  }

  static void dispatch(
    const rmw_request_id_t * request_id,
    const void * response,
    void * instance)
  {
    auto * self = static_cast<Client *>(instance);
    if (self->callback_ != nullptr) {
      self->callback_(*request_id, *static_cast<const Response *>(response), self->user_data_);
    }
  }

private:
  Node & node_;
  rcl_client_t client_;
  Response response_;
  Callback callback_;
  void * user_data_;
};

template<typename ServiceT>
class Service
{
public:
  using Request = typename ServiceT::Request;
  using Response = typename ServiceT::Response;
  using Callback = rcl_ret_t (*)(
    const Request & request,
    Response & response,
    void * user_data);

  Service(
    Node & node,
    const char * service_name,
    Callback callback,
    void * user_data = nullptr)
  : node_(node),
    service_(rcl_get_zero_initialized_service()),
    request_(),
    response_(),
    callback_(callback),
    user_data_(user_data)
  {
    if (callback_ == nullptr) {
      throw std::invalid_argument("VelaROS service callback is null");
    }
    rcl_service_options_t options = rcl_service_get_default_options();
    detail::throw_if_error(
      rcl_service_init(
        &service_, node_.native_handle(),
        detail::service_type_support<ServiceT>(), service_name, &options),
      "rcl_service_init");
  }

  ~Service() noexcept
  {
    (void)close();
  }

  Service(const Service &) = delete;
  Service & operator=(const Service &) = delete;
  Service(Service &&) = delete;
  Service & operator=(Service &&) = delete;

  rcl_ret_t close() noexcept
  {
    if (service_.impl == nullptr) {
      return RCL_RET_OK;
    }
    const rcl_ret_t ret = rcl_service_fini(&service_, node_.native_handle());
    service_ = rcl_get_zero_initialized_service();
    return ret;
  }

  rcl_service_t * native_handle() noexcept
  {
    return &service_;
  }

  void * request_storage() noexcept
  {
    return &request_;
  }

  void * response_storage() noexcept
  {
    return &response_;
  }

  static rcl_ret_t dispatch(
    const void * request,
    void * response,
    void * instance)
  {
    auto * self = static_cast<Service *>(instance);
    return self->callback_(
      *static_cast<const Request *>(request),
      *static_cast<Response *>(response),
      self->user_data_);
  }

private:
  Node & node_;
  rcl_service_t service_;
  Request request_;
  Response response_;
  Callback callback_;
  void * user_data_;
};

class WallTimer
{
public:
  using Callback = void (*)(void * user_data);

  WallTimer(
    Context & context,
    std::chrono::nanoseconds period,
    Callback callback,
    void * user_data = nullptr,
    bool autostart = true);
  ~WallTimer() noexcept;

  WallTimer(const WallTimer &) = delete;
  WallTimer & operator=(const WallTimer &) = delete;
  WallTimer(WallTimer &&) = delete;
  WallTimer & operator=(WallTimer &&) = delete;

  void cancel();
  void reset();
  rcl_ret_t close() noexcept;

  rcl_timer_t * native_handle() noexcept
  {
    return &timer_;
  }

private:
  static void dispatch(rcl_timer_t *, int64_t, uintptr_t callback_data);

  rcl_allocator_t allocator_;
  rcl_clock_t clock_;
  rcl_timer_t timer_;
  Callback callback_;
  void * user_data_;
  bool clock_initialized_;
};

struct ExecutorOptions
{
  size_t subscriptions = 0;
  size_t timers = 0;
  size_t clients = 0;
  size_t services = 0;
#ifdef CONFIG_VELAROS_ACTIONS
  size_t action_clients = 0;
  size_t action_servers = 0;
#endif
};

namespace executors
{

class SingleThreadedExecutor
{
public:
  explicit SingleThreadedExecutor(
    Context & context, const ExecutorOptions & options);
  ~SingleThreadedExecutor() noexcept;

  SingleThreadedExecutor(const SingleThreadedExecutor &) = delete;
  SingleThreadedExecutor & operator=(const SingleThreadedExecutor &) = delete;
  SingleThreadedExecutor(SingleThreadedExecutor &&) = delete;
  SingleThreadedExecutor & operator=(SingleThreadedExecutor &&) = delete;

  template<typename MessageT>
  void add_subscription(Subscription<MessageT> & subscription)
  {
    detail::throw_if_error(
      velaros_executor_add_subscription(
        &executor_, subscription.native_handle(), subscription.message_storage(),
        &Subscription<MessageT>::dispatch, &subscription),
      "velaros_executor_add_subscription");
  }

  template<typename ServiceT>
  void add_client(Client<ServiceT> & client)
  {
    detail::throw_if_error(
      velaros_executor_add_client(
        &executor_, client.native_handle(), client.response_storage(),
        &Client<ServiceT>::dispatch, &client),
      "velaros_executor_add_client");
  }

  template<typename ServiceT>
  void add_service(Service<ServiceT> & service)
  {
    detail::throw_if_error(
      velaros_executor_add_service(
        &executor_, service.native_handle(), service.request_storage(),
        service.response_storage(), &Service<ServiceT>::dispatch, &service),
      "velaros_executor_add_service");
  }

#ifdef CONFIG_VELAROS_ACTIONS
  template<typename ActionClientT>
  void add_action_client(ActionClientT & client)
  {
    detail::throw_if_error(
      velaros_executor_add_action_client(
        &executor_, client.native_handle(), &ActionClientT::dispatch, &client),
      "velaros_executor_add_action_client");
  }

  template<typename ActionServerT>
  void add_action_server(ActionServerT & server)
  {
    detail::throw_if_error(
      velaros_executor_add_action_server(
        &executor_, server.native_handle(), &ActionServerT::dispatch, &server),
      "velaros_executor_add_action_server");
  }
#endif

  void add_timer(WallTimer & timer);
  bool spin_once(std::chrono::nanoseconds timeout);
  rcl_ret_t close() noexcept;

private:
  velaros_executor_t executor_;
};

}  // namespace executors

template<typename MessageT>
std::unique_ptr<Publisher<MessageT>> Node::create_publisher(
  const char * topic_name, const QoS & qos)
{
  return std::unique_ptr<Publisher<MessageT>>(
    new Publisher<MessageT>(*this, topic_name, qos));
}

template<typename MessageT>
std::unique_ptr<Subscription<MessageT>> Node::create_subscription(
  const char * topic_name,
  const QoS & qos,
  typename Subscription<MessageT>::Callback callback,
  void * user_data)
{
  return std::unique_ptr<Subscription<MessageT>>(
    new Subscription<MessageT>(*this, topic_name, qos, callback, user_data));
}

template<typename ServiceT>
std::unique_ptr<Client<ServiceT>> Node::create_client(
  const char * service_name,
  typename Client<ServiceT>::Callback callback,
  void * user_data)
{
  return std::unique_ptr<Client<ServiceT>>(
    new Client<ServiceT>(*this, service_name, callback, user_data));
}

template<typename ServiceT>
std::unique_ptr<Service<ServiceT>> Node::create_service(
  const char * service_name,
  typename Service<ServiceT>::Callback callback,
  void * user_data)
{
  return std::unique_ptr<Service<ServiceT>>(
    new Service<ServiceT>(*this, service_name, callback, user_data));
}

}  // namespace rclcpp

#endif  // VELAROS__RCLCPP__RCLCPP_HPP_
