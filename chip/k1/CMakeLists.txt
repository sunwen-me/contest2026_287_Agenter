# ##############################################################################
# chip/k1/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

set(
  SRCS
  k1_head.S
  k1_start.c
  k1_irq.c
  k1_irq_dispatch.c
  k1_timerisr.c
  k1_console.c)

if(CONFIG_K1_PLIC)
  list(APPEND SRCS k1_plic.c)
endif()

if(CONFIG_K1_EMAC OR CONFIG_K1_FB OR CONFIG_K1_USBDEV OR CONFIG_USBHOST_XHCI_K1)
  list(APPEND SRCS k1_cache.c)
endif()

if(CONFIG_K1_EMAC)
  list(APPEND SRCS k1_emac.c)
endif()

if(CONFIG_K1_I2C)
  list(APPEND SRCS k1_i2c.c)
endif()

if(CONFIG_K1_SPI)
  list(APPEND SRCS k1_spi.c)
endif()

if(CONFIG_K1_SDIO)
  list(APPEND SRCS k1_sdio.c)
endif()

if(CONFIG_K1_RTL8852BS2_BT)
  list(APPEND SRCS k1_bt_uart.c)
endif()

if(CONFIG_K1_PWM)
  list(APPEND SRCS k1_pwm.c)
endif()

if(CONFIG_K1_WATCHDOG)
  list(APPEND SRCS k1_wdt.c)
endif()

if(CONFIG_K1_FB)
  list(APPEND SRCS k1_fb.c)
endif()

if(CONFIG_K1_USBDEV)
  list(APPEND SRCS k1_usbdev.c)
endif()

if(CONFIG_K1_USB_PROBE OR CONFIG_K1_USB_HOST_GLUE)
  list(APPEND SRCS k1_usb.c)
endif()

target_sources(arch PRIVATE ${SRCS})
