/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cerrno>
#include <cstdio>
#include <inttypes.h>
#include <poll.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>

#include "BnVelaRosRuntime.h"
#include "velaros/config.h"

#define VELAROS_RUNTIME_SERVICE_NAME "openvela.velaros.runtime"

using android::IPCThreadState;
using android::IServiceManager;
using android::ProcessState;
using android::String16;
using android::binder::Status;
using android::defaultServiceManager;
using android::sp;

namespace
{

Status config_status(int ret)
{
  if (ret >= 0) {
    return Status::ok();
  }
  return Status::fromServiceSpecificError(-ret);
}

class VelaRosRuntimeService final : public BnVelaRosRuntime
{
public:
  explicit VelaRosRuntimeService(const velaros_config_t & config)
    : config_(config)
  {
  }

  Status getState(int32_t * value) override
  {
    velaros_runtime_state_t state;
    int ret;

    ++request_count_;
    if (value == nullptr) {
      return Status::fromServiceSpecificError(EINVAL);
    }
    ret = velaros_runtime_get_state(&state);
    if (ret >= 0) {
      *value = state;
    }
    return config_status(ret);
  }

  Status getBridgeEnabled(int32_t * value) override
  {
    ++request_count_;
    if (value == nullptr) {
      return Status::fromServiceSpecificError(EINVAL);
    }
    *value = config_.bridge_enabled ? 1 : 0;
    return Status::ok();
  }

  Status getDomainId(int32_t * value) override
  {
    ++request_count_;
    if (value == nullptr) {
      return Status::fromServiceSpecificError(EINVAL);
    }
    *value = config_.domain_id;
    return Status::ok();
  }

  Status getParticipantId(int32_t * value) override
  {
    ++request_count_;
    if (value == nullptr) {
      return Status::fromServiceSpecificError(EINVAL);
    }
    *value = config_.participant_id;
    return Status::ok();
  }

  Status getHeartbeatPeriodMs(int32_t * value) override
  {
    ++request_count_;
    if (value == nullptr) {
      return Status::fromServiceSpecificError(EINVAL);
    }
    *value = config_.heartbeat_period_ms;
    return Status::ok();
  }

  Status setBridgeEnabled(int32_t enabled) override
  {
    int ret;

    ++request_count_;
    if (enabled != 0 && enabled != 1) {
      return Status::fromServiceSpecificError(ERANGE);
    }
    ret = velaros_config_store_bridge_enabled(enabled != 0);
    if (ret >= 0) {
      config_.bridge_enabled = enabled != 0;
    }
    return config_status(ret);
  }

  Status setDomainId(int32_t domain_id) override
  {
    int ret;

    ++request_count_;
    ret = velaros_config_store_domain_id(domain_id);
    if (ret >= 0) {
      config_.domain_id = domain_id;
    }
    return config_status(ret);
  }

  Status setParticipantId(int32_t participant_id) override
  {
    int ret;

    ++request_count_;
    ret = velaros_config_store_participant_id(participant_id);
    if (ret >= 0) {
      config_.participant_id = participant_id;
    }
    return config_status(ret);
  }

  Status setHeartbeatPeriodMs(int32_t period_ms) override
  {
    int ret;

    ++request_count_;
    ret = velaros_config_store_heartbeat_period_ms(period_ms);
    if (ret >= 0) {
      config_.heartbeat_period_ms = period_ms;
    }
    return config_status(ret);
  }

  Status getRequestCount(int32_t * value) override
  {
    if (value == nullptr) {
      return Status::fromServiceSpecificError(EINVAL);
    }
    *value = ++request_count_;
    return Status::ok();
  }

  Status shutdown() override
  {
    ++request_count_;
    shutdown_requested_ = true;
    return config_status(
      velaros_runtime_set_state(VELAROS_RUNTIME_STOPPING));
  }

  bool shutdown_requested() const
  {
    return shutdown_requested_;
  }

  int32_t request_count() const
  {
    return request_count_;
  }

private:
  velaros_config_t config_{};
  int32_t request_count_ = 0;
  bool shutdown_requested_ = false;
};

}  // namespace

extern "C" int main(int argc, char * argv[])
{
  velaros_config_t config;
  struct pollfd binder_poll;
  sp<IServiceManager> service_manager;
  sp<VelaRosRuntimeService> service;
  int binder_fd = -1;
  int config_ret;
  int result = 1;
  int ret;

  (void)argc;
  (void)argv;
  ret = velaros_runtime_set_state(VELAROS_RUNTIME_STARTING);
  config_ret = velaros_config_load(&config, true);
  if (ret < 0 || config_ret < 0) {
    std::fprintf(
      stderr,
      "VelaROS runtime KVDB initialization failed: state=%d config=%d\n",
      ret, config_ret);
    velaros_runtime_set_state(VELAROS_RUNTIME_FAULT);
    return 1;
  }

  service_manager = defaultServiceManager();
  if (service_manager == nullptr) {
    std::fprintf(stderr, "VelaROS Binder service manager unavailable\n");
    goto cleanup;
  }

  service = new VelaRosRuntimeService(config);
  ret = service_manager->addService(
    String16(VELAROS_RUNTIME_SERVICE_NAME), service);
  if (ret != android::OK) {
    std::fprintf(stderr, "VelaROS Binder registration failed: %d\n", ret);
    goto cleanup;
  }

  ret = IPCThreadState::self()->setupPolling(&binder_fd);
  if (ret != android::OK || binder_fd < 0) {
    std::fprintf(stderr, "VelaROS Binder polling setup failed: %d\n", ret);
    goto cleanup;
  }
  IPCThreadState::self()->flushCommands();

  binder_poll.fd = binder_fd;
  binder_poll.events = POLLIN;
  binder_poll.revents = 0;
  if (velaros_runtime_set_state(VELAROS_RUNTIME_RUNNING) < 0) {
    std::fprintf(stderr, "VelaROS runtime state publication failed\n");
    goto cleanup;
  }

  std::printf(
    "VelaROS runtime service ready: %s domain=%" PRId32
    " participant=%" PRId32 " bridge=%d heartbeat=%" PRId32 "ms\n",
    VELAROS_RUNTIME_SERVICE_NAME, config.domain_id, config.participant_id,
    config.bridge_enabled ? 1 : 0, config.heartbeat_period_ms);

  while (!service->shutdown_requested()) {
    ret = poll(&binder_poll, 1, 1000);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::fprintf(stderr, "VelaROS Binder poll failed: %d\n", errno);
      goto cleanup;
    }
    if (ret > 0 && (binder_poll.revents & POLLIN) != 0) {
      ret = IPCThreadState::self()->handlePolledCommands();
      IPCThreadState::self()->flushCommands();
      if (ret != android::OK) {
        std::fprintf(stderr, "VelaROS Binder dispatch failed: %d\n", ret);
        goto cleanup;
      }
    }
  }

  result = 0;

cleanup:
  if (binder_fd >= 0) {
    IPCThreadState::self()->stopProcess();
  }
  if (result != 0) {
    velaros_runtime_set_state(VELAROS_RUNTIME_FAULT);
  } else {
    velaros_runtime_set_state(VELAROS_RUNTIME_STOPPED);
    std::printf(
      "VelaROS runtime service stopped: requests=%" PRId32 "\n",
      service->request_count());
  }
  return result;
}
