/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__BUFFER_BACKEND_HPP
#define VELAROS__BUFFER_BACKEND_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "rosidl_buffer/buffer_impl_base.hpp"
#include "rosidl_buffer_backend/buffer_backend.hpp"
#include "velaros/buffer_pool.h"

namespace velaros
{

inline constexpr char kBufferBackendType[] = "velaros";
inline constexpr char kStaticBufferBackendName[] = "velaros/static";

class VelaBufferImpl final : public rosidl::BufferImplBase<uint8_t>
{
public:
  ~VelaBufferImpl() override;

  VelaBufferImpl(const VelaBufferImpl &) = delete;
  VelaBufferImpl & operator=(const VelaBufferImpl &) = delete;
  VelaBufferImpl(VelaBufferImpl &&) = delete;
  VelaBufferImpl & operator=(VelaBufferImpl &&) = delete;

  std::string get_backend_type() const override;
  size_t size() const override;
  std::unique_ptr<rosidl::BufferImplBase<uint8_t>> to_cpu() const override;
  std::unique_ptr<rosidl::BufferImplBase<uint8_t>> clone() const override;

  uint8_t * writable_data();
  const uint8_t * data() const;
  velaros_buffer_ret_t commit(size_t length);
  const velaros_buffer_descriptor_t & descriptor() const;
  bool valid() const;

  static std::unique_ptr<VelaBufferImpl> allocate(size_t capacity, uint32_t owner);
  static std::unique_ptr<VelaBufferImpl> adopt(
    const velaros_buffer_descriptor_t & descriptor);

private:
  VelaBufferImpl() = default;

  velaros_buffer_descriptor_t descriptor_{};
  uint8_t * writable_payload_{nullptr};
  const uint8_t * payload_{nullptr};
  size_t length_{0};
  bool owns_lease_{false};
  bool committed_{false};
};

std::shared_ptr<rosidl::BufferBackend> create_buffer_backend();

}  // namespace velaros

#endif  // VELAROS__BUFFER_BACKEND_HPP
