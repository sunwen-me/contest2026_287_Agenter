/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/executor.h"

#include <string.h>

#include "rcl/error_handling.h"
#include "rcutils/allocator.h"

#ifdef CONFIG_VELAROS_ACTIONS
#include "rcl_action/action_client.h"
#include "rcl_action/action_server.h"
#include "rcl_action/wait.h"
#endif

velaros_executor_t velaros_executor_get_zero_initialized(void)
{
  static const velaros_executor_t zero_executor;
  return zero_executor;
}

static bool velaros_executor_is_valid(const velaros_executor_t * executor)
{
  return executor != NULL && executor->initialized &&
         executor->context != NULL && executor->wait_set.impl != NULL;
}

static rcl_ret_t velaros_executor_init_impl(
  velaros_executor_t * executor,
  rcl_context_t * context,
  size_t subscription_capacity,
  size_t timer_capacity,
  size_t client_capacity,
  size_t service_capacity,
#ifdef CONFIG_VELAROS_ACTIONS
  size_t action_client_capacity,
  size_t action_server_capacity,
#endif
  rcl_allocator_t allocator)
{
  rcl_ret_t ret;

  if (executor == NULL || context == NULL ||
      !rcutils_allocator_is_valid(&allocator) ||
      (subscription_capacity == 0 && timer_capacity == 0 &&
       client_capacity == 0 && service_capacity == 0
#ifdef CONFIG_VELAROS_ACTIONS
       && action_client_capacity == 0 && action_server_capacity == 0
#endif
      )) {
    RCL_SET_ERROR_MSG("invalid VelaROS executor init argument");
    return RCL_RET_INVALID_ARGUMENT;
  }
  if (executor->initialized || executor->wait_set.impl != NULL) {
    RCL_SET_ERROR_MSG("VelaROS executor is already initialized");
    return RCL_RET_ALREADY_INIT;
  }
  if (!rcl_context_is_valid(context)) {
    RCL_SET_ERROR_MSG("VelaROS executor context is not valid");
    return RCL_RET_NOT_INIT;
  }

  executor->allocator = allocator;
  executor->context = context;
  executor->subscription_capacity = subscription_capacity;
  executor->timer_capacity = timer_capacity;
  executor->client_capacity = client_capacity;
  executor->service_capacity = service_capacity;
#ifdef CONFIG_VELAROS_ACTIONS
  executor->action_client_capacity = action_client_capacity;
  executor->action_server_capacity = action_server_capacity;
#endif
  executor->wait_set = rcl_get_zero_initialized_wait_set();

  ret = rcl_wait_set_init(
    &executor->wait_set,
    subscription_capacity
#ifdef CONFIG_VELAROS_ACTIONS
      + (2u * action_client_capacity)
#endif
    , 0,
    timer_capacity
#ifdef CONFIG_VELAROS_ACTIONS
      + action_server_capacity
#endif
    ,
    client_capacity
#ifdef CONFIG_VELAROS_ACTIONS
      + (3u * action_client_capacity)
#endif
    ,
    service_capacity
#ifdef CONFIG_VELAROS_ACTIONS
      + (3u * action_server_capacity)
#endif
    , 0, context, allocator);
  if (ret != RCL_RET_OK) {
    *executor = velaros_executor_get_zero_initialized();
    return ret;
  }

  if (subscription_capacity > 0) {
    executor->subscriptions = allocator.zero_allocate(
      subscription_capacity, sizeof(*executor->subscriptions), allocator.state);
    if (executor->subscriptions == NULL) {
      goto fail;
    }
  }
  if (timer_capacity > 0) {
    executor->timers = allocator.zero_allocate(
      timer_capacity, sizeof(*executor->timers), allocator.state);
    if (executor->timers == NULL) {
      goto fail;
    }
  }
  if (client_capacity > 0) {
    executor->clients = allocator.zero_allocate(
      client_capacity, sizeof(*executor->clients), allocator.state);
    if (executor->clients == NULL) {
      goto fail;
    }
  }
  if (service_capacity > 0) {
    executor->services = allocator.zero_allocate(
      service_capacity, sizeof(*executor->services), allocator.state);
    if (executor->services == NULL) {
      goto fail;
    }
  }
#ifdef CONFIG_VELAROS_ACTIONS
  if (action_client_capacity > 0) {
    executor->action_clients = allocator.zero_allocate(
      action_client_capacity, sizeof(*executor->action_clients), allocator.state);
    if (executor->action_clients == NULL) {
      goto fail;
    }
  }
  if (action_server_capacity > 0) {
    executor->action_servers = allocator.zero_allocate(
      action_server_capacity, sizeof(*executor->action_servers), allocator.state);
    if (executor->action_servers == NULL) {
      goto fail;
    }
  }
#endif

  executor->initialized = true;
  return RCL_RET_OK;

fail:
  if (executor->subscriptions != NULL) {
    allocator.deallocate(executor->subscriptions, allocator.state);
  }
  if (executor->timers != NULL) {
    allocator.deallocate(executor->timers, allocator.state);
  }
  if (executor->clients != NULL) {
    allocator.deallocate(executor->clients, allocator.state);
  }
  if (executor->services != NULL) {
    allocator.deallocate(executor->services, allocator.state);
  }
#ifdef CONFIG_VELAROS_ACTIONS
  if (executor->action_clients != NULL) {
    allocator.deallocate(executor->action_clients, allocator.state);
  }
  if (executor->action_servers != NULL) {
    allocator.deallocate(executor->action_servers, allocator.state);
  }
#endif
  if (rcl_wait_set_fini(&executor->wait_set) != RCL_RET_OK) {
    rcl_reset_error();
  }
  *executor = velaros_executor_get_zero_initialized();
  RCL_SET_ERROR_MSG("allocating VelaROS executor entity storage failed");
  return RCL_RET_BAD_ALLOC;
}

