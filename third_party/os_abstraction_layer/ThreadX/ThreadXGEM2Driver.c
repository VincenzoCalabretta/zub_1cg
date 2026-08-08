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

/* Temporary bring-up instrumentation (see gem2_initialize/gem2_enable). */
extern void xil_printf(const char *fmt, ...);

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

/* TX ring depth. Root-caused on real hardware: with GEM2_TX_BD_COUNT
 * temporarily forced to 1, only the ring's first descriptor ever completed
 * — the true fault was gem2_packet_send() never building an Ethernet
 * header (NetX hands the driver only the upper-layer payload; see that
 * function's comment), producing undersized/malformed frames on the wire.
 * Restored to GEM2_BD_COUNT now that framing is fixed. */
#define GEM2_TX_BD_COUNT GEM2_BD_COUNT

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
    u8 tx_bd[GEM2_TX_BD_COUNT * GEM2_BD_ALIGN] __attribute__((aligned(GEM2_BD_ALIGN)));
    u8 tx_q1_dummy[GEM2_BD_ALIGN]           __attribute__((aligned(GEM2_BD_ALIGN)));

    /* Static per-slot TX buffers for multi-buffer packet flattening */
    u8 tx_buf[GEM2_TX_BD_COUNT][GEM2_RX_BUFSIZE] __attribute__((aligned(GEM2_BD_ALIGN)));

    /* NX_PACKET pointers for in-flight TX (released on TX complete) */
    NX_PACKET *tx_pkts[GEM2_TX_BD_COUNT];

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

    /* Temporary bring-up diagnostics: total frames the ISR path has
     * actually processed, independent of any raw register snapshot. */
    u32 diag_rx_frames;
    u32 diag_tx_frames;
    u32 diag_isr_calls;
    u32 diag_last_etype;
    u32 diag_last_len;
    u32 diag_tx_complete;
    u32 diag_last_tx_stat;
} Gem2Ctx;

static Gem2Ctx sCtx;

/* Temporary bring-up diagnostic accessor — see main.c's diag_thread_entry. */
void gem2_diag_get(u32 *rx_frames, u32 *tx_frames, u32 *isr_calls, u32 *last_etype, u32 *last_len,
                    u32 *tx_complete, u32 *last_tx_stat, u32 *tx_head, u32 *tx_tail, u32 *tx_count)
{
    *rx_frames = sCtx.diag_rx_frames;
    *tx_frames = sCtx.diag_tx_frames;
    *isr_calls = sCtx.diag_isr_calls;
    *last_etype = sCtx.diag_last_etype;
    *last_len = sCtx.diag_last_len;
    *tx_complete = sCtx.diag_tx_complete;
    *last_tx_stat = sCtx.diag_last_tx_stat;
    *tx_head = sCtx.tx_head;
    *tx_tail = sCtx.tx_tail;
    *tx_count = sCtx.tx_count;
}

/* ── Forward declarations ──────────────────────────────────────────────── */
static void gem2_initialize(NX_IP_DRIVER *req);
static void gem2_enable(NX_IP_DRIVER *req);
static void gem2_disable(NX_IP_DRIVER *req);
static void gem2_packet_send(NX_IP_DRIVER *req);
static void gem2_rx_process(void);
static void gem2_tx_cleanup(void);
static UINT gem2_alloc_rx_packet(u32 slot);
static void gem2_phy_enable_tx_delay(XEmacPs *mac);

/* ── KSZ9131 PHY bring-up: enable the TXC internal DLL delay ─────────────
 *
 * Per Microchip KSZ9131RNX datasheet DS00002841D §4.9.3.1: the RXC delay
 * DLL is enabled by default, but the TXC delay DLL is *disabled* by
 * default (bypass_txdll = 1 in the TX DLL Control Register, MMD device
 * 2h / register 0x4Dh, §5.3.70). With no PS8-side compensating delay
 * either, RGMII TXC/TXD timing at the PHY's sampling latches is unmet:
 * the GEM's own TX DMA reports every descriptor complete (it only tracks
 * its own AXI-side work, not what the PHY does with the bits), but the
 * PHY never latches a decodable frame, so nothing reaches the wire. RX is
 * unaffected because the PHY's RXC delay needs no cooperation from the
 * MAC side. Confirmed by direct-register MDIO scan and packet capture in
 * this session — see ORBTRACE_TEST_REPORT follow-up notes.
 *
 * MDIO indirect MMD access follows the standard IEEE 802.3 clause 22.2.4.3
 * two-register procedure at direct registers 0Dh (MMD Access Control) /
 * 0Eh (MMD Access Address/Data Register), per §5.3 of the same datasheet.
 */
