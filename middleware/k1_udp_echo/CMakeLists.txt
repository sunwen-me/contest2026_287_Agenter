# SPDX-License-Identifier: Apache-2.0

if(CONFIG_K1_UDP_ECHO)
  nuttx_add_application(
    NAME
    k1_udpecho
    SRCS
    k1_udp_echo.c
    STACKSIZE
    ${CONFIG_K1_UDP_ECHO_STACKSIZE}
    PRIORITY
    ${CONFIG_K1_UDP_ECHO_PRIORITY})
endif()
