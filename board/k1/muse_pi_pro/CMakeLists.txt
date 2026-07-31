# SPDX-License-Identifier: Apache-2.0

add_subdirectory(src)

set_property(
  GLOBAL PROPERTY LD_SCRIPT
                  "${NUTTX_BOARD_ABS_DIR}/scripts/ld.script")
