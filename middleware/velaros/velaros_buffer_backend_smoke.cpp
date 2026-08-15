/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

#include "buffer_backend_context.hpp"
#include "buffer_backend_loader.hpp"
#include "fastcdr/Cdr.h"
#include "fastcdr/FastBuffer.h"
#include "rmw/topic_endpoint_info.h"
#include "rosidl_buffer/buffer.hpp"
#include "rosidl_typesupport_fastrtps_cpp/buffer_serialization.hpp"
#include "uORB/uORB.h"
#include "velaros/buffer_backend.hpp"
#include "velaros/buffer_pool.h"
#include "velaros/uorb_topics.h"

namespace
{

constexpr uint32_t kProducerOwner = UINT32_C(0x1001);
constexpr uint32_t kConsumerOwner = UINT32_C(0x2001);
constexpr size_t kPayloadSize = 128;

bool check_pattern(const uint8_t * data, size_t length)
{
  if (data == nullptr || length != kPayloadSize) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (data[index] != static_cast<uint8_t>((index * 17u + 3u) & 0xffu)) {
      return false;
    }
  }
  return true;
}

void fill_pattern(uint8_t * data, size_t length)
{
  for (size_t index = 0; index < length; ++index) {
    data[index] = static_cast<uint8_t>((index * 17u + 3u) & 0xffu);
  }
}

bool test_pool_bounds()
{
  std::array<velaros_buffer_descriptor_t, CONFIG_VELAROS_BUFFER_POOL_SLOTS>
    descriptors{};
  std::array<void *, CONFIG_VELAROS_BUFFER_POOL_SLOTS> payloads{};
  velaros_buffer_descriptor_t extra{};
  velaros_buffer_descriptor_t stale{};
  velaros_buffer_descriptor_t reused{};
  void * extra_payload = nullptr;
  void * reused_payload = nullptr;

  if (velaros_buffer_pool_reset() != VELAROS_BUFFER_OK) {
    return false;
  }
  for (size_t index = 0; index < descriptors.size(); ++index) {
    if (velaros_buffer_acquire(
        32, kProducerOwner + static_cast<uint32_t>(index),
        &descriptors[index], &payloads[index]) != VELAROS_BUFFER_OK)
    {
      return false;
    }
  }
  if (velaros_buffer_acquire(
      32, UINT32_C(0x3001), &extra, &extra_payload) !=
    VELAROS_BUFFER_EXHAUSTED)
  {
    return false;
  }
  for (const auto & descriptor : descriptors) {
    if (velaros_buffer_release(&descriptor) != VELAROS_BUFFER_OK) {
      return false;
    }
  }
  stale = descriptors[0];
  if (velaros_buffer_release(&descriptors[0]) !=
    VELAROS_BUFFER_STALE_DESCRIPTOR)
  {
    return false;
  }
  if (velaros_buffer_pool_reset() != VELAROS_BUFFER_OK ||
    velaros_buffer_acquire(
      32, kProducerOwner, &reused, &reused_payload) != VELAROS_BUFFER_OK)
  {
    return false;
  }
  if (velaros_buffer_release(&stale) != VELAROS_BUFFER_STALE_DESCRIPTOR) {
    (void)velaros_buffer_reclaim_owner(kProducerOwner);
    return false;
  }
  if (velaros_buffer_release(&reused) != VELAROS_BUFFER_OK) {
    return false;
  }
  return velaros_buffer_pool_reset() == VELAROS_BUFFER_OK;
}

bool test_uorb_descriptor_transfer()
{
  velaros_buffer_descriptor_t producer{};
  velaros_buffer_descriptor_t consumer{};
  struct velaros_buffer_transfer_s outgoing{};
  struct velaros_buffer_transfer_s incoming{};
  void * writable = nullptr;
  const void * mapped = nullptr;
  size_t mapped_length = 0;
  int subscription = -1;
  int advertisement = -1;
  bool passed = false;

  subscription = orb_subscribe(ORB_ID(velaros_buffer_transfer));
  if (subscription < 0 ||
    velaros_buffer_acquire(
      kPayloadSize, kProducerOwner, &producer, &writable) != VELAROS_BUFFER_OK)
  {
    goto cleanup;
  }
  fill_pattern(static_cast<uint8_t *>(writable), kPayloadSize);
  if (velaros_buffer_commit(&producer, kPayloadSize) != VELAROS_BUFFER_OK ||
    velaros_buffer_retain(
      &producer, kConsumerOwner, &consumer) != VELAROS_BUFFER_OK)
  {
    goto cleanup;
  }

  outgoing.timestamp = 1;
  outgoing.descriptor = consumer;
  advertisement = orb_advertise(ORB_ID(velaros_buffer_transfer), &outgoing);
  if (advertisement < 0 ||
    velaros_buffer_release(&producer) != VELAROS_BUFFER_OK ||
    orb_copy(
      ORB_ID(velaros_buffer_transfer), subscription, &incoming) < 0 ||
    velaros_buffer_map(
      &incoming.descriptor, &mapped, &mapped_length) != VELAROS_BUFFER_OK)
  {
    goto cleanup;
  }

  passed = mapped == writable &&
    check_pattern(static_cast<const uint8_t *>(mapped), mapped_length) &&
    velaros_buffer_release(&incoming.descriptor) == VELAROS_BUFFER_OK &&
    velaros_buffer_map(
      &incoming.descriptor, &mapped, &mapped_length) ==
    VELAROS_BUFFER_STALE_DESCRIPTOR;

cleanup:
  if (advertisement >= 0) {
    (void)orb_unadvertise(advertisement);
  }
  if (subscription >= 0) {
    (void)orb_unsubscribe(subscription);
  }
  (void)velaros_buffer_reclaim_owner(kProducerOwner);
  (void)velaros_buffer_reclaim_owner(kConsumerOwner);
  return passed;
}

