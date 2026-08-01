/*
 * ThreadXGEM2Driver.c — Eclipse NetX Duo Ethernet driver for ZynqMP GEM2
 *
 * SPDX-License-Identifier: MIT
 *
 * Architecture
 * ────────────
 * The driver manages two DMA BD rings (TX and RX), each with GEM2_BD_COUNT
 * descriptors.  RX NX_PACKETs are pre-allocated at LINK_INITIALIZE time and
 * their data buffers are pointed to by the RX BDs.  When the MAC delivers a
 * frame (ISR), the driver hands the packet to NetX and refills the BD with a
 * new packet allocation.
 *
 * TX is zero-copy for single-buffer NX_PACKET chains.  Multi-buffer chains
 * are flattened into a per-slot static TX buffer so the caller's packet can
 * be released immediately.
 *
 * GIC integration
 * ───────────────
 * board/a53/timer.c enables GEM2 in the GIC (INTID 93) and routes every
 * GEM2 interrupt to gem2_irq_handler() declared weak there.  This file
 * provides the strong override.
 *
 * ZynqMP-specific workaround
 * ──────────────────────────
 * ZynqMP GEM checks TXQ1 (priority queue) before TXQ0.  After reset,
 * TXQ1BASE points at a stale address in OCM / exception-vector space
 * (0x380).  We park a dummy BD with USED=1|WRAP=1 at TXQ1 so the DMA
 * skips it immediately and falls through to TXQ0 with our real frames.
 * This mirrors the workaround in eth_loopback/src/eth_loopback.c.
 */

#include "ThreadXGEM2Driver.h"

#include <string.h>

/* tx_port.h (pulled in via nx_api.h above) typedef's LONG=int and ULONG=unsigned int.
 * xil_types.h guards its own typedef with #if !defined(LONG) || !defined(ULONG),
 * which is a macro check — not a typedef check.  Define the macros here so the
 * xil_types.h guard evaluates false and the conflicting typedefs are skipped. */
#define LONG  LONG
#define ULONG ULONG

#include "xemacps.h"
#include "xemacps_hw.h"
#include "xil_cache.h"
#include "xparameters.h"

/* Ethernet protocol constants — not exported by NetX public headers;
 * defined locally following the pattern in nx_ram_network_driver.c. */
#ifndef NX_ETHERNET_IP
#define NX_ETHERNET_IP   0x0800U
#endif
#ifndef NX_ETHERNET_IPV6
#define NX_ETHERNET_IPV6 0x86DDU
#endif
#ifndef NX_ETHERNET_ARP
#define NX_ETHERNET_ARP  0x0806U
#endif
#ifndef NX_ETHERNET_RARP
#define NX_ETHERNET_RARP 0x8035U
#endif
#ifndef NX_ETHERNET_SIZE
#define NX_ETHERNET_SIZE 14U
#endif

/* ── Tunables ──────────────────────────────────────────────────────────── */

/* BD ring depth — must be a power of 2 for the index-wrap arithmetic */
#define GEM2_BD_COUNT    4U

/* XEmacPs DMA requires 64-byte aligned BDs */
#define GEM2_BD_ALIGN    64U

/* Maximum receive frame size (Ethernet MTU 1500 + 14 header; no FCS) */
#define GEM2_RX_BUFSIZE  1536U

/* Number of TX poll iterations before declaring TX timeout */
#define GEM2_TX_POLL     500000U

/* GEM2 local MAC address (locally-administered) */
static u8 sMAC[6] = { 0x00, 0x0A, 0x35, 0x00, 0x01, 0x02 };

/* ── Driver context ────────────────────────────────────────────────────── */

