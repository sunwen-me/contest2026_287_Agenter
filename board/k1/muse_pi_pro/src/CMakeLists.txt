# SPDX-License-Identifier: Apache-2.0

target_sources(board PRIVATE k1_boot.c)

if(CONFIG_BOARDCTL_RESET)
  target_sources(board PRIVATE k1_reset.c)
endif()

if(CONFIG_DEV_GPIO AND NOT CONFIG_GPIO_LOWER_HALF)
  target_sources(board PRIVATE k1_gpio.c)
endif()

if(CONFIG_K1_RTL8852BS2_WIFI OR CONFIG_K1_RTL8852BS2_BT)
  target_sources(board PRIVATE k1_wireless.c)
endif()