rmw_topic_endpoint_info_t make_endpoint(uint8_t seed)
{
  rmw_topic_endpoint_info_t endpoint =
    rmw_get_zero_initialized_topic_endpoint_info();

  for (size_t index = 0; index < RMW_GID_STORAGE_SIZE; ++index) {
    endpoint.endpoint_gid[index] = static_cast<uint8_t>(seed + index);
  }
  return endpoint;
}

bool test_static_backend_and_fallback()
{
  rmw_fastrtps_cpp::BufferBackendContext context;
  const auto local_endpoint = make_endpoint(0x10);
  const auto remote_endpoint = make_endpoint(0x80);
  bool descriptor_passed = false;
  bool fallback_passed = false;

  rmw_fastrtps_cpp::initialize_buffer_backends(context);
  auto backend_it = context.backend_instances.find(velaros::kBufferBackendType);
  if (backend_it == context.backend_instances.end() || !backend_it->second) {
    rmw_fastrtps_cpp::shutdown_buffer_backends(context);
    return false;
  }
  backend_it->second->on_creating_endpoint(local_endpoint);

  {
    auto impl = velaros::VelaBufferImpl::allocate(kPayloadSize, kProducerOwner);
    if (!impl || impl->writable_data() == nullptr) {
      rmw_fastrtps_cpp::shutdown_buffer_backends(context);
      return false;
    }
    fill_pattern(impl->writable_data(), kPayloadSize);
    if (impl->commit(kPayloadSize) != VELAROS_BUFFER_OK) {
      rmw_fastrtps_cpp::shutdown_buffer_backends(context);
      return false;
    }

    rosidl::Buffer<uint8_t> source(std::move(impl));
    const auto * source_impl =
      dynamic_cast<const velaros::VelaBufferImpl *>(source.get_impl());
    std::array<char, 1024> wire{};
    eprosima::fastcdr::FastBuffer write_buffer(wire.data(), wire.size());
    eprosima::fastcdr::Cdr writer(write_buffer);

    rosidl_typesupport_fastrtps_cpp::serialize_buffer_with_endpoint(
      writer, source, local_endpoint, context.serialization_context);
    const size_t wire_length = writer.get_serialized_data_length();
    eprosima::fastcdr::FastBuffer read_buffer(wire.data(), wire_length);
    eprosima::fastcdr::Cdr reader(read_buffer);
    rosidl::Buffer<uint8_t> output;
    if (!rosidl_typesupport_fastrtps_cpp::deserialize_buffer_with_endpoint(
        reader, output, local_endpoint, context.serialization_context))
    {
      rmw_fastrtps_cpp::shutdown_buffer_backends(context);
      return false;
    }
    const auto * output_impl =
      dynamic_cast<const velaros::VelaBufferImpl *>(output.get_impl());
    descriptor_passed = source_impl != nullptr && output_impl != nullptr &&
      source_impl->data() == output_impl->data() &&
      wire_length < kPayloadSize &&
      check_pattern(output_impl->data(), output_impl->size());

    std::array<char, 1024> fallback_wire{};
    eprosima::fastcdr::FastBuffer fallback_write_buffer(
      fallback_wire.data(), fallback_wire.size());
    eprosima::fastcdr::Cdr fallback_writer(fallback_write_buffer);
    rosidl_typesupport_fastrtps_cpp::serialize_buffer_with_endpoint(
      fallback_writer, source, remote_endpoint, context.serialization_context);
    const size_t fallback_length = fallback_writer.get_serialized_data_length();
    eprosima::fastcdr::FastBuffer fallback_read_buffer(
      fallback_wire.data(), fallback_length);
    eprosima::fastcdr::Cdr fallback_reader(fallback_read_buffer);
    rosidl::Buffer<uint8_t> fallback_output;
    fallback_passed =
      rosidl_typesupport_fastrtps_cpp::deserialize_buffer_with_endpoint(
        fallback_reader, fallback_output, remote_endpoint,
        context.serialization_context) &&
      fallback_output.get_backend_type() == "cpu" &&
      fallback_output.to_vector() == source.to_vector();
  }

  rmw_fastrtps_cpp::shutdown_buffer_backends(context);
  return descriptor_passed && fallback_passed;
}

}  // namespace

extern "C" int main(int, char **)
{
  velaros_buffer_pool_stats_t stats{};

  if (!test_pool_bounds()) {
    std::fprintf(stderr, "VelaROS fixed buffer pool bounds: FAIL\n");
    return 1;
  }
  std::printf("VelaROS fixed buffer pool bounds: PASS\n");

  if (!test_uorb_descriptor_transfer()) {
    std::fprintf(stderr, "VelaROS uORB descriptor zero-copy: FAIL\n");
    return 1;
  }
  std::printf("VelaROS uORB descriptor zero-copy: PASS\n");

  if (!test_static_backend_and_fallback()) {
    std::fprintf(stderr, "VelaROS static rosidl buffer backend: FAIL\n");
    return 1;
  }
  std::printf("VelaROS static rosidl buffer backend: PASS\n");
  std::printf("VelaROS incompatible endpoint CPU/CDR fallback: PASS\n");

  velaros_buffer_pool_get_stats(&stats);
  if (stats.active_slots != 0 || stats.active_leases != 0) {
    std::fprintf(
      stderr, "VelaROS buffer cleanup: FAIL slots=%zu leases=%zu\n",
      stats.active_slots, stats.active_leases);
    return 1;
  }
  std::printf(
    "VelaROS buffer backend smoke: PASS pool=%zu x %zu B, high_water=%zu\n",
    stats.slot_count, stats.slot_size, stats.high_water_slots);
  return 0;
}
