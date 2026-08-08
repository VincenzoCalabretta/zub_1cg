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
                    unsigned int *tx_complete, unsigned int *last_tx_stat,
                    unsigned int *tx_head, unsigned int *tx_tail, unsigned int *tx_count,
                    unsigned int *last_isr, unsigned int *rxused_count,
                    unsigned int *driver_cmd_count, unsigned int *last_driver_cmd,
                    unsigned int *last_driver_status);
void gem2_diag_get_ip_dump(unsigned char *out40);
void gem2_diag_get_tx_extra(unsigned int *txused_count, unsigned int *last_txsr);
void gem2_diag_get_tx_recover(unsigned int *attempts, unsigned int *txqbase_before,
                               unsigned int *txqbase_after);
void gem2_diag_get_tx_dst(unsigned int *dst_msw, unsigned int *dst_lsw, unsigned int *cmd);
void gem2_diag_get_rx_bd_dump(unsigned int *rx_tail, unsigned int *rxqbase, unsigned int *rx_bd_base,
                               unsigned int addr_words[4], unsigned int stat_words[4]);
void gem2_diag_get_tx_bd_dump(unsigned int *tx_head, unsigned int *tx_tail, unsigned int *tx_count,
                               unsigned int *txqbase, unsigned int *tx_bd_base,
                               unsigned int addr_words[4], unsigned int stat_words[4]);

/**
 * @brief Poll for a wedged TX ring and recover it if stalled.
 *
 * Call periodically (e.g. once per second) from a non-ISR context. The DMA
 * halts TXQ0 scanning when it walks into an already-USED descriptor and
 * does not reliably generate a fresh interrupt to report that — see
 * ThreadXGEM2Driver.c's XEMACPS_IXR_TXUSED_MASK comment. This detects "no
 * TX completions since the last call, but frames are still queued" and
 * performs the same TXQBASE resync recovery the ISR attempts, so a stall
 * that produces no further interrupt at all still gets recovered.
 */
void gem2_tx_poll_recover(void);

#ifdef __cplusplus
}
#endif

#endif /* THREADX_GEM2_DRIVER_H */
