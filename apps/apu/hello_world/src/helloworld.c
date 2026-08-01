/******************************************************************************
* Copyright (C) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

/*
 * Include only platform.h (safe: no Xilinx type headers when -DSDT) and
 * tx_api.h.  Xilinx BSP headers (xil_types.h, xgpio_l.h, …) define LONG and
 * ULONG as 64-bit `long`, while ThreadX's tx_port.h defines them as 32-bit
 * `int`.  Keeping them in separate translation units avoids the conflict.
 *
 * GPIO is accessed directly (same as XGpio_WriteReg/ReadReg macros).
 * xil_printf is forward-declared to avoid pulling in xil_types.h.
 */

#include "platform.h"
#include "tx_api.h"
#include "timer.h"

/* xil_printf lives in xuartps_hw.c — forward-declare to skip xil_printf.h */
extern void xil_printf(const char *fmt, ...);

/* AXI GPIO peripheral at 0xa0000000 (PL bitstream) */
#define RGB_BASE          0xa0000000UL
#define XGPIO_DATA_OFFSET 0x0U
#define XGPIO_TRI_OFFSET  0x4U

static inline void gpio_out(ULONG base, UINT off, UINT val)
{
    /* ULONG is 32-bit on this ThreadX port; use unsigned long for the address
     * cast so the pointer is the correct width on AArch64.          */
    *(volatile UINT *)((unsigned long)base + off) = val;
}

static inline UINT gpio_in(ULONG base, UINT off)
{
    return *(volatile UINT *)((unsigned long)base + off);
}

/* ── ThreadX objects ─────────────────────────────────────────────────── */

static TX_THREAD led_thread;
static ULONG     led_stack[1024];   /* 4 KiB: ULONG=u32 on this port */

/* ── LED thread: cycle all 3-bit RGB values once per second ─────────── */

static void led_entry(ULONG arg)
{
    (void)arg;

    /* Print before any AXI access: proves this thread was scheduled. */
    xil_printf("led thread: running\r\n");

    /* Set all three RGB GPIO pins to output (TRI = 0) */
    gpio_out(RGB_BASE, XGPIO_TRI_OFFSET, 0U);

    static const UINT vals[] = { 0,1,2,3,4,5,6,7 };
    UINT idx = 0;
    for (;;) {
        UINT v = vals[idx];
        gpio_out(RGB_BASE, XGPIO_DATA_OFFSET, v);
        UINT rb = gpio_in(RGB_BASE, XGPIO_DATA_OFFSET);
        xil_printf("GPIO=0x%x  readback=0x%x\r\n", v, rb);
        idx = (idx + 1U) % 8U;
        tx_thread_sleep(100);   /* 100 ticks × 10 ms/tick = 1 s */
    }
}

/* ── Application definition (called by ThreadX before scheduler) ─────── */

void tx_application_define(void *first_unused_memory)
{
    (void)first_unused_memory;  /* static stack: dynamic pool not used */

    tx_thread_create(
        &led_thread,        /* control block  */
        "led",              /* name           */
        led_entry,          /* entry function */
        0,                  /* entry arg      */
        led_stack,          /* stack start    */
        sizeof(led_stack),  /* stack bytes    */
        1, 1,               /* priority / preempt-threshold */
        TX_NO_TIME_SLICE,
        TX_AUTO_START
    );
}

/* ── C entry point ───────────────────────────────────────────────────── */

int main(void)
{
    init_platform();

    xil_printf("ThreadX RGB LED (AArch64 / A53)\r\n");

    /* Program GIC + TTC0 for 100 Hz ThreadX tick; IRQs enabled by ThreadX. */
    timer_init();

    /* Hand control to ThreadX — never returns. */
    tx_kernel_enter();

    return 0;
}