typedef struct {
    XEmacPs         mac;
    NX_IP          *ip_ptr;
    NX_PACKET_POOL *pool_ptr;

    /* TX BD ring + ZynqMP TXQ1 dummy BD */
    u8 tx_bd[GEM2_BD_COUNT * GEM2_BD_ALIGN] __attribute__((aligned(GEM2_BD_ALIGN)));
    u8 tx_q1_dummy[GEM2_BD_ALIGN]           __attribute__((aligned(GEM2_BD_ALIGN)));

    /* Static per-slot TX buffers for multi-buffer packet flattening */
    u8 tx_buf[GEM2_BD_COUNT][GEM2_RX_BUFSIZE] __attribute__((aligned(GEM2_BD_ALIGN)));

    /* NX_PACKET pointers for in-flight TX (released on TX complete) */
    NX_PACKET *tx_pkts[GEM2_BD_COUNT];

    /* TX ring head (next free BD) and tail (oldest in-flight BD) */
    u32 tx_head;
    u32 tx_tail;
    u32 tx_count;   /* BDs currently in flight */

    /* RX BD ring */
    u8 rx_bd[GEM2_BD_COUNT * GEM2_BD_ALIGN] __attribute__((aligned(GEM2_BD_ALIGN)));

    /* Pre-allocated NX_PACKETs backing the RX BDs */
    NX_PACKET *rx_pkts[GEM2_BD_COUNT];

    /* Next RX BD to check for completion (software tail pointer) */
    u32 rx_tail;

    UINT initialized;
} Gem2Ctx;

static Gem2Ctx sCtx;

/* ── Forward declarations ──────────────────────────────────────────────── */
static void gem2_initialize(NX_IP_DRIVER *req);
static void gem2_enable(NX_IP_DRIVER *req);
static void gem2_disable(NX_IP_DRIVER *req);
static void gem2_packet_send(NX_IP_DRIVER *req);
static void gem2_rx_process(void);
static void gem2_tx_cleanup(void);
static UINT gem2_alloc_rx_packet(u32 slot);

/* ── Driver entry point ────────────────────────────────────────────────── */

void nx_driver_gem2(NX_IP_DRIVER *req)
{
    switch (req->nx_ip_driver_command) {
        case NX_LINK_INITIALIZE:
            gem2_initialize(req);
            break;
        case NX_LINK_ENABLE:
            gem2_enable(req);
            break;
        case NX_LINK_DISABLE:
            gem2_disable(req);
            break;
        case NX_LINK_PACKET_SEND:
        case NX_LINK_PACKET_BROADCAST:
            gem2_packet_send(req);
            break;
        case NX_LINK_MULTICAST_JOIN:
        case NX_LINK_MULTICAST_LEAVE:
            /* GEM2 hardware hash-based multicast filtering not yet configured */
            req->nx_ip_driver_status = NX_SUCCESS;
            break;
        case NX_LINK_GET_STATUS:
            /* Always report link up — add PHY polling for production use */
            *req->nx_ip_driver_return_ptr = NX_TRUE;
            req->nx_ip_driver_status      = NX_SUCCESS;
            break;
        case NX_LINK_DEFERRED_PROCESSING:
            /* IP thread processes deferred packets; nothing extra needed here */
            req->nx_ip_driver_status = NX_SUCCESS;
            break;
        default:
            req->nx_ip_driver_status = NX_UNHANDLED_COMMAND;
            break;
    }
}

/* ── LINK_INITIALIZE ───────────────────────────────────────────────────── */

