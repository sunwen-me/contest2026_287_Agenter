/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__EXECUTOR_H
#define VELAROS__EXECUTOR_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rcl/allocator.h"
#include "rcl/client.h"
#include "rcl/context.h"
#include "rcl/service.h"
#include "rcl/subscription.h"
#include "rcl/timer.h"
#include "rcl/types.h"
#include "rcl/wait.h"

#ifdef CONFIG_VELAROS_ACTIONS
/* Keep the core executor header usable by non-Action applications without
 * pulling the generated action_msgs headers into every consumer. */
typedef struct rcl_action_client_s rcl_action_client_t;
typedef struct rcl_action_server_s rcl_action_server_t;
#endif

typedef void (*velaros_subscription_callback_t)(
  const void * message, void * user_data);

typedef void (*velaros_client_callback_t)(
  const rmw_request_id_t * request_id,
  const void * response,
  void * user_data);

typedef rcl_ret_t (*velaros_service_callback_t)(
  const void * request,
  void * response,
  void * user_data);

#ifdef CONFIG_VELAROS_ACTIONS
typedef rcl_ret_t (*velaros_action_client_callback_t)(
  rcl_action_client_t * client,
  bool feedback_ready,
  bool status_ready,
  bool goal_response_ready,
  bool cancel_response_ready,
  bool result_response_ready,
  void * user_data);

typedef rcl_ret_t (*velaros_action_server_callback_t)(
  rcl_action_server_t * server,
  bool goal_request_ready,
  bool cancel_request_ready,
  bool result_request_ready,
  bool goal_expired,
  void * user_data);
#endif

typedef struct velaros_executor_subscription_s
{
  rcl_subscription_t * subscription;
  void * message;
  velaros_subscription_callback_t callback;
  void * user_data;
} velaros_executor_subscription_t;

typedef struct velaros_executor_client_s
{
  rcl_client_t * client;
  void * response;
  velaros_client_callback_t callback;
  void * user_data;
} velaros_executor_client_t;

typedef struct velaros_executor_service_s
{
  rcl_service_t * service;
  void * request;
  void * response;
  velaros_service_callback_t callback;
  void * user_data;
} velaros_executor_service_t;

#ifdef CONFIG_VELAROS_ACTIONS
typedef struct velaros_executor_action_client_s
{
  rcl_action_client_t * client;
  velaros_action_client_callback_t callback;
  void * user_data;
} velaros_executor_action_client_t;

typedef struct velaros_executor_action_server_s
{
  rcl_action_server_t * server;
  velaros_action_server_callback_t callback;
  void * user_data;
} velaros_executor_action_server_t;
#endif

typedef struct velaros_executor_s
{
  rcl_wait_set_t wait_set;
  rcl_allocator_t allocator;
  rcl_context_t * context;
  velaros_executor_subscription_t * subscriptions;
  rcl_timer_t ** timers;
  velaros_executor_client_t * clients;
  velaros_executor_service_t * services;
  size_t subscription_capacity;
  size_t subscription_count;
  size_t timer_capacity;
  size_t timer_count;
  size_t client_capacity;
  size_t client_count;
  size_t service_capacity;
  size_t service_count;
#ifdef CONFIG_VELAROS_ACTIONS
  velaros_executor_action_client_t * action_clients;
  velaros_executor_action_server_t * action_servers;
  size_t action_client_capacity;
  size_t action_client_count;
  size_t action_server_capacity;
  size_t action_server_count;
#endif
  bool initialized;
} velaros_executor_t;

velaros_executor_t velaros_executor_get_zero_initialized(void);

rcl_ret_t velaros_executor_init(
  velaros_executor_t * executor,
  rcl_context_t * context,
  size_t subscription_capacity,
  size_t timer_capacity,
  size_t client_capacity,
  size_t service_capacity,
  rcl_allocator_t allocator);

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
  rcl_allocator_t allocator);
#endif

rcl_ret_t velaros_executor_add_subscription(
  velaros_executor_t * executor,
  rcl_subscription_t * subscription,
  void * message,
  velaros_subscription_callback_t callback,
  void * user_data);

rcl_ret_t velaros_executor_add_timer(
  velaros_executor_t * executor, rcl_timer_t * timer);

rcl_ret_t velaros_executor_add_client(
  velaros_executor_t * executor,
  rcl_client_t * client,
  void * response,
  velaros_client_callback_t callback,
  void * user_data);

rcl_ret_t velaros_executor_add_service(
  velaros_executor_t * executor,
  rcl_service_t * service,
  void * request,
  void * response,
  velaros_service_callback_t callback,
  void * user_data);

#ifdef CONFIG_VELAROS_ACTIONS
rcl_ret_t velaros_executor_add_action_client(
  velaros_executor_t * executor,
  rcl_action_client_t * client,
  velaros_action_client_callback_t callback,
  void * user_data);

rcl_ret_t velaros_executor_add_action_server(
  velaros_executor_t * executor,
  rcl_action_server_t * server,
  velaros_action_server_callback_t callback,
  void * user_data);
#endif

/* Wait once, then dispatch every ready registered entity in registration order. */
rcl_ret_t velaros_executor_spin_once(
  velaros_executor_t * executor, int64_t timeout_ns);

rcl_ret_t velaros_executor_fini(velaros_executor_t * executor);

#ifdef __cplusplus
}
#endif

#endif  /* VELAROS__EXECUTOR_H */