#define KSZ9131_PHY_ID1              0x0022U
#define KSZ9131_MMD_CTRL_REG         0x0DU
#define KSZ9131_MMD_DATA_REG         0x0EU
#define KSZ9131_MMD_FUNC_ADDR        0x0000U
#define KSZ9131_MMD_FUNC_DATA        0x4000U
#define KSZ9131_MMD_DEV_PCS_EXT      0x02U
#define KSZ9131_RX_DLL_CTRL_REG      0x4CU
#define KSZ9131_BYPASS_RXDLL_BIT     (1U << 12)
#define KSZ9131_RXDLL_RESET_BIT      (1U << 13)
#define KSZ9131_TX_DLL_CTRL_REG      0x4DU
#define KSZ9131_BYPASS_TXDLL_BIT     (1U << 12)
#define KSZ9131_TXDLL_RESET_BIT      (1U << 13)

static UINT gem2_phy_find(XEmacPs *mac, u32 *phy_addr)
{
    for (u32 addr = 0U; addr < 32U; addr++) {
        u16 id1 = 0U;
        if (XEmacPs_PhyRead(mac, addr, 2U, &id1) == XST_SUCCESS && id1 == KSZ9131_PHY_ID1) {
            *phy_addr = addr;
            return XST_SUCCESS;
        }
    }
    return XST_FAILURE;
}

static void gem2_mmd_select(XEmacPs *mac, u32 phy_addr, u16 mmd_dev, u16 reg)
{
    XEmacPs_PhyWrite(mac, phy_addr, KSZ9131_MMD_CTRL_REG, KSZ9131_MMD_FUNC_ADDR | mmd_dev);
    XEmacPs_PhyWrite(mac, phy_addr, KSZ9131_MMD_DATA_REG, reg);
    XEmacPs_PhyWrite(mac, phy_addr, KSZ9131_MMD_CTRL_REG, KSZ9131_MMD_FUNC_DATA | mmd_dev);
}

static u16 gem2_mmd_read(XEmacPs *mac, u32 phy_addr, u16 mmd_dev, u16 reg)
{
    u16 value = 0U;
    gem2_mmd_select(mac, phy_addr, mmd_dev, reg);
    XEmacPs_PhyRead(mac, phy_addr, KSZ9131_MMD_DATA_REG, &value);
    return value;
}

static void gem2_mmd_write(XEmacPs *mac, u32 phy_addr, u16 mmd_dev, u16 reg, u16 value)
{
    gem2_mmd_select(mac, phy_addr, mmd_dev, reg);
    XEmacPs_PhyWrite(mac, phy_addr, KSZ9131_MMD_DATA_REG, value);
}

static void gem2_mmd_reset_pulse(XEmacPs *mac, u32 phy_addr, u16 reg, u16 value, u16 reset_bit)
{
    gem2_mmd_write(mac, phy_addr, KSZ9131_MMD_DEV_PCS_EXT, reg, value);
    /* "These bits are not self-clearing and must be set then reset by
     * software" (§4.9.3.1) — pulse *dll_reset after changing DLL config. */
    gem2_mmd_write(mac, phy_addr, KSZ9131_MMD_DEV_PCS_EXT, reg, (u16)(value | reset_bit));
    gem2_mmd_write(mac, phy_addr, KSZ9131_MMD_DEV_PCS_EXT, reg, value);
}

