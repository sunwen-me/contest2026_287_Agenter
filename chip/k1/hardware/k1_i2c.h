/****************************************************************************
 * vendor/spacemit/chips/k1/hardware/k1_i2c.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_K1_HARDWARE_K1_I2C_H
#define __CHIP_K1_HARDWARE_K1_I2C_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MUSE Pi Pro uses I2C2 for its on-board 24C02 EEPROM. */

#define K1_I2C2_BASE                    0xd4012000ul
#define K1_APBC_I2C2_CLK_RST            0xd4015038ul

/* I2C register offsets. */

#define K1_I2C_ICR_OFFSET                0x000ul
#define K1_I2C_ISR_OFFSET                0x004ul
#define K1_I2C_ISAR_OFFSET               0x008ul
#define K1_I2C_IDBR_OFFSET               0x00cul
#define K1_I2C_ILCR_OFFSET               0x010ul
#define K1_I2C_IWCR_OFFSET               0x014ul
#define K1_I2C_IRCR_OFFSET               0x018ul
#define K1_I2C_IBMR_OFFSET               0x01cul

/* ICR bits. */

#define K1_I2C_ICR_START                 (1ul << 0)
#define K1_I2C_ICR_STOP                  (1ul << 1)
#define K1_I2C_ICR_ACKNAK                (1ul << 2)
#define K1_I2C_ICR_TB                    (1ul << 3)
#define K1_I2C_ICR_FAST                  (1ul << 8)
#define K1_I2C_ICR_UR                    (1ul << 10)
#define K1_I2C_ICR_RSTREQ                (1ul << 11)
#define K1_I2C_ICR_SCLE                  (1ul << 13)
#define K1_I2C_ICR_IUE                   (1ul << 14)
#define K1_I2C_ICR_ALDIE                 (1ul << 18)
#define K1_I2C_ICR_DTEIE                 (1ul << 19)
#define K1_I2C_ICR_DRFIE                 (1ul << 20)
#define K1_I2C_ICR_GCD                   (1ul << 21)
#define K1_I2C_ICR_BEIE                  (1ul << 22)
#define K1_I2C_ICR_MSDIE                 (1ul << 25)
#define K1_I2C_ICR_MSDE                  (1ul << 26)
#define K1_I2C_ICR_TXDONEIE              (1ul << 27)
#define K1_I2C_ICR_TXEIE                 (1ul << 28)
#define K1_I2C_ICR_RXHFIE                (1ul << 29)
#define K1_I2C_ICR_RXFIE                 (1ul << 30)
#define K1_I2C_ICR_RXOVIE                (1ul << 31)

/* ISR bits.  Interrupt status bits are write-one-to-clear. */

#define K1_I2C_ISR_ACKNAK                (1ul << 14)
#define K1_I2C_ISR_UB                    (1ul << 15)
#define K1_I2C_ISR_IBB                   (1ul << 16)
#define K1_I2C_ISR_EBB                   (1ul << 17)
#define K1_I2C_ISR_ALD                   (1ul << 18)
#define K1_I2C_ISR_ITE                   (1ul << 19)
#define K1_I2C_ISR_IRF                   (1ul << 20)
#define K1_I2C_ISR_GCAD                  (1ul << 21)
#define K1_I2C_ISR_BED                   (1ul << 22)
#define K1_I2C_ISR_SAD                   (1ul << 23)
#define K1_I2C_ISR_SSD                   (1ul << 24)
#define K1_I2C_ISR_MSD                   (1ul << 26)
#define K1_I2C_ISR_TXDONE                (1ul << 27)
#define K1_I2C_ISR_TXE                   (1ul << 28)
#define K1_I2C_ISR_RXHF                  (1ul << 29)
#define K1_I2C_ISR_RXF                   (1ul << 30)
#define K1_I2C_ISR_RXOV                  (1ul << 31)

#define K1_I2C_ISR_INT_STATUS_MASK       \
  (K1_I2C_ISR_ALD | K1_I2C_ISR_ITE | K1_I2C_ISR_IRF | \
   K1_I2C_ISR_GCAD | K1_I2C_ISR_BED | K1_I2C_ISR_SAD | \
   K1_I2C_ISR_SSD | K1_I2C_ISR_MSD | K1_I2C_ISR_TXDONE | \
   K1_I2C_ISR_TXE | K1_I2C_ISR_RXHF | K1_I2C_ISR_RXF | \
   K1_I2C_ISR_RXOV)

/* I2C reset-cycle and bus-monitor fields. */

#define K1_I2C_IRCR_RESET_CLOCKS_MASK    0xful
#define K1_I2C_IRCR_SDA_GLITCH_DISABLE   (1ul << 7)
#define K1_I2C_IBMR_SDA                   (1ul << 0)
#define K1_I2C_IBMR_SCL                   (1ul << 1)

#endif /* __CHIP_K1_HARDWARE_K1_I2C_H */
