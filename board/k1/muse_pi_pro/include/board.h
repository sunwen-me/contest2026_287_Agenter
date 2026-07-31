/****************************************************************************
 * board/k1/muse_pi_pro/include/board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARD_K1_MUSE_PI_PRO_INCLUDE_BOARD_H
#define __BOARD_K1_MUSE_PI_PRO_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* The K1 timebase frequency is reported by the board device tree. */

#define BOARD_K1_TIMEBASE_FREQUENCY 24000000ul

#ifndef __ASSEMBLY__

#ifdef __cplusplus
extern "C"
{
#endif

void k1_muse_pi_pro_board_initialize(void);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARD_K1_MUSE_PI_PRO_INCLUDE_BOARD_H */