rcl_ret_t velaros_executor_init(
  velaros_executor_t * executor,
  rcl_context_t * context,
  size_t subscription_capacity,
  size_t timer_capacity,
  size_t client_capacity,
  size_t service_capacity,
  rcl_allocator_t allocator)
{
  return velaros_executor_init_impl(
    executor, context, subscription_capacity, timer_capacity,
    client_capacity, service_capacity,
#ifdef CONFIG_VELAROS_ACTIONS
    0, 0,
#endif
    allocator);
}

#ifdef CONFIG_VELAROS_ACTIONS
rcl_ret_t velaros_executor_init_with_actions(
  velaros_executor_t * executor,
  rcl_context_t * context,
  size_t subscription_capacity,
  size_t timer_capacity,
  size_t client_capacity,
  size_t service_capacity,
  size_t action_client_capacity,
  size_t action_server_capacity,
  rcl_allocator_t allocator)
{
  return velaros_executor_init_impl(
    executor, context, subscription_capacity, timer_capacity,
    client_capacity, service_capacity, action_client_capacity,
    action_server_capacity, allocator);
}
#endif

rcl_ret_t velaros_executor_add_subscription(
  velaros_executor_t * executor,
  rcl_subscription_t * subscription,
  void * message,
  velaros_subscription_callback_t callback,
  void * user_data)
{
  velaros_executor_subscription_t * entry;

  if (!velaros_executor_is_valid(executor) || subscription == NULL ||
      message == NULL || callback == NULL) {
    RCL_SET_ERROR_MSG("invalid VelaROS executor subscription argument");
    return RCL_RET_INVALID_ARGUMENT;
  }
  if (!rcl_subscription_is_valid(subscription)) {
    RCL_SET_ERROR_MSG("VelaROS executor subscription is not valid");
    return RCL_RET_SUBSCRIPTION_INVALID;
  }
  if (executor->subscription_count >= executor->subscription_capacity) {
    RCL_SET_ERROR_MSG("VelaROS executor subscription capacity exhausted");
    return RCL_RET_WAIT_SET_FULL;
  }
  for (size_t index = 0; index < executor->subscription_count; ++index) {
    if (executor->subscriptions[index].subscription == subscription) {
      RCL_SET_ERROR_MSG("subscription is already registered with VelaROS executor");
      return RCL_RET_ALREADY_INIT;
    }
  }

  entry = &executor->subscriptions[executor->subscription_count++];
  entry->subscription = subscription;
  entry->message = message;
  entry->callback = callback;
  entry->user_data = user_data;
  return RCL_RET_OK;
}

