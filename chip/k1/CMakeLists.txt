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

if(CONFIG_K1_EMAC)
  list(APPEND SRCS k1_cache.c k1_emac.c)
endif()

target_sources(arch PRIVATE ${SRCS})
