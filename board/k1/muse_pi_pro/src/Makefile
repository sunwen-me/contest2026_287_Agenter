############################################################################
# board/k1/muse_pi_pro/src/Makefile
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(TOPDIR)/Make.defs

CSRCS = k1_boot.c

ifeq ($(CONFIG_DEV_GPIO),y)
ifneq ($(CONFIG_GPIO_LOWER_HALF),y)
  CSRCS += k1_gpio.c
endif
endif

include $(TOPDIR)/boards/Board.mk