rcl_ret_t velaros_executor_add_timer(
  velaros_executor_t * executor, rcl_timer_t * timer)
{
  if (!velaros_executor_is_valid(executor) || timer == NULL || timer->impl == NULL) {
    RCL_SET_ERROR_MSG("invalid VelaROS executor timer argument");
    return RCL_RET_INVALID_ARGUMENT;
  }
  if (executor->timer_count >= executor->timer_capacity) {
    RCL_SET_ERROR_MSG("VelaROS executor timer capacity exhausted");
    return RCL_RET_WAIT_SET_FULL;
  }
  for (size_t index = 0; index < executor->timer_count; ++index) {
    if (executor->timers[index] == timer) {
      RCL_SET_ERROR_MSG("timer is already registered with VelaROS executor");
      return RCL_RET_ALREADY_INIT;
    }
  }

  executor->timers[executor->timer_count++] = timer;
  return RCL_RET_OK;
}

rcl_ret_t velaros_executor_add_client(
  velaros_executor_t * executor,
  rcl_client_t * client,
  void * response,
  velaros_client_callback_t callback,
  void * user_data)
{
  velaros_executor_client_t * entry;

  if (!velaros_executor_is_valid(executor) || client == NULL ||
      response == NULL || callback == NULL) {
    RCL_SET_ERROR_MSG("invalid VelaROS executor client argument");
    return RCL_RET_INVALID_ARGUMENT;
  }
  if (!rcl_client_is_valid(client)) {
    RCL_SET_ERROR_MSG("VelaROS executor client is not valid");
    return RCL_RET_CLIENT_INVALID;
  }
  if (executor->client_count >= executor->client_capacity) {
    RCL_SET_ERROR_MSG("VelaROS executor client capacity exhausted");
    return RCL_RET_WAIT_SET_FULL;
  }
  for (size_t index = 0; index < executor->client_count; ++index) {
    if (executor->clients[index].client == client) {
      RCL_SET_ERROR_MSG("client is already registered with VelaROS executor");
      return RCL_RET_ALREADY_INIT;
    }
  }

  entry = &executor->clients[executor->client_count++];
  entry->client = client;
  entry->response = response;
  entry->callback = callback;
  entry->user_data = user_data;
  return RCL_RET_OK;
}

rcl_ret_t velaros_executor_add_service(
  velaros_executor_t * executor,
  rcl_service_t * service,
  void * request,
  void * response,
  velaros_service_callback_t callback,
  void * user_data)
{
  velaros_executor_service_t * entry;

  if (!velaros_executor_is_valid(executor) || service == NULL ||
      request == NULL || response == NULL || callback == NULL) {
    RCL_SET_ERROR_MSG("invalid VelaROS executor service argument");
    return RCL_RET_INVALID_ARGUMENT;
  }
  if (!rcl_service_is_valid(service)) {
    RCL_SET_ERROR_MSG("VelaROS executor service is not valid");
    return RCL_RET_SERVICE_INVALID;
  }
  if (executor->service_count >= executor->service_capacity) {
    RCL_SET_ERROR_MSG("VelaROS executor service capacity exhausted");
    return RCL_RET_WAIT_SET_FULL;
  }
  for (size_t index = 0; index < executor->service_count; ++index) {
    if (executor->services[index].service == service) {
      RCL_SET_ERROR_MSG("service is already registered with VelaROS executor");
      return RCL_RET_ALREADY_INIT;
    }
  }

  entry = &executor->services[executor->service_count++];
  entry->service = service;
  entry->request = request;
  entry->response = response;
  entry->callback = callback;
  entry->user_data = user_data;
  return RCL_RET_OK;
}

