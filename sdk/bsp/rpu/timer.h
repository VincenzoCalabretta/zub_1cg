#pragma once

#include <stdint.h>

/*
 * RPU PL390 + TTC0 timer driver for Zynq UltraScale+ R5F.
 *
 * timer_init() configures the RPU GIC and TTC0 channel 0 with the counter
 * stopped. timer_start() enables the periodic tick after ThreadX has restored
 * its first thread context.
 */

void timer_init(void);
void timer_start(void);

/*
 * Called by ThreadX during kernel entry (_tx_initialize_low_level hook).
 * Implemented here so board code owns the hardware initialisation order.
 */
void _tx_initialize_low_level(void);
