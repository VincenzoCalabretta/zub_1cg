#pragma once

#include <stdint.h>

/*
 * GIC400 + TTC0 timer driver for Zynq UltraScale+ R5F.
 *
 * timer_init() configures GIC400 and TTC0 channel 0 to fire at
 * TX_TIMER_TICKS_PER_SECOND (default 100 Hz) and satisfies the
 * ThreadX requirement that _tx_initialize_low_level() sets up the
 * periodic tick before tx_kernel_enter() starts the scheduler.
 */

void timer_init(void);

/*
 * Called by ThreadX during kernel entry (_tx_initialize_low_level hook).
 * Implemented here so board code owns the hardware initialisation order.
 */
void _tx_initialize_low_level(void);
