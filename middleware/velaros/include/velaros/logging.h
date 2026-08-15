/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VELAROS__LOGGING_H
#define VELAROS__LOGGING_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct velaros_logging_stats_s
{
  uint32_t emitted;
  uint32_t truncated;
  uint32_t invalid_severity;
} velaros_logging_stats_t;

/* Install before starting tasks which may log. rcutils handler replacement is
 * process-global and is not itself thread-safe.
 */
void velaros_logging_install_syslog(void);

/* Restore the handler which was active at install time. */
void velaros_logging_restore(void);

bool velaros_logging_is_syslog_installed(void);

void velaros_logging_reset_stats(void);

velaros_logging_stats_t velaros_logging_get_stats(void);

#ifdef __cplusplus
}
#endif

#endif  /* VELAROS__LOGGING_H */
