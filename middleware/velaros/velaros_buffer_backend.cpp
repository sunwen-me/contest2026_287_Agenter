/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/buffer_backend.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fastcdr/Cdr.h"
#include "rosidl_buffer/cpu_buffer_impl.hpp"
#include "rosidl_typesupport_fastrtps_cpp/identifier.hpp"
#include "rosidl_typesupport_fastrtps_cpp/message_type_support.h"
#include "rosidl_typesupport_fastrtps_cpp/message_type_support_decl.hpp"

namespace velaros
{
namespace
{

uint32_t endpoint_hash(const rmw_topic_endpoint_info_t & endpoint)
{
  uint32_t hash = UINT32_C(2166136261);

  for (size_t index = 0; index < RMW_GID_STORAGE_SIZE; ++index) {
    hash ^= endpoint.endpoint_gid[index];
    hash *= UINT32_C(16777619);
  }
  return hash == 0 ? 1 : hash;
}

bool descriptor_serialize(
  const void * untyped,
  eprosima::fastcdr::Cdr & cdr)
{
  const auto * descriptor =
    static_cast<const velaros_buffer_descriptor_t *>(untyped);

  if (descriptor == nullptr) {
    return false;
  }
  cdr << descriptor->pool_cookie;
  cdr << descriptor->slot;
  cdr << descriptor->generation;
  cdr << descriptor->lease;
  cdr << descriptor->owner;
  cdr << descriptor->length;
  cdr << descriptor->capacity;
  cdr << descriptor->flags;
  return true;
}

bool descriptor_deserialize(
  eprosima::fastcdr::Cdr & cdr,
  void * untyped)
{
  auto * descriptor = static_cast<velaros_buffer_descriptor_t *>(untyped);

  if (descriptor == nullptr) {
    return false;
  }
  cdr >> descriptor->pool_cookie;
  cdr >> descriptor->slot;
  cdr >> descriptor->generation;
  cdr >> descriptor->lease;
  cdr >> descriptor->owner;
  cdr >> descriptor->length;
  cdr >> descriptor->capacity;
  cdr >> descriptor->flags;
  return true;
}

uint32_t descriptor_serialized_size(const void *)
{
  return 36;
}

size_t descriptor_max_serialized_size(char & bounds_info)
{
  bounds_info = ROSIDL_TYPESUPPORT_FASTRTPS_PLAIN_TYPE;
  return 36;
}

message_type_support_callbacks_t g_descriptor_callbacks = {
  "velaros::internal",
  "VelaBufferDescriptor",
  descriptor_serialize,
  descriptor_deserialize,
  descriptor_serialized_size,
  descriptor_max_serialized_size,
  nullptr,
  false,
  nullptr,
  nullptr
};

rosidl_message_type_support_t g_descriptor_type_support = {
  rosidl_typesupport_fastrtps_cpp::typesupport_identifier,
  &g_descriptor_callbacks,
  get_message_typesupport_handle_function,
  nullptr,
  nullptr,
  nullptr
};

void delete_buffer_impl(void * pointer)
{
  delete static_cast<VelaBufferImpl *>(pointer);
}

class VelaBufferBackend final : public rosidl::BufferBackend
{
public:
  std::string get_backend_type() const override
  {
    return kBufferBackendType;
  }

  std::string get_backend_metadata() const override
  {
    return std::to_string(velaros_buffer_pool_cookie());
  }

  const rosidl_message_type_support_t * get_descriptor_type_support() const override
  {
    return &g_descriptor_type_support;
  }

  std::shared_ptr<void> create_empty_descriptor() const override
  {
    return std::make_shared<velaros_buffer_descriptor_t>();
  }

  std::shared_ptr<void> create_descriptor_with_endpoint(
    const void * untyped_impl,
    const rmw_topic_endpoint_info_t & endpoint) const override
  {
    const auto * impl = static_cast<const rosidl::BufferImplBase<uint8_t> *>(
      untyped_impl);
    const auto * vela_impl = dynamic_cast<const VelaBufferImpl *>(impl);
    const uint32_t owner = endpoint_hash(endpoint);
    velaros_buffer_descriptor_t retained{};

    if (vela_impl == nullptr || !vela_impl->valid() ||
      !endpoint_is_compatible(owner))
    {
      return nullptr;
    }
    if (velaros_buffer_retain(
        &vela_impl->descriptor(), owner, &retained) != VELAROS_BUFFER_OK)
    {
      return nullptr;
    }
    return std::make_shared<velaros_buffer_descriptor_t>(retained);
  }

  std::unique_ptr<void, void (*)(void *)> from_descriptor_with_endpoint(
    const void * untyped_descriptor,
    const rmw_topic_endpoint_info_t & endpoint) const override
  {
    const auto * descriptor =
      static_cast<const velaros_buffer_descriptor_t *>(untyped_descriptor);

    if (descriptor == nullptr ||
      !endpoint_is_compatible(endpoint_hash(endpoint)))
    {
      return {nullptr, delete_buffer_impl};
    }

    auto impl = VelaBufferImpl::adopt(*descriptor);
    return {impl.release(), delete_buffer_impl};
  }