static void gem2_initialize(NX_IP_DRIVER *req)
{
    LONG rc;
    u32  i;

    sCtx.ip_ptr   = req->nx_ip_driver_ptr;
    sCtx.pool_ptr = req->nx_ip_driver_ptr->nx_ip_default_packet_pool;

    /* 1. Init XEmacPs driver */
    XEmacPs_Config *cfg = XEmacPs_LookupConfig(XPAR_XEMACPS_0_BASEADDR);
    if (!cfg) { req->nx_ip_driver_status = NX_NOT_SUCCESSFUL; return; }

    rc = XEmacPs_CfgInitialize(&sCtx.mac, cfg, cfg->BaseAddress);
    if (rc != XST_SUCCESS) { req->nx_ip_driver_status = NX_NOT_SUCCESSFUL; return; }

    /* 2. Configure MAC speed, disable all MAC interrupts (polled init) */
    XEmacPs_IntDisable(&sCtx.mac, 0x7FFFFFFFU);

    u32 nwcfg = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCFG_OFFSET);
    nwcfg &= ~XEMACPS_NWCFG_MDCCLKDIV_MASK;
    nwcfg |= (6U << 18U);                              /* MDC ÷48 */
    nwcfg |= XEMACPS_NWCFG_100_MASK | XEMACPS_NWCFG_FDEN_MASK;
    XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCFG_OFFSET, nwcfg);

    /* 3. Set MAC options */
    rc = XEmacPs_SetOptions(&sCtx.mac,
             XEMACPS_PROMISC_OPTION    |
             XEMACPS_FCS_INSERT_OPTION |
             XEMACPS_FCS_STRIP_OPTION);
    if (rc != XST_SUCCESS) { req->nx_ip_driver_status = NX_NOT_SUCCESSFUL; return; }

    /* 4. Set MAC address from NetX interface or use default */
    XEmacPs_SetMacAddress(&sCtx.mac, (void *)sMAC, 1);

    /* Publish MAC address to the NetX interface */
    req->nx_ip_driver_interface->nx_interface_physical_address_msw =
        (ULONG)((sMAC[0] << 8) | sMAC[1]);
    req->nx_ip_driver_interface->nx_interface_physical_address_lsw =
        (ULONG)((sMAC[2] << 24) | (sMAC[3] << 16) | (sMAC[4] << 8) | sMAC[5]);

    /* 5. TX BD ring */
    rc = XEmacPs_BdRingCreate(
            &XEmacPs_GetTxRing(&sCtx.mac),
            (UINTPTR)sCtx.tx_bd, (UINTPTR)sCtx.tx_bd,
            GEM2_BD_ALIGN, GEM2_BD_COUNT);
    if (rc != XST_SUCCESS) { req->nx_ip_driver_status = NX_NOT_SUCCESSFUL; return; }

    /* 6. RX BD ring */
    rc = XEmacPs_BdRingCreate(
            &XEmacPs_GetRxRing(&sCtx.mac),
            (UINTPTR)sCtx.rx_bd, (UINTPTR)sCtx.rx_bd,
            GEM2_BD_ALIGN, GEM2_BD_COUNT);
    if (rc != XST_SUCCESS) { req->nx_ip_driver_status = NX_NOT_SUCCESSFUL; return; }

    /* 7. Pre-allocate RX packets and point BDs at their data areas */
    for (i = 0U; i < GEM2_BD_COUNT; i++) {
        if (gem2_alloc_rx_packet(i) != NX_SUCCESS) {
            req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
            return;
        }
    }

    /* 8. ZynqMP TXQ1 dummy BD — park a USED+WRAP sentinel so the DMA skips
     *    priority queue 1 and falls through to our real frames on queue 0.
     *    Without this the DMA spins on whatever stale address TXQ1BASE holds
     *    after reset (typically 0x380 in OCM / exception-vector space). */
    memset(sCtx.tx_q1_dummy, 0, GEM2_BD_ALIGN);
    XEmacPs_BdWrite((XEmacPs_Bd *)sCtx.tx_q1_dummy, XEMACPS_BD_STAT_OFFSET,
                    XEMACPS_TXBUF_USED_MASK | XEMACPS_TXBUF_WRAP_MASK);
    Xil_DCacheFlushRange((UINTPTR)sCtx.tx_q1_dummy, GEM2_BD_ALIGN);

    sCtx.tx_head = 0U;
    sCtx.tx_tail = 0U;
    sCtx.tx_count = 0U;
    sCtx.rx_tail  = 0U;
    sCtx.initialized = NX_TRUE;

    req->nx_ip_driver_interface->nx_interface_valid = NX_TRUE;
    req->nx_ip_driver_status = NX_SUCCESS;
}

/* ── LINK_ENABLE ───────────────────────────────────────────────────────── */

static void gem2_enable(NX_IP_DRIVER *req)
{
    if (!sCtx.initialized) { req->nx_ip_driver_status = NX_NOT_SUCCESSFUL; return; }

    /* Point HW at BD rings (required for ZynqMP GEM) */
    XEmacPs_SetQueuePtr(&sCtx.mac, (UINTPTR)sCtx.tx_q1_dummy, 1U, 1U);
    XEmacPs_SetQueuePtr(&sCtx.mac, (UINTPTR)sCtx.tx_bd, 0U, 1U);
    XEmacPs_SetQueuePtr(&sCtx.mac, (UINTPTR)sCtx.rx_bd, 0U, 0U);

    XEmacPs_Start(&sCtx.mac);

    /* Enable RX and TX complete interrupts in the MAC */
    XEmacPs_IntEnable(&sCtx.mac,
                      XEMACPS_IXR_FRAMERX_MASK | XEMACPS_IXR_TXCOMPL_MASK);

    req->nx_ip_driver_interface->nx_interface_link_up = NX_TRUE;
    req->nx_ip_driver_status = NX_SUCCESS;
}

