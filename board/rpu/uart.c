#include "uart.h"
#include <stdint.h>

/*
 * Zynq UltraScale+ PS UART register map.
 * Base addresses:
 *   UART0 : 0xFF000000  ← used (MIO 10/11, L3_SEL=6 → ttyUSB1 on ZUBoard 1CG)
 *   UART1 : 0xFF010000
 *
 * UART0 is the console: psu_init configures UART0_REF_CTRL (not UART1), and
 * the A53 BSP outbyte() targets 0xFF000000.
 *
 * Reference: UG1085 Zynq UltraScale+ TRM, chapter PS UART.
 */
#define UART_BASE       0xFF000000UL

#define REG32(addr)     (*((volatile uint32_t *)(addr)))

/* Register offsets */
#define UART_CR         REG32(UART_BASE + 0x00)   /* Control */
#define UART_MR         REG32(UART_BASE + 0x04)   /* Mode */
#define UART_IER        REG32(UART_BASE + 0x08)   /* Interrupt enable */
#define UART_IDR        REG32(UART_BASE + 0x0C)   /* Interrupt disable */
#define UART_BAUDGEN    REG32(UART_BASE + 0x18)   /* Baud rate generator */
#define UART_SR         REG32(UART_BASE + 0x2C)   /* Status */
#define UART_FIFO       REG32(UART_BASE + 0x30)   /* TX/RX FIFO */
#define UART_BAUDDIV    REG32(UART_BASE + 0x34)   /* Baud rate divider */

/* CR bits */
#define CR_RXRST    (1U << 0)
#define CR_TXRST    (1U << 1)
#define CR_RX_EN    (1U << 2)
#define CR_RX_DIS   (1U << 3)
#define CR_TX_EN    (1U << 4)
#define CR_TX_DIS   (1U << 5)

/* MR bits: 8N1, normal mode, no parity */
#define MR_NORMAL   0x00000020U  /* CHRL=11 (8-bit), PAR=100 (none), NBSTOP=00 (1 stop) */

/* SR bits */
#define SR_TX_FULL  (1U << 4)   /* TX FIFO full */
#define SR_TX_EMPTY (1U << 3)   /* TX FIFO empty */

/*
 * Baud rate formula: baud = uart_ref_clk / (CD * (BDIV + 1))
 *
 * uart_ref_clk: load_r5.tcl sets UART0_REF_CTRL (0xFF5E0074) to
 * SRCSEL=IOPLL, DIV1=1, DIV0=15. After psu_init (FBDIV=45 → IOPLL=1500 MHz):
 *   UART0_REF_CLK = 1500 / 1 / 15 = 100 MHz.
 * psu_init is run by zub_ctl --pre-xsct (psu_init_only.tcl) before OpenOCD.
 *
 * For 115200: CD = 62, BDIV = 13  → 100000000 / (62×14) = 115207 baud (0.06% err)
 */
#define UART_REF_CLK_HZ  100000000UL

static void _compute_baud(uint32_t baud, uint32_t *cd, uint32_t *bdiv)
{
    /* Iterate BDIV from 4..255, pick the divisor that minimises error. */
    uint32_t best_cd = 62, best_bdiv = 13, best_err = ~0U;
    for (uint32_t b = 4; b <= 255; b++) {
        uint32_t c = UART_REF_CLK_HZ / (baud * (b + 1));
        if (c == 0 || c > 65535) continue;
        uint32_t actual = UART_REF_CLK_HZ / (c * (b + 1));
        uint32_t err = (actual > baud) ? (actual - baud) : (baud - actual);
        if (err < best_err) { best_err = err; best_cd = c; best_bdiv = b; }
    }
    *cd   = best_cd;
    *bdiv = best_bdiv;
}

void uart_init(uint32_t baud_rate)
{
    /* Disable all interrupts */
    UART_IDR = 0xFFFFFFFFU;

    /* Reset TX and RX; poll for self-clear with a generous timeout so a
     * mis-powered UART clock cannot cause an infinite spin. */
    UART_CR = CR_TXRST | CR_RXRST;
    for (volatile uint32_t _i = 0; _i < 2000000U; _i++) {
        if (!(UART_CR & (CR_TXRST | CR_RXRST))) break;
    }

    /* 8N1, normal channel mode */
    UART_MR = MR_NORMAL;

    /* Baud rate */
    uint32_t cd, bdiv;
    _compute_baud(baud_rate, &cd, &bdiv);
    UART_BAUDGEN = cd;
    UART_BAUDDIV = bdiv;

    /* Enable TX (and RX so the FIFO doesn't stall) */
    UART_CR = CR_TX_EN | CR_RX_EN;
}

void uart_putc(char c)
{
    while (UART_SR & SR_TX_FULL) {}   /* spin while TX FIFO is full */
    UART_FIFO = (uint32_t)(unsigned char)c;
}

void uart_print(const char *s)
{
    while (*s) uart_putc(*s++);
}