#ifdef CONFIG_VELAROS_ACTIONS
rcl_ret_t velaros_executor_add_action_client(
  velaros_executor_t * executor,
  rcl_action_client_t * client,
  velaros_action_client_callback_t callback,
  void * user_data)
{
  if (!velaros_executor_is_valid(executor) || client == NULL || callback == NULL) {
    RCL_SET_ERROR_MSG("invalid VelaROS executor Action client argument");
    return RCL_RET_INVALID_ARGUMENT;
  }
  if (!rcl_action_client_is_valid(client)) {
    return RCL_RET_ACTION_CLIENT_INVALID;
  }
  if (executor->action_client_count >= executor->action_client_capacity) {
    RCL_SET_ERROR_MSG("VelaROS executor Action client capacity exhausted");
    return RCL_RET_WAIT_SET_FULL;
  }
  for (size_t index = 0; index < executor->action_client_count; ++index) {
    if (executor->action_clients[index].client == client) {
      return RCL_RET_ALREADY_INIT;
    }
  }
  velaros_executor_action_client_t * entry =
    &executor->action_clients[executor->action_client_count++];
  entry->client = client;
  entry->callback = callback;
  entry->user_data = user_data;
  return RCL_RET_OK;
}

rcl_ret_t velaros_executor_add_action_server(
  velaros_executor_t * executor,
  rcl_action_server_t * server,
  velaros_action_server_callback_t callback,
  void * user_data)
{
  if (!velaros_executor_is_valid(executor) || server == NULL || callback == NULL) {
    RCL_SET_ERROR_MSG("invalid VelaROS executor Action server argument");
    return RCL_RET_INVALID_ARGUMENT;
  }
  if (!rcl_action_server_is_valid(server)) {
    return RCL_RET_ACTION_SERVER_INVALID;
  }
  if (executor->action_server_count >= executor->action_server_capacity) {
    RCL_SET_ERROR_MSG("VelaROS executor Action server capacity exhausted");
    return RCL_RET_WAIT_SET_FULL;
  }
  for (size_t index = 0; index < executor->action_server_count; ++index) {
    if (executor->action_servers[index].server == server) {
      return RCL_RET_ALREADY_INIT;
    }
  }
  velaros_executor_action_server_t * entry =
    &executor->action_servers[executor->action_server_count++];
  entry->server = server;
  entry->callback = callback;
  entry->user_data = user_data;
  return RCL_RET_OK;
}
#endif