/* ── LINK_DISABLE ──────────────────────────────────────────────────────── */

static void gem2_disable(NX_IP_DRIVER *req)
{
    XEmacPs_IntDisable(&sCtx.mac, 0x7FFFFFFFU);
    XEmacPs_Stop(&sCtx.mac);
    req->nx_ip_driver_interface->nx_interface_link_up = NX_FALSE;
    req->nx_ip_driver_status = NX_SUCCESS;
}

/* ── LINK_PACKET_SEND ──────────────────────────────────────────────────── */

static void gem2_packet_send(NX_IP_DRIVER *req)
{
    NX_PACKET *pkt   = req->nx_ip_driver_packet;
    u32 slot = sCtx.tx_head;

    if (sCtx.tx_count >= GEM2_BD_COUNT) {
        /* Ring full — drop; caller must handle retransmit */
        nx_packet_transmit_release(pkt);
        req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
        return;
    }

    /* Flatten the packet chain into the static TX buffer for this slot.
     * This adds a copy but avoids aliasing the NX_PACKET data area while
     * the DMA is active and simplifies cache management. */
    u8 *dst  = sCtx.tx_buf[slot];
    u32 total = 0U;
    for (NX_PACKET *frag = pkt; frag && total < GEM2_RX_BUFSIZE; frag = frag->nx_packet_next) {
        ULONG chunk = (ULONG)(frag->nx_packet_append_ptr - frag->nx_packet_prepend_ptr);
        if (total + chunk > GEM2_RX_BUFSIZE) { chunk = GEM2_RX_BUFSIZE - total; }
        memcpy(dst + total, frag->nx_packet_prepend_ptr, chunk);
        total += chunk;
    }

    /* Set up TX BD */
    XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.tx_bd + slot * GEM2_BD_ALIGN);
    memset(bd, 0, GEM2_BD_ALIGN);
    XEmacPs_BdSetAddressTx(bd, (UINTPTR)(sCtx.tx_buf[slot]));
    XEmacPs_BdWrite(bd, XEMACPS_BD_STAT_OFFSET,
                    (total & 0x3FFFU)       |
                    XEMACPS_TXBUF_LAST_MASK |
                    /* set WRAP on last BD in ring */
                    ((slot == GEM2_BD_COUNT - 1U) ? XEMACPS_TXBUF_WRAP_MASK : 0U));

    Xil_DCacheFlushRange((UINTPTR)sCtx.tx_buf[slot], total);
    Xil_DCacheFlushRange((UINTPTR)bd, GEM2_BD_ALIGN);

    sCtx.tx_pkts[slot] = pkt;
    sCtx.tx_head = (slot + 1U) % GEM2_BD_COUNT;
    sCtx.tx_count++;

    XEmacPs_Transmit(&sCtx.mac);  /* set STARTTX in NWCTRL */

    req->nx_ip_driver_status = NX_SUCCESS;
}

/* ── RX processing (called from ISR) ──────────────────────────────────── */

static void gem2_rx_process(void)
{
    for (u32 i = 0U; i < GEM2_BD_COUNT; i++) {
        u32 slot = sCtx.rx_tail;
        XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.rx_bd + slot * GEM2_BD_ALIGN);

        Xil_DCacheInvalidateRange((UINTPTR)bd, GEM2_BD_ALIGN);
        u32 addr_word = XEmacPs_BdRead(bd, XEMACPS_BD_ADDR_OFFSET);

        if (!(addr_word & XEMACPS_RXBUF_NEW_MASK)) {
            break;  /* DMA still owns this BD */
        }

        u32 stat_word = XEmacPs_BdRead(bd, XEMACPS_BD_STAT_OFFSET);
        u32 frame_len = stat_word & XEMACPS_RXBUF_LEN_MASK;

        NX_PACKET *pkt = sCtx.rx_pkts[slot];
        Xil_DCacheInvalidateRange((UINTPTR)pkt->nx_packet_prepend_ptr, frame_len);

        /* Determine EtherType (bytes 12–13 of Ethernet header) */
        u8  *eth  = (u8 *)pkt->nx_packet_prepend_ptr;
        u16  etype = (u16)((eth[12] << 8) | eth[13]);

        /* Adjust pointers past the Ethernet header */
        pkt->nx_packet_prepend_ptr += NX_ETHERNET_SIZE;
        pkt->nx_packet_length       = frame_len - NX_ETHERNET_SIZE;
        pkt->nx_packet_append_ptr   = pkt->nx_packet_prepend_ptr + pkt->nx_packet_length;

        /* Route to NetX */
        if (etype == NX_ETHERNET_IP || etype == NX_ETHERNET_IPV6) {
            _nx_ip_packet_deferred_receive(sCtx.ip_ptr, pkt);
        } else if (etype == NX_ETHERNET_ARP) {
            _nx_arp_packet_deferred_receive(sCtx.ip_ptr, pkt);
        } else if (etype == NX_ETHERNET_RARP) {
            _nx_rarp_packet_deferred_receive(sCtx.ip_ptr, pkt);
        } else {
            nx_packet_release(pkt);
        }

        /* Refill the slot and reset BD ownership to DMA */
        (void)gem2_alloc_rx_packet(slot);

        /* Advance tail */
        sCtx.rx_tail = (slot + 1U) % GEM2_BD_COUNT;
    }
}

