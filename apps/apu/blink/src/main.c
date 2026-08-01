/*
 * Bare-metal A53 hardware verification — ZUBoard 1CG.
 *
 * What this proves when it runs:
 *   1. JTAG ELF load works (psu_init + xsct dow + con)
 *   2. UART0 (0xFF000000) wired to ttyUSB1 at 115200 baud
 *   3. AXI GPIO (0xA0000000) reachable from A53 via HPM0_FPD
 *      (requires PL bitstream programmed)
 *
 * Build:  nix develop --command bazel build --config=apu //apps/apu/blink
 * Load:   xsct scripts/xsct/load_a53.tcl
 * Watch:  picocom -b 115200 /dev/ttyUSB1
 */

#include "xil_printf.h"

#define RGB_BASE  0xa0000000UL
#define DATA_OFF  0x0UL
#define TRI_OFF   0x4UL

static inline unsigned int rd(unsigned long addr)
{
    return *(volatile unsigned int *)addr;
}

static inline void wr(unsigned long addr, unsigned int v)
{
    *(volatile unsigned int *)addr = v;
}

static void delay(void)
{
    volatile unsigned long i;
    for (i = 50000000UL; i; i--);
}

int main(void)
{
    xil_printf("\r\n=== ZUBoard 1CG — A53 bare-metal ===\r\n");
    xil_printf("UART OK\r\n");

    /* GPIO: set TRI=0 (all outputs).  Hangs here if PL not programmed. */
    xil_printf("GPIO TRI (before): 0x%08x\r\n", rd(RGB_BASE + TRI_OFF));
    wr(RGB_BASE + TRI_OFF, 0x0U);
    xil_printf("GPIO TRI (after):  0x%08x  (expect 0x0)\r\n",
               rd(RGB_BASE + TRI_OFF));

    xil_printf("Cycling RGB LED — 0..7 with 1 s hold each\r\n");

    unsigned int idx = 0;
    while (1) {
        unsigned int v = idx & 0x7U;
        wr(RGB_BASE + DATA_OFF, v);
        unsigned int rb = rd(RGB_BASE + DATA_OFF);
        xil_printf("LED 0x%x  readback 0x%x\r\n", v, rb);
        idx++;
        delay();
    }
}
