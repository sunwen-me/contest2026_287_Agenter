/* Temporary RAM-only handoff wrapper for the stock U-Boot `go` command. */

  .section .text
  .globl _start
  .type _start, @function

_start:
  /* Leave a visible marker before changing the handoff registers. */
  li    t0, 0xd4017000
  li    t1, 0xd4017014       /* UART0 LSR */
1:
  lw    t2, 0(t1)
  andi  t2, t2, 0x40         /* TEMT */
  beqz  t2, 1b
  li    t2, 87               /* 'W' */
  sw    t2, 0(t0)

  /* NuttX k1_head.S interprets a0 as mhartid. */
  li    a0, 0
  li    a1, 0
  li    t0, 0x11000000
  jr    t0

2:
  wfi
  j     2b

  .size _start, .-_start
