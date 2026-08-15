# SPDX-License-Identifier: Apache-2.0

if(CONFIG_K1_WATCHDOG_SMOKE)
  nuttx_add_application(
    NAME
    k1_wdt_smoke
    SRCS
    k1_wdt_smoke.c
    STACKSIZE
    4096)
endif()
