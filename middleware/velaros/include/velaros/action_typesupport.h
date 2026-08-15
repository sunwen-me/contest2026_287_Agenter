/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__ACTION_TYPESUPPORT_H
#define VELAROS__ACTION_TYPESUPPORT_H

#include "rosidl_runtime_c/action_type_support_struct.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Static Fast RTPS C type support for example_interfaces/Fibonacci. */
#ifdef CONFIG_VELAROS_ACTION_FIBONACCI
const rosidl_action_type_support_t * velaros_fibonacci_action_typesupport(void);
#endif

/* Static Fast RTPS C type support for velaros_interfaces/MoveRelative. */
const rosidl_action_type_support_t * velaros_move_relative_action_typesupport(void);

#ifdef __cplusplus
}
#endif

#endif  /* VELAROS__ACTION_TYPESUPPORT_H */