/* ── TX completion cleanup (called from ISR) ───────────────────────────── */

static void gem2_tx_cleanup(void)
{
    while (sCtx.tx_count > 0U) {
        u32 slot = sCtx.tx_tail;
        XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.tx_bd + slot * GEM2_BD_ALIGN);

        Xil_DCacheInvalidateRange((UINTPTR)bd, GEM2_BD_ALIGN);
        u32 stat = XEmacPs_BdRead(bd, XEMACPS_BD_STAT_OFFSET);

        if (!(stat & XEMACPS_TXBUF_USED_MASK)) {
            break;  /* DMA still owns this BD */
        }

        /* Release the associated NX_PACKET */
        if (sCtx.tx_pkts[slot]) {
            nx_packet_transmit_release(sCtx.tx_pkts[slot]);
            sCtx.tx_pkts[slot] = NX_NULL;
        }

        sCtx.tx_tail = (slot + 1U) % GEM2_BD_COUNT;
        sCtx.tx_count--;
    }
}

/* ── Allocate one RX NX_PACKET and configure its BD ───────────────────── */

static UINT gem2_alloc_rx_packet(u32 slot)
{
    NX_PACKET *pkt = NX_NULL;
    if (nx_packet_allocate(sCtx.pool_ptr, &pkt, NX_RECEIVE_PACKET, NX_NO_WAIT)
            != NX_SUCCESS) {
        return NX_NO_PACKET;
    }

    sCtx.rx_pkts[slot] = pkt;

    XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.rx_bd + slot * GEM2_BD_ALIGN);

    /* Clear stat word; set BD address with DMA-owns (bit 0 = 0) */
    XEmacPs_BdWrite(bd, XEMACPS_BD_STAT_OFFSET, 0U);
    XEmacPs_BdSetAddressRx(bd, (UINTPTR)pkt->nx_packet_prepend_ptr);

    /* Preserve WRAP bit on the last BD so the ring is circular */
    if (slot == GEM2_BD_COUNT - 1U) {
        u32 addr = XEmacPs_BdRead(bd, XEMACPS_BD_ADDR_OFFSET);
        XEmacPs_BdWrite(bd, XEMACPS_BD_ADDR_OFFSET, addr | XEMACPS_RXBUF_WRAP_MASK);
    }

    Xil_DCacheFlushRange((UINTPTR)bd, GEM2_BD_ALIGN);
    Xil_DCacheFlushRange((UINTPTR)pkt->nx_packet_prepend_ptr, GEM2_RX_BUFSIZE);

    return NX_SUCCESS;
}

/* ── GEM2 IRQ handler (overrides weak no-op in board/a53/timer.c) ──────── */

void gem2_irq_handler(void)
{
    if (!sCtx.initialized) { return; }

    u32 isr = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_ISR_OFFSET);

    /* Clear interrupts by writing 1s to the ISR (write-to-clear) */
    XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_ISR_OFFSET, isr);

    if (isr & XEMACPS_IXR_FRAMERX_MASK) {
        gem2_rx_process();
    }
    if (isr & XEMACPS_IXR_TXCOMPL_MASK) {
        gem2_tx_cleanup();
    }
}
