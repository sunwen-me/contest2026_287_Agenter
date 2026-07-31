/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal command-line boundary for the first openvela rcl lifecycle port.
 * YAML parameters and remapping are deliberately deferred.
 */

#include <string.h>

#include "rcl/arguments.h"
#include "rcl/error_handling.h"
#include "rcl/remap.h"

#include "arguments_impl.h"

rcl_arguments_t rcl_get_zero_initialized_arguments(void)
{
  rcl_arguments_t arguments = {0};
  return arguments;
}

static rcl_ret_t allocate_empty_arguments(
  rcl_allocator_t allocator,
  rcl_arguments_t * arguments)
{
  if (arguments == NULL || arguments->impl != NULL ||
      !rcutils_allocator_is_valid(&allocator)) {
    RCL_SET_ERROR_MSG("invalid minimal rcl arguments");
    return RCL_RET_INVALID_ARGUMENT;
  }

  arguments->impl = allocator.zero_allocate(
    1, sizeof(rcl_arguments_impl_t), allocator.state);
  if (arguments->impl == NULL) {
    RCL_SET_ERROR_MSG("failed to allocate minimal rcl arguments");
    return RCL_RET_BAD_ALLOC;
  }

  arguments->impl->allocator = allocator;
  return RCL_RET_OK;
}

rcl_ret_t rcl_parse_arguments(
  int argc,
  const char * const * argv,
  rcl_allocator_t allocator,
  rcl_arguments_t * args_output)
{
  (void)argv;
  if (argc != 0) {
    RCL_SET_ERROR_MSG(
      "openvela minimal rcl currently accepts only an empty argument list");
    return RCL_RET_UNSUPPORTED;
  }
  return allocate_empty_arguments(allocator, args_output);
}

rcl_ret_t rcl_arguments_copy(
  const rcl_arguments_t * args,
  rcl_arguments_t * args_out)
{
  if (args == NULL || args->impl == NULL || args_out == NULL) {
    RCL_SET_ERROR_MSG("invalid minimal rcl arguments copy");
    return RCL_RET_INVALID_ARGUMENT;
  }

  rcl_ret_t ret = allocate_empty_arguments(args->impl->allocator, args_out);
  if (ret != RCL_RET_OK) {
    return ret;
  }

  if (args->impl->enclave != NULL) {
    const size_t length = strlen(args->impl->enclave) + 1;
    args_out->impl->enclave = args->impl->allocator.allocate(
      length, args->impl->allocator.state);
    if (args_out->impl->enclave == NULL) {
      args_out->impl->allocator.deallocate(
        args_out->impl, args_out->impl->allocator.state);
      args_out->impl = NULL;
      RCL_SET_ERROR_MSG("failed to copy minimal rcl enclave argument");
      return RCL_RET_BAD_ALLOC;
    }
    memcpy(args_out->impl->enclave, args->impl->enclave, length);
  }

  return RCL_RET_OK;
}

rcl_ret_t rcl_arguments_fini(rcl_arguments_t * args)
{
  if (args == NULL || args->impl == NULL) {
    RCL_SET_ERROR_MSG("minimal rcl arguments are not initialized");
    return RCL_RET_INVALID_ARGUMENT;
  }

  rcl_allocator_t allocator = args->impl->allocator;
  allocator.deallocate(args->impl->enclave, allocator.state);
  allocator.deallocate(args->impl, allocator.state);
  args->impl = NULL;
  return RCL_RET_OK;
}

rcl_ret_t rcl_remap_node_name(
  const rcl_arguments_t * local_arguments,
  const rcl_arguments_t * global_arguments,
  const char * node_name,
  rcl_allocator_t allocator,
  char ** output_name)
{
  (void)local_arguments;
  (void)global_arguments;
  (void)node_name;
  (void)allocator;
  if (output_name == NULL) {
    return RCL_RET_INVALID_ARGUMENT;
  }
  *output_name = NULL;
  return RCL_RET_OK;
}

rcl_ret_t rcl_remap_node_namespace(
  const rcl_arguments_t * local_arguments,
  const rcl_arguments_t * global_arguments,
  const char * node_name,
  rcl_allocator_t allocator,
  char ** output_namespace)
{
  (void)local_arguments;
  (void)global_arguments;
  (void)node_name;
  (void)allocator;
  if (output_namespace == NULL) {
    return RCL_RET_INVALID_ARGUMENT;
  }
  *output_namespace = NULL;
  return RCL_RET_OK;
}
