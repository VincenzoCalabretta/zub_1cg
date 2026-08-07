#pragma once

/*
 * GIC400 + TTC0 timer driver for ZynqMP Cortex-A53.
 *
 * timer_init() programs the GIC and TTC0 channel 0 to fire at 100 Hz,
 * providing the periodic tick that ThreadX needs.  IRQs are NOT enabled
 * here; ThreadX enables them via ERET when the first thread starts.
 *
 * The GIC is also configured to route GEM2 interrupts (SPI 61, INTID 93)
 * to CPU0.  gem2_irq_handler() is declared as a weak no-op here; link
 * ThreadXGEM2Driver to override it with the real NetX receive handler.
 */

void timer_init(void);

/*
 * GEM2 IRQ handler — override by linking ThreadXGEM2Driver.c.
 * The weak definition in timer.c is a no-op so the board BSP compiles
 * without requiring the NetX Duo driver.
 */
void gem2_irq_handler(void);
