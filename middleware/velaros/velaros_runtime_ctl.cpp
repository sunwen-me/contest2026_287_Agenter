/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <unistd.h>

#include <binder/IServiceManager.h>
#include <utils/String16.h>

#include "IVelaRosRuntime.h"
#include "velaros/config.h"

#define VELAROS_RUNTIME_SERVICE_NAME "openvela.velaros.runtime"
#define VELAROS_SERVICE_LOOKUP_RETRIES 100
#define VELAROS_SERVICE_LOOKUP_DELAY_US 20000

using android::IBinder;
using android::IServiceManager;
using android::String16;
using android::defaultServiceManager;
using android::interface_cast;
using android::sp;
using android::binder::Status;

namespace
{

sp<IVelaRosRuntime> find_service()
{
  sp<IServiceManager> service_manager = defaultServiceManager();

  if (service_manager == nullptr) {
    return nullptr;
  }
  for (int retry = 0; retry < VELAROS_SERVICE_LOOKUP_RETRIES; ++retry) {
    sp<IBinder> binder = service_manager->checkService(
      String16(VELAROS_RUNTIME_SERVICE_NAME));
    if (binder != nullptr) {
      return interface_cast<IVelaRosRuntime>(binder);
    }
    usleep(VELAROS_SERVICE_LOOKUP_DELAY_US);
  }
  return nullptr;
}

bool status_ok(const Status & status, const char * operation)
{
  if (status.isOk()) {
    return true;
  }
  std::fprintf(
    stderr, "%s failed: exception=%d service=%d\n", operation,
    status.exceptionCode(), status.serviceSpecificErrorCode());
  return false;
}

bool read_status(
  const sp<IVelaRosRuntime> & service, velaros_config_t * config,
  int32_t * runtime_state)
{
  int32_t bridge_enabled;

  return status_ok(service->getState(runtime_state), "getState") &&
         status_ok(
           service->getBridgeEnabled(&bridge_enabled), "getBridgeEnabled") &&
         status_ok(service->getDomainId(&config->domain_id), "getDomainId") &&
         status_ok(
           service->getParticipantId(&config->participant_id),
           "getParticipantId") &&
         status_ok(
           service->getHeartbeatPeriodMs(&config->heartbeat_period_ms),
           "getHeartbeatPeriodMs") &&
         ((config->bridge_enabled = bridge_enabled != 0), true);
}

int run_smoke(const sp<IVelaRosRuntime> & service)
{
  velaros_config_t original;
  velaros_config_t persisted;
  int32_t runtime_state;
  int32_t request_count;
  const int32_t test_bridge = 0;
  const int32_t test_period = 250;
  bool passed = false;

  if (!read_status(service, &original, &runtime_state) ||
      runtime_state != VELAROS_RUNTIME_RUNNING) {
    std::fprintf(stderr, "VelaROS runtime did not enter RUNNING state\n");
    goto shutdown;
  }

  if (!status_ok(
      service->setBridgeEnabled(test_bridge), "setBridgeEnabled") ||
      !status_ok(
      service->setHeartbeatPeriodMs(test_period),
      "setHeartbeatPeriodMs") ||
      velaros_config_load(&persisted, false) < 0 ||
      persisted.bridge_enabled != (test_bridge != 0) ||
      persisted.heartbeat_period_ms != test_period) {
    std::fprintf(stderr, "VelaROS KVDB cross-task verification failed\n");
    goto restore;
  }

  std::printf("VelaROS Binder service discovery: PASS\n");
  std::printf("VelaROS KVDB cross-task config: PASS\n");
  std::printf("VelaROS Binder single-task poll loop: PASS\n");
  passed = true;

restore:
  if (!status_ok(
      service->setBridgeEnabled(original.bridge_enabled ? 1 : 0),
      "restoreBridgeEnabled") ||
      !status_ok(
      service->setHeartbeatPeriodMs(original.heartbeat_period_ms),
      "restoreHeartbeatPeriodMs")) {
    passed = false;
  }
  if (!status_ok(
      service->getRequestCount(&request_count), "getRequestCount") ||
      request_count < 9) {
    std::fprintf(stderr, "VelaROS Binder request accounting failed\n");
    passed = false;
  }

shutdown:
  if (!status_ok(service->shutdown(), "shutdown")) {
    passed = false;
  }
  if (passed) {
    std::printf("VelaROS service lifecycle: PASS\n");
    return 0;
  }
  return 1;
}

bool parse_int32(const char * text, int32_t * value)
{
  char * end = nullptr;
  long parsed;

  if (text == nullptr || value == nullptr) {
    return false;
  }
  parsed = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || parsed < INT32_MIN ||
      parsed > INT32_MAX) {
    return false;
  }
  *value = static_cast<int32_t>(parsed);
  return true;
}

void print_usage()
{
  std::fprintf(
    stderr,
    "usage: velarosctl status | smoke | stop | "
    "set bridge|domain|participant|heartbeat <value>\n");
}

}  // namespace

extern "C" int main(int argc, char * argv[])
{
  sp<IVelaRosRuntime> service;
  velaros_config_t config;
  int32_t state;
  int32_t value;
  Status status;

  if (argc < 2) {
    print_usage();
    return 1;
  }
  service = find_service();
  if (service == nullptr) {
    std::fprintf(stderr, "VelaROS runtime service not found\n");
    return 1;
  }

  if (std::strcmp(argv[1], "smoke") == 0) {
    return run_smoke(service);
  }
  if (std::strcmp(argv[1], "stop") == 0) {
    return status_ok(service->shutdown(), "shutdown") ? 0 : 1;
  }
  if (std::strcmp(argv[1], "status") == 0) {
    if (!read_status(service, &config, &state)) {
      return 1;
    }
    std::printf(
      "state=%" PRId32 " bridge=%d domain=%" PRId32
      " participant=%" PRId32 " heartbeat=%" PRId32 "ms\n",
      state, config.bridge_enabled ? 1 : 0, config.domain_id,
      config.participant_id, config.heartbeat_period_ms);
    return 0;
  }
  if (argc == 4 && std::strcmp(argv[1], "set") == 0 &&
      parse_int32(argv[3], &value)) {
    if (std::strcmp(argv[2], "bridge") == 0) {
      status = service->setBridgeEnabled(value);
    } else if (std::strcmp(argv[2], "domain") == 0) {
      status = service->setDomainId(value);
    } else if (std::strcmp(argv[2], "participant") == 0) {
      status = service->setParticipantId(value);
    } else if (std::strcmp(argv[2], "heartbeat") == 0) {
      status = service->setHeartbeatPeriodMs(value);
    } else {
      print_usage();
      return 1;
    }
    return status_ok(status, "set") ? 0 : 1;
  }

  print_usage();
  return 1;
}