static void gem2_phy_enable_tx_delay(XEmacPs *mac)
{
    u32 phy_addr;
    if (gem2_phy_find(mac, &phy_addr) != XST_SUCCESS) {
        xil_printf("gem2: PHY not found on MDIO bus\r\n");
        return;
    }
    /* The PHY chip is not reset by JTAG/PS resets — only by a full board
     * power cycle — so its register state can carry over from an earlier
     * run (bit us once this session: bypass_rxdll left set from a control
     * experiment persisted across several reflashes). Deterministically
     * set both RX and TX DLL bypass bits explicitly rather than assuming
     * either is at its power-on default. */
    u16 rx_before = gem2_mmd_read(mac, phy_addr, KSZ9131_MMD_DEV_PCS_EXT, KSZ9131_RX_DLL_CTRL_REG);
    u16 rx_enabled = (u16)(rx_before & ~KSZ9131_BYPASS_RXDLL_BIT);
    gem2_mmd_reset_pulse(mac, phy_addr, KSZ9131_RX_DLL_CTRL_REG, rx_enabled, KSZ9131_RXDLL_RESET_BIT);

    u16 before = gem2_mmd_read(mac, phy_addr, KSZ9131_MMD_DEV_PCS_EXT, KSZ9131_TX_DLL_CTRL_REG);
    u16 enabled = (u16)(before & ~KSZ9131_BYPASS_TXDLL_BIT);
    gem2_mmd_reset_pulse(mac, phy_addr, KSZ9131_TX_DLL_CTRL_REG, enabled, KSZ9131_TXDLL_RESET_BIT);

    u16 rx_after = gem2_mmd_read(mac, phy_addr, KSZ9131_MMD_DEV_PCS_EXT, KSZ9131_RX_DLL_CTRL_REG);
    u16 after = gem2_mmd_read(mac, phy_addr, KSZ9131_MMD_DEV_PCS_EXT, KSZ9131_TX_DLL_CTRL_REG);
    xil_printf("gem2: PHY@%lu RX DLL before=0x%x after=0x%x, TX DLL before=0x%x after=0x%x\r\n",
               (unsigned long)phy_addr, rx_before, rx_after, before, after);

    /* IEEE clause-22 PHYs commonly only re-apply internal timing config at
     * link (re)establishment; force one via BMCR so the new DLL settings
     * are live from link-up rather than hot-swapped under an already-
     * established link the PHY never re-timed. */
    u16 bmcr = 0U;
    XEmacPs_PhyRead(mac, phy_addr, 0U, &bmcr);
    XEmacPs_PhyWrite(mac, phy_addr, 0U, (u16)(bmcr | (1U << 9))); /* Restart Auto-Negotiation */

    UINT waited;
    u16 bmsr = 0U;
    /* IEEE 802.3 clause 28 auto-negotiation commonly takes 1-3s to
     * complete; the per-iteration spin duration here is uncalibrated, so
     * this loop is sized generously rather than to a specific wall-clock
     * target. No scheduler is running yet (this runs from
     * tx_application_define(), before tx_kernel_enter() starts it), so
     * tx_thread_sleep() isn't usable here. */
    for (waited = 0U; waited < 5000U; waited++) {
        XEmacPs_PhyRead(mac, phy_addr, 1U, &bmsr);
        if (bmsr & (1U << 2)) { /* Link Status */
            break;
        }
        for (volatile u32 spin = 0U; spin < 200000U; spin++) {
        }
    }
    xil_printf("gem2: PHY@%lu link renegotiated iter=%u BMSR=0x%x\r\n",
               (unsigned long)phy_addr, (unsigned)waited, bmsr);
}

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
        /* ARP/RARP request and ARP-response transmission use dedicated
         * commands (see nx_arp_packet_receive.c, nx_arp_packet_send.c) —
         * all are just "transmit this already-built packet" to the driver. */
        case NX_LINK_ARP_SEND:
        case NX_LINK_ARP_RESPONSE_SEND:
        case NX_LINK_RARP_SEND:
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
    if (!cfg) {
        xil_printf("gem2: XEmacPs_LookupConfig failed\r\n");
        req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
        return;
    }

    rc = XEmacPs_CfgInitialize(&sCtx.mac, cfg, cfg->BaseAddress);
    if (rc != XST_SUCCESS) {
        xil_printf("gem2: XEmacPs_CfgInitialize failed rc=%ld\r\n", rc);
        req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
        return;
    }

    /* 2. Configure MAC speed, disable all MAC interrupts (polled init) */
    XEmacPs_IntDisable(&sCtx.mac, 0x7FFFFFFFU);

    u32 nwcfg = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCFG_OFFSET);
    nwcfg &= ~XEMACPS_NWCFG_MDCCLKDIV_MASK;
    nwcfg |= (6U << 18U);                              /* MDC ÷48 */
    nwcfg |= XEMACPS_NWCFG_FDEN_MASK;
    XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCFG_OFFSET, nwcfg);

    /* The Orbtrace throughput acceptance gate (400 Mbit/s) is unreachable at
     * 100 Mbps, so this driver runs the link at Gigabit unconditionally
     * rather than negotiating a rate from the PHY over MDIO. Sets both the
     * NWCFG speed/1000 bits and the GEM_CLK_CTRL reference-clock divisors
     * for 1000 Mbps (the divisors for 10/100 Mbps differ and are wrong for
     * a Gigabit link partner). A link partner that cannot do Gigabit will
     * not connect. */
    XEmacPs_SetOperatingSpeed(&sCtx.mac, 1000U);

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
            GEM2_BD_ALIGN, GEM2_TX_BD_COUNT);
    if (rc != XST_SUCCESS) {
        xil_printf("gem2: TX BdRingCreate failed rc=%ld\r\n", rc);
        req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
        return;
    }

    /* 6. RX BD ring */
    rc = XEmacPs_BdRingCreate(
            &XEmacPs_GetRxRing(&sCtx.mac),
            (UINTPTR)sCtx.rx_bd, (UINTPTR)sCtx.rx_bd,
            GEM2_BD_ALIGN, GEM2_BD_COUNT);
    if (rc != XST_SUCCESS) {
        xil_printf("gem2: RX BdRingCreate failed rc=%ld\r\n", rc);
        req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
        return;
    }

    /* 7. Pre-allocate RX packets and point BDs at their data areas */
    for (i = 0U; i < GEM2_BD_COUNT; i++) {
        if (gem2_alloc_rx_packet(i) != NX_SUCCESS) {
            xil_printf("gem2: RX packet alloc failed at slot %lu\r\n", (unsigned long)i);
            req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
            return;
        }
    }
    xil_printf("gem2: initialize OK\r\n");

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
    if (!sCtx.initialized) {
        xil_printf("gem2: enable called before initialize\r\n");
        req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
        return;
    }

    /* Point HW at BD rings (required for ZynqMP GEM) */
    XEmacPs_SetQueuePtr(&sCtx.mac, (UINTPTR)sCtx.tx_q1_dummy, 1U, 1U);
    XEmacPs_SetQueuePtr(&sCtx.mac, (UINTPTR)sCtx.tx_bd, 0U, 1U);
    XEmacPs_SetQueuePtr(&sCtx.mac, (UINTPTR)sCtx.rx_bd, 0U, 0U);

    XEmacPs_Start(&sCtx.mac);

    /* MDIO (management port) is only live once XEmacPs_Start() has set
     * NWCTRL's MPE bit, so the PHY fix must run after Start(), not in
     * gem2_initialize(). */
    gem2_phy_enable_tx_delay(&sCtx.mac);

    /* Enable RX and TX complete interrupts in the MAC */
    XEmacPs_IntEnable(&sCtx.mac,
                      XEMACPS_IXR_FRAMERX_MASK | XEMACPS_IXR_TXCOMPL_MASK);

    req->nx_ip_driver_interface->nx_interface_link_up = NX_TRUE;
    req->nx_ip_driver_status = NX_SUCCESS;
    xil_printf("gem2: enable OK, NWCTRL=0x%lx NWCFG=0x%lx NWSR=0x%lx\r\n",
               (unsigned long)XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET),
               (unsigned long)XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCFG_OFFSET),
               (unsigned long)XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWSR_OFFSET));
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

    if (sCtx.tx_count >= GEM2_TX_BD_COUNT) {
        /* Ring full — drop; caller must handle retransmit */
        nx_packet_transmit_release(pkt);
        req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
        return;
    }

    /* NetX Duo's IP/ARP layers hand the driver only the upper-layer payload
     * (see e.g. nx_arp_packet_send.c, which only prepends the ARP message
     * itself) plus the resolved destination address in
     * nx_ip_driver_physical_address_msw/lsw — building the 14-byte Ethernet
     * header is the driver's job, per NetX's own reference driver
     * (nx_ram_network_driver.c's NX_LINK_PACKET_SEND/ARP_SEND/... case).
     * Without this, the MAC transmitted raw upper-layer bytes with no
     * Ethernet framing at all: confirmed on real hardware via tcpdump, which
     * showed exactly the ARP message bytes with no header, misparsed as a
     * bogus 802.3 frame with a garbage "MAC" address built from ARP payload
     * bytes. This was mistaken for an RGMII timing/skew problem earlier in
     * this bring-up — see ORBTRACE_TEST_REPORT follow-up notes — but a real
     * skew issue would not produce byte-identical "corruption" independent
     * of PHY tap_sel, which is what a tap_sel sweep on real hardware showed
     * once the separate TX-ring-stall bug (GEM2_TX_BD_COUNT) was fixed. */
    u16 ether_type;
    switch (req->nx_ip_driver_command) {
        case NX_LINK_ARP_SEND:
        case NX_LINK_ARP_RESPONSE_SEND:
            ether_type = NX_ETHERNET_ARP;
            break;
        case NX_LINK_RARP_SEND:
            ether_type = NX_ETHERNET_RARP;
            break;
        default:
            ether_type = (pkt->nx_packet_ip_version == 4U) ? NX_ETHERNET_IP : NX_ETHERNET_IPV6;
            break;
    }

    pkt->nx_packet_prepend_ptr -= NX_ETHERNET_SIZE;
    pkt->nx_packet_length += NX_ETHERNET_SIZE;

    u8 *eth = (u8 *)pkt->nx_packet_prepend_ptr;
    ULONG dst_msw = req->nx_ip_driver_physical_address_msw;
    ULONG dst_lsw = req->nx_ip_driver_physical_address_lsw;
    ULONG src_msw = req->nx_ip_driver_interface->nx_interface_physical_address_msw;
    ULONG src_lsw = req->nx_ip_driver_interface->nx_interface_physical_address_lsw;

    eth[0] = (u8)(dst_msw >> 8);  eth[1] = (u8)dst_msw;
    eth[2] = (u8)(dst_lsw >> 24); eth[3] = (u8)(dst_lsw >> 16);
    eth[4] = (u8)(dst_lsw >> 8);  eth[5] = (u8)dst_lsw;
    eth[6] = (u8)(src_msw >> 8);  eth[7] = (u8)src_msw;
    eth[8] = (u8)(src_lsw >> 24); eth[9] = (u8)(src_lsw >> 16);
    eth[10] = (u8)(src_lsw >> 8); eth[11] = (u8)src_lsw;
    eth[12] = (u8)(ether_type >> 8);
    eth[13] = (u8)ether_type;

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
                    ((slot == GEM2_TX_BD_COUNT - 1U) ? XEMACPS_TXBUF_WRAP_MASK : 0U));

    Xil_DCacheFlushRange((UINTPTR)sCtx.tx_buf[slot], total);
    Xil_DCacheFlushRange((UINTPTR)bd, GEM2_BD_ALIGN);

    sCtx.tx_pkts[slot] = pkt;
    sCtx.tx_head = (slot + 1U) % GEM2_TX_BD_COUNT;
    sCtx.tx_count++;
    sCtx.diag_tx_frames++;

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
        sCtx.diag_rx_frames++;

        NX_PACKET *pkt = sCtx.rx_pkts[slot];
        Xil_DCacheInvalidateRange((UINTPTR)pkt->nx_packet_prepend_ptr, frame_len);

        /* Determine EtherType (bytes 12–13 of Ethernet header) */
        u8  *eth  = (u8 *)pkt->nx_packet_prepend_ptr;
        u16  etype = (u16)((eth[12] << 8) | eth[13]);
        sCtx.diag_last_etype = etype;
        sCtx.diag_last_len = frame_len;

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
        sCtx.diag_last_tx_stat = stat;

        if (!(stat & XEMACPS_TXBUF_USED_MASK)) {
            break;  /* DMA still owns this BD */
        }
        sCtx.diag_tx_complete++;

        /* Release the associated NX_PACKET */
        if (sCtx.tx_pkts[slot]) {
            nx_packet_transmit_release(sCtx.tx_pkts[slot]);
            sCtx.tx_pkts[slot] = NX_NULL;
        }

        sCtx.tx_tail = (slot + 1U) % GEM2_TX_BD_COUNT;
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
    sCtx.diag_isr_calls++;

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