  void on_creating_endpoint(
    const rmw_topic_endpoint_info_t & endpoint) const override
  {
    const uint32_t hash = endpoint_hash(endpoint);
    std::lock_guard<std::mutex> guard(endpoint_lock_);
    compatible_endpoints_.insert(hash);
  }

  std::pair<bool, std::vector<std::set<uint32_t>>> on_discovering_endpoint(
    const rmw_topic_endpoint_info_t & endpoint,
    const std::vector<rmw_topic_endpoint_info_t> &,
    const std::unordered_map<std::string, std::string> & supported) override
  {
    auto backend = supported.find(kBufferBackendType);
    const bool compatible =
      backend != supported.end() && backend->second == get_backend_metadata();
    std::vector<std::set<uint32_t>> groups;

    if (compatible) {
      const uint32_t hash = endpoint_hash(endpoint);
      std::lock_guard<std::mutex> guard(endpoint_lock_);
      compatible_endpoints_.insert(hash);
      groups.push_back(compatible_endpoints_);
    }
    return {compatible, std::move(groups)};
  }

private:
  bool endpoint_is_compatible(uint32_t hash) const
  {
    std::lock_guard<std::mutex> guard(endpoint_lock_);
    return compatible_endpoints_.find(hash) != compatible_endpoints_.end();
  }

  mutable std::mutex endpoint_lock_;
  mutable std::set<uint32_t> compatible_endpoints_;
};

}  // namespace

VelaBufferImpl::~VelaBufferImpl()
{
  if (owns_lease_) {
    (void)velaros_buffer_release(&descriptor_);
  }
}

std::string VelaBufferImpl::get_backend_type() const
{
  return kBufferBackendType;
}

size_t VelaBufferImpl::size() const
{
  return committed_ ? length_ : descriptor_.capacity;
}

std::unique_ptr<rosidl::BufferImplBase<uint8_t>> VelaBufferImpl::to_cpu() const
{
  auto cpu = std::make_unique<rosidl::CpuBufferImpl<uint8_t>>();

  if (valid() && committed_) {
    cpu->get_storage().assign(payload_, payload_ + length_);
  }
  return cpu;
}

std::unique_ptr<rosidl::BufferImplBase<uint8_t>> VelaBufferImpl::clone() const
{
  if (!valid() || !committed_) {
    return nullptr;
  }
  auto cloned = allocate(length_, descriptor_.owner);
  if (!cloned) {
    return nullptr;
  }
  std::memcpy(cloned->writable_data(), payload_, length_);
  if (cloned->commit(length_) != VELAROS_BUFFER_OK) {
    return nullptr;
  }
  return cloned;
}

uint8_t * VelaBufferImpl::writable_data()
{
  return committed_ ? nullptr : writable_payload_;
}

const uint8_t * VelaBufferImpl::data() const
{
  return committed_ ? payload_ : nullptr;
}

velaros_buffer_ret_t VelaBufferImpl::commit(size_t length)
{
  velaros_buffer_ret_t ret;
  const void * payload = nullptr;
  size_t mapped_length = 0;

  if (!owns_lease_ || committed_) {
    return VELAROS_BUFFER_INVALID_ARGUMENT;
  }
  ret = velaros_buffer_commit(&descriptor_, length);
  if (ret != VELAROS_BUFFER_OK) {
    return ret;
  }
  ret = velaros_buffer_map(&descriptor_, &payload, &mapped_length);
  if (ret != VELAROS_BUFFER_OK) {
    return ret;
  }
  payload_ = static_cast<const uint8_t *>(payload);
  length_ = mapped_length;
  writable_payload_ = nullptr;
  committed_ = true;
  return VELAROS_BUFFER_OK;
}

const velaros_buffer_descriptor_t & VelaBufferImpl::descriptor() const
{
  return descriptor_;
}

bool VelaBufferImpl::valid() const
{
  return owns_lease_ && descriptor_.pool_cookie != 0;
}

std::unique_ptr<VelaBufferImpl> VelaBufferImpl::allocate(
  size_t capacity,
  uint32_t owner)
{
  auto impl = std::unique_ptr<VelaBufferImpl>(new VelaBufferImpl());
  void * writable = nullptr;

  if (velaros_buffer_acquire(
      capacity, owner, &impl->descriptor_, &writable) != VELAROS_BUFFER_OK)
  {
    return nullptr;
  }
  impl->writable_payload_ = static_cast<uint8_t *>(writable);
  impl->payload_ = static_cast<const uint8_t *>(writable);
  impl->owns_lease_ = true;
  return impl;
}

std::unique_ptr<VelaBufferImpl> VelaBufferImpl::adopt(
  const velaros_buffer_descriptor_t & descriptor)
{
  auto impl = std::unique_ptr<VelaBufferImpl>(new VelaBufferImpl());
  const void * payload = nullptr;
  size_t length = 0;

  if (velaros_buffer_map(&descriptor, &payload, &length) != VELAROS_BUFFER_OK) {
    return nullptr;
  }
  impl->descriptor_ = descriptor;
  impl->payload_ = static_cast<const uint8_t *>(payload);
  impl->length_ = length;
  impl->owns_lease_ = true;
  impl->committed_ = true;
  return impl;
}

std::shared_ptr<rosidl::BufferBackend> create_buffer_backend()
{
  return std::make_shared<VelaBufferBackend>();
}

}  // namespace velaros