rcl_ret_t velaros_executor_spin_once(
  velaros_executor_t * executor, int64_t timeout_ns)
{
  rcl_ret_t ret;

  if (!velaros_executor_is_valid(executor)) {
    RCL_SET_ERROR_MSG("VelaROS executor is not initialized");
    return RCL_RET_NOT_INIT;
  }
  if (executor->subscription_count + executor->timer_count +
      executor->client_count + executor->service_count
#ifdef CONFIG_VELAROS_ACTIONS
      + executor->action_client_count + executor->action_server_count
#endif
      == 0) {
    RCL_SET_ERROR_MSG("VelaROS executor has no registered entities");
    return RCL_RET_WAIT_SET_EMPTY;
  }

  ret = rcl_wait_set_clear(&executor->wait_set);
  if (ret != RCL_RET_OK) {
    return ret;
  }
  for (size_t index = 0; index < executor->subscription_count; ++index) {
    ret = rcl_wait_set_add_subscription(
      &executor->wait_set, executor->subscriptions[index].subscription, NULL);
    if (ret != RCL_RET_OK) {
      return ret;
    }
  }
  for (size_t index = 0; index < executor->timer_count; ++index) {
    ret = rcl_wait_set_add_timer(
      &executor->wait_set, executor->timers[index], NULL);
    if (ret != RCL_RET_OK) {
      return ret;
    }
  }
  for (size_t index = 0; index < executor->client_count; ++index) {
    ret = rcl_wait_set_add_client(
      &executor->wait_set, executor->clients[index].client, NULL);
    if (ret != RCL_RET_OK) {
      return ret;
    }
  }
  for (size_t index = 0; index < executor->service_count; ++index) {
    ret = rcl_wait_set_add_service(
      &executor->wait_set, executor->services[index].service, NULL);
    if (ret != RCL_RET_OK) {
      return ret;
    }
  }
#ifdef CONFIG_VELAROS_ACTIONS
  for (size_t index = 0; index < executor->action_client_count; ++index) {
    ret = rcl_action_wait_set_add_action_client(
      &executor->wait_set, executor->action_clients[index].client, NULL, NULL);
    if (ret != RCL_RET_OK) {
      return ret;
    }
  }
  for (size_t index = 0; index < executor->action_server_count; ++index) {
    ret = rcl_action_wait_set_add_action_server(
      &executor->wait_set, executor->action_servers[index].server, NULL);
    if (ret != RCL_RET_OK) {
      return ret;
    }
  }
#endif

  ret = rcl_wait(&executor->wait_set, timeout_ns);
  if (ret == RCL_RET_TIMEOUT) {
    /* A timeout is a normal spin result.  rcl_wait records it in the global
     * error slot, so clear that diagnostic before Action progress calls can
     * report a real failure. */
    rcl_reset_error();
#ifdef CONFIG_VELAROS_ACTIONS
    /* Action execution is deliberately driven by the caller-owned executor
     * instead of per-goal worker threads.  Give each registered server one
     * bounded progress opportunity even when no wire entity became ready. */
    for (size_t index = 0; index < executor->action_server_count; ++index) {
      velaros_executor_action_server_t * entry = &executor->action_servers[index];
      rcl_ret_t progress_ret = entry->callback(
        entry->server, false, false, false, false, entry->user_data);
      if (progress_ret != RCL_RET_OK) {
        return progress_ret;
      }
    }
#endif
    return ret;
  }
  if (ret != RCL_RET_OK) {
    return ret;
  }

  for (size_t index = 0; index < executor->subscription_count; ++index) {
    velaros_executor_subscription_t * entry = &executor->subscriptions[index];

    if (executor->wait_set.subscriptions[index] == NULL) {
      continue;
    }
    ret = rcl_take(entry->subscription, entry->message, NULL, NULL);
    if (ret == RCL_RET_SUBSCRIPTION_TAKE_FAILED) {
      continue;
    }
    if (ret != RCL_RET_OK) {
      return ret;
    }
    entry->callback(entry->message, entry->user_data);
  }

  for (size_t index = 0; index < executor->timer_count; ++index) {
    if (executor->wait_set.timers[index] == NULL) {
      continue;
    }
    ret = rcl_timer_call(executor->timers[index]);
    if (ret != RCL_RET_OK) {
      return ret;
    }
  }

  for (size_t index = 0; index < executor->client_count; ++index) {
    velaros_executor_client_t * entry = &executor->clients[index];
    rmw_request_id_t request_id;

    if (executor->wait_set.clients[index] == NULL) {
      continue;
    }
    memset(&request_id, 0, sizeof(request_id));
    ret = rcl_take_response(entry->client, &request_id, entry->response);
    if (ret == RCL_RET_CLIENT_TAKE_FAILED) {
      continue;
    }
    if (ret != RCL_RET_OK) {
      return ret;
    }
    entry->callback(&request_id, entry->response, entry->user_data);
  }

  for (size_t index = 0; index < executor->service_count; ++index) {
    velaros_executor_service_t * entry = &executor->services[index];
    rmw_request_id_t request_id;

    if (executor->wait_set.services[index] == NULL) {
      continue;
    }
    memset(&request_id, 0, sizeof(request_id));
    ret = rcl_take_request(entry->service, &request_id, entry->request);
    if (ret == RCL_RET_SERVICE_TAKE_FAILED) {
      continue;
    }
    if (ret != RCL_RET_OK) {
      return ret;
    }
    ret = entry->callback(entry->request, entry->response, entry->user_data);
    if (ret != RCL_RET_OK) {
      return ret;
    }
    ret = rcl_send_response(entry->service, &request_id, entry->response);
    if (ret != RCL_RET_OK) {
      return ret;
    }
  }
#ifdef CONFIG_VELAROS_ACTIONS
  for (size_t index = 0; index < executor->action_client_count; ++index) {
    velaros_executor_action_client_t * entry = &executor->action_clients[index];
    bool feedback_ready;
    bool status_ready;
    bool goal_response_ready;
    bool cancel_response_ready;
    bool result_response_ready;
    /* Ready/take probes use non-fatal return codes for empty queues.  Do not
     * let an earlier probe's diagnostic leak into the next Action operation. */
    rcl_reset_error();
    ret = rcl_action_client_wait_set_get_entities_ready(
      &executor->wait_set, entry->client, &feedback_ready, &status_ready,
      &goal_response_ready, &cancel_response_ready, &result_response_ready);
    if (ret != RCL_RET_OK) {
      return ret;
    }
    if (feedback_ready || status_ready || goal_response_ready ||
        cancel_response_ready || result_response_ready) {
      ret = entry->callback(
        entry->client, feedback_ready, status_ready, goal_response_ready,
        cancel_response_ready, result_response_ready, entry->user_data);
      if (ret != RCL_RET_OK) {
        return ret;
      }
      rcl_reset_error();
    }
  }
  for (size_t index = 0; index < executor->action_server_count; ++index) {
    velaros_executor_action_server_t * entry = &executor->action_servers[index];
    bool goal_request_ready;
    bool cancel_request_ready;
    bool result_request_ready;
    bool goal_expired;
    rcl_reset_error();
    ret = rcl_action_server_wait_set_get_entities_ready(
      &executor->wait_set, entry->server, &goal_request_ready,
      &cancel_request_ready, &result_request_ready, &goal_expired);
    if (ret != RCL_RET_OK) {
      return ret;
    }
    /* Always call the server once per successful wait.  Besides dispatching
     * ready wire entities this is the deterministic, caller-thread execution
     * hook used by the static VelaROS Action API. */
    ret = entry->callback(
      entry->server, goal_request_ready, cancel_request_ready,
      result_request_ready, goal_expired, entry->user_data);
    if (ret != RCL_RET_OK) {
      return ret;
    }
    rcl_reset_error();
  }
#endif
  return RCL_RET_OK;
}

