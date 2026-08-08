/*
 * ThreadXGEM2Driver.h — Eclipse NetX Duo driver for ZynqMP GEM2 (XEmacPs)
 *
 * SPDX-License-Identifier: MIT
 *
 * Link this module alongside board/a53:bsp and @netxduo to activate the
 * interrupt-driven Ethernet driver.  The strong gem2_irq_handler() defined
 * here overrides the weak no-op in board/a53/timer.c.
 */

#ifndef THREADX_GEM2_DRIVER_H
#define THREADX_GEM2_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nx_api.h"

/**
 * @brief NetX Duo driver entry point for GEM2.
 *
 * Pass this function pointer to nx_ip_create():
 * @code
 *   nx_ip_create(&ip, "IP", IP_ADDR, MASK, &pool, nx_driver_gem2, ...);
 * @endcode
 */
void nx_driver_gem2(NX_IP_DRIVER *driver_req_ptr);

/**
 * @brief GEM2 IRQ handler — called by board/a53/timer.c IRQHandler.
 *
 * Overrides the weak no-op so the timer BSP need not include NetX headers.
 * Do not call directly.
 */
void gem2_irq_handler(void);

/**
 * @brief Temporary bring-up diagnostic: cumulative RX frames processed, TX
 * frames submitted, and GEM2 ISR invocations since gem2_initialize().
 * Not part of the stable driver API.
 */
void gem2_diag_get(unsigned int *rx_frames, unsigned int *tx_frames, unsigned int *isr_calls,
                    unsigned int *last_etype, unsigned int *last_len,
                    unsigned int *tx_complete, unsigned int *last_tx_stat);

#ifdef __cplusplus
}
#endif

#endif /* THREADX_GEM2_DRIVER_H */
