/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/action_typesupport.h"

#ifdef CONFIG_VELAROS_ACTION_FIBONACCI
#include "example_interfaces/action/detail/fibonacci__functions.h"
#endif
#include "velaros_interfaces/action/detail/move_relative__functions.h"
#include "rosidl_typesupport_interface/macros.h"

/* CancelGoal and GoalStatusArray are shared by every ROS 2 Action type.  Keep
 * their declarations before both optional development and product adapters so
 * neither type relies on the other type's include/declaration side effects.
 */

extern const rosidl_service_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c,
  action_msgs,
  srv,
  CancelGoal)(void);

extern const rosidl_message_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c,
  action_msgs,
  msg,
  GoalStatusArray)(void);

#ifdef CONFIG_VELAROS_ACTION_FIBONACCI
extern const rosidl_service_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c,
  example_interfaces,
  action,
  Fibonacci_SendGoal)(void);

extern const rosidl_service_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c,
  example_interfaces,
  action,
  Fibonacci_GetResult)(void);

extern const rosidl_message_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c,
  example_interfaces,
  action,
  Fibonacci_FeedbackMessage)(void);

const rosidl_action_type_support_t * velaros_fibonacci_action_typesupport(void)
{
  static rosidl_action_type_support_t type_support;

  if (type_support.goal_service_type_support == NULL) {
    type_support.goal_service_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        example_interfaces,
        action,
        Fibonacci_SendGoal)();
    type_support.result_service_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        example_interfaces,
        action,
        Fibonacci_GetResult)();
    type_support.cancel_service_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        action_msgs,
        srv,
        CancelGoal)();
    type_support.feedback_message_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        example_interfaces,
        action,
        Fibonacci_FeedbackMessage)();
    type_support.status_message_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        action_msgs,
        msg,
        GoalStatusArray)();
    type_support.get_type_hash_func =
      example_interfaces__action__Fibonacci__get_type_hash;
    type_support.get_type_description_func = NULL;
    type_support.get_type_description_sources_func = NULL;
  }
  return &type_support;
}
#endif

extern const rosidl_service_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c,
  velaros_interfaces,
  action,
  MoveRelative_SendGoal)(void);

extern const rosidl_service_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c,
  velaros_interfaces,
  action,
  MoveRelative_GetResult)(void);

extern const rosidl_message_type_support_t *
ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
  rosidl_typesupport_fastrtps_c,
  velaros_interfaces,
  action,
  MoveRelative_FeedbackMessage)(void);

const rosidl_action_type_support_t * velaros_move_relative_action_typesupport(void)
{
  static rosidl_action_type_support_t type_support;

  if (type_support.goal_service_type_support == NULL) {
    type_support.goal_service_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        velaros_interfaces,
        action,
        MoveRelative_SendGoal)();
    type_support.result_service_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        velaros_interfaces,
        action,
        MoveRelative_GetResult)();
    type_support.cancel_service_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__SERVICE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        action_msgs,
        srv,
        CancelGoal)();
    type_support.feedback_message_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        velaros_interfaces,
        action,
        MoveRelative_FeedbackMessage)();
    type_support.status_message_type_support =
      ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME(
        rosidl_typesupport_fastrtps_c,
        action_msgs,
        msg,
        GoalStatusArray)();
    type_support.get_type_hash_func =
      velaros_interfaces__action__MoveRelative__get_type_hash;
    type_support.get_type_description_func = NULL;
    type_support.get_type_description_sources_func = NULL;
  }
  return &type_support;
}
