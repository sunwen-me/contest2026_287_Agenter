# SPDX-License-Identifier: Apache-2.0

target_sources(board PRIVATE k1_boot.c)

if(CONFIG_DEV_GPIO AND NOT CONFIG_GPIO_LOWER_HALF)
  target_sources(board PRIVATE k1_gpio.c)
endif()
