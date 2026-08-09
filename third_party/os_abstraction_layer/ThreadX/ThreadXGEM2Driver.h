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

/* Driver-private marker for a packet whose complete data area is mapped
 * non-cacheable. nx_packet_allocate() clears this field on pool reuse. */
#define GEM2_PACKET_NONCACHE 0x80000000UL

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
void gem2_diag_get_tx_extra(unsigned int *txused_count, unsigned int *last_txsr,
                             unsigned int *tx_deferred_requests,
                             unsigned int *tx_deferred_runs);
void gem2_diag_get_tx_recover(unsigned int *attempts, unsigned int *txqbase_before,
                               unsigned int *txqbase_after);
void gem2_diag_get_tx_dst(unsigned int *dst_msw, unsigned int *dst_lsw, unsigned int *cmd);
void gem2_diag_get_arp_dump(unsigned char *out28, unsigned int *valid);
void gem2_diag_get_req_dump(unsigned int *addr_lo, unsigned int *addr_hi, unsigned int *sizeof_req,
                             unsigned char *out48);
unsigned int gem2_diag_get_tx_dropped_bad_dst(void);
void gem2_diag_get_tx_pkt_state(unsigned int *retransmit_count, unsigned int *prepend_before,
                                 unsigned int *prepend_after, unsigned int *append,
                                 unsigned int *length_before, unsigned int *length_after);
/* ip/msw/lsw must each point to a 4-element array (GEM2_ARP_CACHE_SIZE in
 * ThreadXGEM2Driver.c). */
void gem2_diag_get_arp_cache(unsigned int *count, unsigned int *ip, unsigned int *msw, unsigned int *lsw);
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

/**
 * @brief Recover an RX-only DMA stall while an active trace stream is still
 * generating TX traffic. Call once per second from a thread context.
 */
void gem2_rx_poll_recover(void);

/** @brief Number of RX-only polling recoveries attempted. */
unsigned int gem2_diag_get_rx_poll_recover(void);

/**
 * @brief Read the PHY's live link status over MDIO (BMSR bit 2), independent
 * of any GEM2/NetX state — see gem2_link_poll_recover() for why this reads
 * the PHY directly rather than trusting NWSR or a cached flag.
 *
 * *phy_found reports whether gem2_phy_enable_tx_delay() ever located the PHY
 * on the MDIO bus (it always should have, by LINK_ENABLE time); if it is 0
 * the other outputs are meaningless. *link_up is bit 2 of the freshly-read
 * BMSR (1 = link established).
 */
void gem2_diag_get_phy_link(unsigned int *phy_addr, unsigned int *phy_found,
                             unsigned int *bmsr, unsigned int *link_up);

/**
 * @brief Tell the driver whether an application-level consumer currently
 * expects GEM2 interrupt activity (e.g. an active Orbtrace capture with a
 * connected TCP client). Call with 1 when starting, 0 when stopping.
 *
 * gem2_link_poll_recover()'s total-freeze detection only fires while this is
 * set, so idle periods with no client connected (where isr_calls legitimately
 * never advances) cannot trigger a spurious full MAC/PHY reinit.
 */
void gem2_set_trace_active(unsigned int active);

/**
 * @brief Poll for a total GEM2 interrupt freeze (isr_calls not advancing at
 * all, not just tx_count backlog) and recover with a full MAC/PHY reinit if
 * it persists.
 *
 * gem2_tx_poll_recover() only detects a stall when tx_count > 0 — it never
 * fires if nothing is queued for TX, which is exactly the state observed
 * during the 2026-08-09 handoff's sustained-load freeze (tx_count==0,
 * isr_calls frozen, eventually not even ARP-responsive). This is a second,
 * independent detector gated by gem2_set_trace_active() rather than ring
 * occupancy: after several consecutive seconds of zero ISR activity while
 * active, it escalates past the lighter TXQBASE-only resync in
 * gem2_tx_stall_recover() to a full XEmacPs_Stop()/reconfigure/Start() cycle
 * plus a forced PHY autonegotiation restart, on the theory that whatever
 * state the hardware reached, the existing narrower recovery paths were
 * insufficient to escape it. Call periodically (e.g. once per second) from
 * a non-ISR context, same as gem2_tx_poll_recover().
 */
void gem2_link_poll_recover(void);

/**
 * @brief Temporary bring-up diagnostic: how many times
 * gem2_link_poll_recover() has escalated to a full MAC/PHY reinit.
 */
void gem2_diag_get_link_recover(unsigned int *attempts);

/**
 * @brief Temporary bring-up diagnostic: gem2_link_poll_recover()'s own
 * gating/escalation state (sCtx.trace_active and its internal stall-tick
 * counter), exposed directly so a sustained isr_calls freeze can be
 * distinguished as "trace_active was already 0" vs. "trace_active stayed 1
 * but the escalation still never fired".
 */
void gem2_diag_get_link_poll_state(unsigned int *trace_active, unsigned int *stall_ticks);

#ifdef __cplusplus
}
#endif

#endif /* THREADX_GEM2_DRIVER_H */
