/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "velaros/logging.h"

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

#include "rcutils/logging.h"

#define VELAROS_SYSLOG_MESSAGE_SIZE 384

static rcutils_logging_output_handler_t g_previous_handler;
static bool g_installed;
static uint32_t g_emitted;
static uint32_t g_truncated;
static uint32_t g_invalid_severity;

static int velaros_syslog_priority(int severity)
{
  switch (severity) {
    case RCUTILS_LOG_SEVERITY_DEBUG:
      return LOG_DEBUG;
    case RCUTILS_LOG_SEVERITY_INFO:
      return LOG_INFO;
    case RCUTILS_LOG_SEVERITY_WARN:
      return LOG_WARNING;
    case RCUTILS_LOG_SEVERITY_ERROR:
      return LOG_ERR;
    case RCUTILS_LOG_SEVERITY_FATAL:
      return LOG_CRIT;
    default:
      __atomic_fetch_add(&g_invalid_severity, 1, __ATOMIC_RELAXED);
      return LOG_NOTICE;
  }
}

static void velaros_syslog_output_handler(
  const rcutils_log_location_t * location,
  int severity,
  const char * name,
  rcutils_time_point_value_t timestamp,
  const char * format,
  va_list * args)
{
  char message[VELAROS_SYSLOG_MESSAGE_SIZE];
  va_list copy;
  int length;

  (void)location;
  (void)timestamp;
  if (format == NULL || args == NULL) {
    __atomic_fetch_add(&g_invalid_severity, 1, __ATOMIC_RELAXED);
    return;
  }

  va_copy(copy, *args);
  length = vsnprintf(message, sizeof(message), format, copy);
  va_end(copy);
  if (length < 0) {
    __atomic_fetch_add(&g_truncated, 1, __ATOMIC_RELAXED);
    return;
  }
  if ((size_t)length >= sizeof(message)) {
    __atomic_fetch_add(&g_truncated, 1, __ATOMIC_RELAXED);
  }

  syslog(
    velaros_syslog_priority(severity), "[ros2][%s] %s\n",
    name != NULL && name[0] != '\0' ? name : "root", message);
  __atomic_fetch_add(&g_emitted, 1, __ATOMIC_RELAXED);
}

void velaros_logging_install_syslog(void)
{
  if (velaros_logging_is_syslog_installed()) {
    return;
  }

  g_previous_handler = rcutils_logging_get_output_handler();
  velaros_logging_reset_stats();
  rcutils_logging_set_output_handler(velaros_syslog_output_handler);
  g_installed = true;
}

void velaros_logging_restore(void)
{
  if (!g_installed) {
    return;
  }
  if (rcutils_logging_get_output_handler() == velaros_syslog_output_handler) {
    rcutils_logging_set_output_handler(g_previous_handler);
  }
  g_previous_handler = NULL;
  g_installed = false;
}

bool velaros_logging_is_syslog_installed(void)
{
  return g_installed &&
         rcutils_logging_get_output_handler() == velaros_syslog_output_handler;
}

void velaros_logging_reset_stats(void)
{
  __atomic_store_n(&g_emitted, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_truncated, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_invalid_severity, 0, __ATOMIC_RELAXED);
}

velaros_logging_stats_t velaros_logging_get_stats(void)
{
  velaros_logging_stats_t stats;

  stats.emitted = __atomic_load_n(&g_emitted, __ATOMIC_RELAXED);
  stats.truncated = __atomic_load_n(&g_truncated, __ATOMIC_RELAXED);
  stats.invalid_severity =
    __atomic_load_n(&g_invalid_severity, __ATOMIC_RELAXED);
  return stats;
}