rcl_ret_t velaros_executor_fini(velaros_executor_t * executor)
{
  rcl_ret_t ret = RCL_RET_OK;
  rcl_allocator_t allocator;

  if (executor == NULL) {
    RCL_SET_ERROR_MSG("VelaROS executor is null");
    return RCL_RET_INVALID_ARGUMENT;
  }
  if (!executor->initialized) {
    return RCL_RET_OK;
  }

  allocator = executor->allocator;
  if (executor->wait_set.impl != NULL) {
    ret = rcl_wait_set_fini(&executor->wait_set);
  }
  if (executor->subscriptions != NULL) {
    allocator.deallocate(executor->subscriptions, allocator.state);
  }
  if (executor->timers != NULL) {
    allocator.deallocate(executor->timers, allocator.state);
  }
  if (executor->clients != NULL) {
    allocator.deallocate(executor->clients, allocator.state);
  }
  if (executor->services != NULL) {
    allocator.deallocate(executor->services, allocator.state);
  }
#ifdef CONFIG_VELAROS_ACTIONS
  if (executor->action_clients != NULL) {
    allocator.deallocate(executor->action_clients, allocator.state);
  }
  if (executor->action_servers != NULL) {
    allocator.deallocate(executor->action_servers, allocator.state);
  }
#endif
  *executor = velaros_executor_get_zero_initialized();
  return ret;
}
