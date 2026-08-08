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

/* XEmacPs DMA requires the BD *ring base* to be 64-byte aligned on aarch64
 * (XEMACPS_DMABD_MINIMUM_ALIGNMENT) — this only constrains the starting
 * address, used for the per-slot buffer/array alignment attribute and as
 * the Alignment argument to XEmacPs_BdRingCreate(). */
#define GEM2_BD_ALIGN    64U

/* The actual byte spacing between consecutive hardware descriptors. Root-
 * caused on real hardware: XEmacPs_BdRingCreate() (xemacps_bdring.c) always
 * sets RingPtr->Separation = sizeof(XEmacPs_Bd) — 16 bytes on aarch64
 * (XEMACPS_BD_NUM_WORDS=4) — regardless of the Alignment parameter passed
 * in, which only validates the ring's *base* address. Every BD accessor in
 * this file previously multiplied the slot index by GEM2_BD_ALIGN (64), 4x
 * the DMA engine's real stride, so only slot 0 ever coincided with a byte
 * offset the hardware actually walked; slots 1-3 (and the WRAP bit meant
 * for the ring's last BD) lived in memory the DMA never touched, and the
 * DMA's internal descriptor pointer instead wrapped back through 4 native
 * 16-byte descriptors packed inside what software thought was just slot 0's
 * padding. Confirmed live via a raw RXQBASE dump during a stall: the
 * register auto-advanced in exact 16-byte steps, never reaching software's
 * 64-byte-spaced "slot 1" address, and RX died permanently after exactly
 * one frame on every reflash regardless of PHY tap_sel. */
#define GEM2_BD_STRIDE   ((u32)sizeof(XEmacPs_Bd))

/* Maximum receive frame size (Ethernet MTU 1500 + 14 header; no FCS) */
#define GEM2_RX_BUFSIZE  1536U

/* Bytes of dead space the DMA engine writes before each received frame
 * (NWCFG.RXOFFS below), so the IP header lands 4-byte aligned after the
 * 14-byte Ethernet header is stripped. Root-caused on real hardware:
 * _nx_ip_checksum_compute() (nx_ip_checksum_compute.c) casts
 * nx_packet_prepend_ptr straight to ULONG* and dereferences it — it
 * requires 4-byte alignment. NX_PACKET pool buffers start 4-aligned, but
 * this driver wrote received frames flush against that alignment and then
 * advanced past a 14-byte (2 mod 4) Ethernet header, leaving the IP header
 * 2 bytes off the boundary NetX's checksum routine assumes. That silently
 * computed the wrong checksum for every single received IP packet — never
 * a wire/PHY corruption issue, confirmed by manually recomputing the exact
 * bytes captured from the DMA buffer in Python and getting a valid
 * checksum every time NetX's own nx_ip_receive_checksum_errors counter
 * incremented. RXOFFS makes the DMA insert this many pad bytes before the
 * frame while keeping the BD's own buffer address itself 4-aligned (the
 * descriptor format's requirement, since bits[1:0] of that word are
 * NEW/WRAP flags). */
#define GEM2_RX_OFFSET   2U

/* Number of TX poll iterations before declaring TX timeout */
#define GEM2_TX_POLL     500000U

/* Number of peers this driver's own ARP cache remembers — see
 * gem2_arp_learn()/gem2_arp_lookup(). Small on purpose: this bring-up
 * firmware talks to a handful of LAN peers at most. */
#define GEM2_ARP_CACHE_SIZE 4U

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
    u32 diag_last_isr;
    u32 diag_rxused_count;
    u32 diag_txused_count;
    u32 diag_last_txsr;
    u32 diag_driver_cmd_count;
    u32 diag_last_driver_cmd;
    u32 diag_last_driver_status;
    u32 diag_tx_recover_attempts;
    u32 diag_tx_recover_txqbase_before;
    u32 diag_tx_recover_txqbase_after;
    u32 diag_last_tx_dst_msw;
    u32 diag_last_tx_dst_lsw;
    u32 diag_last_tx_cmd;
    u32 diag_last_req_addr_lo;
    u32 diag_last_req_addr_hi;
    u32 diag_last_req_sizeof;
    u8  diag_last_req_bytes[48];
    u8  diag_ip_header_dump[40];
    u8  diag_arp_dump[28];
    u32 diag_arp_dump_valid;
    u32 diag_tx_dropped_bad_dst;

    /* Driver-local ARP cache — see gem2_arp_learn()/gem2_arp_lookup(). */
    u32 arp_cache_ip[GEM2_ARP_CACHE_SIZE];
    u32 arp_cache_msw[GEM2_ARP_CACHE_SIZE];
    u32 arp_cache_lsw[GEM2_ARP_CACHE_SIZE];
    u32 arp_cache_count;
} Gem2Ctx;

static Gem2Ctx sCtx;

/* Temporary bring-up diagnostic accessor — see main.c's diag_thread_entry. */
void gem2_diag_get(u32 *rx_frames, u32 *tx_frames, u32 *isr_calls, u32 *last_etype, u32 *last_len,
                    u32 *tx_complete, u32 *last_tx_stat, u32 *tx_head, u32 *tx_tail, u32 *tx_count,
                    u32 *last_isr, u32 *rxused_count, u32 *driver_cmd_count, u32 *last_driver_cmd,
                    u32 *last_driver_status)
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
    *driver_cmd_count = sCtx.diag_driver_cmd_count;
    *last_driver_cmd = sCtx.diag_last_driver_cmd;
    *last_driver_status = sCtx.diag_last_driver_status;
    *last_isr = sCtx.diag_last_isr;
    *rxused_count = sCtx.diag_rxused_count;
}

/* Temporary bring-up diagnostic accessor — see main.c's diag_thread_entry. */
void gem2_diag_get_tx_extra(u32 *txused_count, u32 *last_txsr)
{
    *txused_count = sCtx.diag_txused_count;
    *last_txsr = sCtx.diag_last_txsr;
}

/* Temporary bring-up diagnostic accessor: whether gem2_tx_poll_recover()
 * has actually attempted a recovery, and whether the TXQBASE write itself
 * takes visible effect (before/after read of the same register within the
 * same recovery call) — narrows "recovery never runs" from "recovery runs
 * but the register write doesn't stick / hardware ignores it". */
void gem2_diag_get_tx_recover(u32 *attempts, u32 *txqbase_before, u32 *txqbase_after)
{
    *attempts = sCtx.diag_tx_recover_attempts;
    *txqbase_before = sCtx.diag_tx_recover_txqbase_before;
    *txqbase_after = sCtx.diag_tx_recover_txqbase_after;
}

/* Temporary bring-up diagnostic: the destination MAC (as msw/lsw) and the
 * NX_IP_DRIVER command NetX supplied for the most recent gem2_packet_send()
 * call — to check what NetX itself resolved as the destination hardware
 * address for non-ARP sends (TCP replies), independent of anything on the
 * wire. */
void gem2_diag_get_tx_dst(u32 *dst_msw, u32 *dst_lsw, u32 *cmd)
{
    *dst_msw = sCtx.diag_last_tx_dst_msw;
    *dst_lsw = sCtx.diag_last_tx_dst_lsw;
    *cmd = sCtx.diag_last_tx_cmd;
}

/* Temporary bring-up diagnostic: raw RX BD ring state (all GEM2_BD_COUNT
 * descriptors' ADDR/STAT words), software's rx_tail, and the hardware's own
 * RXQBASE register — to see directly whether software and hardware agree
 * on which descriptor is "next" when RXUSED gets stuck. */
void gem2_diag_get_rx_bd_dump(u32 *rx_tail, u32 *rxqbase, u32 *rx_bd_base, u32 addr_words[4], u32 stat_words[4])
{
    *rx_tail = sCtx.rx_tail;
    *rxqbase = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_RXQBASE_OFFSET);
    *rx_bd_base = (u32)(UINTPTR)sCtx.rx_bd;
    for (u32 i = 0U; i < GEM2_BD_COUNT; i++) {
        XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.rx_bd + i * GEM2_BD_STRIDE);
        Xil_DCacheInvalidateRange((UINTPTR)bd, GEM2_BD_STRIDE);
        addr_words[i] = XEmacPs_BdRead(bd, XEMACPS_BD_ADDR_OFFSET);
        stat_words[i] = XEmacPs_BdRead(bd, XEMACPS_BD_STAT_OFFSET);
    }
}

/* Temporary bring-up diagnostic: raw TX BD ring state — same idea as
 * gem2_diag_get_rx_bd_dump(), for chasing why tx_count stops draining. */
void gem2_diag_get_tx_bd_dump(u32 *tx_head, u32 *tx_tail, u32 *tx_count, u32 *txqbase, u32 *tx_bd_base,
                               u32 addr_words[4], u32 stat_words[4])
{
    *tx_head = sCtx.tx_head;
    *tx_tail = sCtx.tx_tail;
    *tx_count = sCtx.tx_count;
    *txqbase = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_TXQBASE_OFFSET);
    *tx_bd_base = (u32)(UINTPTR)sCtx.tx_bd;
    for (u32 i = 0U; i < GEM2_TX_BD_COUNT; i++) {
        XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.tx_bd + i * GEM2_BD_STRIDE);
        Xil_DCacheInvalidateRange((UINTPTR)bd, GEM2_BD_STRIDE);
        addr_words[i] = XEmacPs_BdRead(bd, XEMACPS_BD_ADDR_OFFSET);
        stat_words[i] = XEmacPs_BdRead(bd, XEMACPS_BD_STAT_OFFSET);
    }
}

/* Temporary bring-up diagnostic: copies the last received IP frame's first
 * 40 bytes (past the Ethernet header) so it can be dumped over UART and
 * compared byte-for-byte against a host-side capture. */
void gem2_diag_get_ip_dump(unsigned char *out40)
{
    memcpy(out40, sCtx.diag_ip_header_dump, sizeof(sCtx.diag_ip_header_dump));
}

/* Temporary bring-up diagnostic: raw bytes of the last received ARP
 * message (28 bytes: hwtype/ptype/hlen/plen/oper/SHA/SPA/THA/TPA), captured
 * by our driver *before* handing the packet to
 * _nx_arp_packet_deferred_receive() — independent of anything NetX does to
 * it afterward. Added to determine whether a garbage destination MAC later
 * observed in NetX's ARP table (see ORBTRACE_TEST_REPORT session 6) is
 * already present in the bytes we deliver (an RX-side bug in this driver)
 * or only appears after NetX processes them (a NetX-side bug — ruled
 * unlikely by reading _nx_arp_packet_receive.c end to end, but not yet
 * proven either way). */
void gem2_diag_get_arp_dump(unsigned char *out28, unsigned int *valid)
{
    memcpy(out28, sCtx.diag_arp_dump, sizeof(sCtx.diag_arp_dump));
    *valid = sCtx.diag_arp_dump_valid;
}

/* Temporary bring-up diagnostic accessor — see the comment where
 * diag_last_req_bytes is captured in gem2_packet_send(). */
void gem2_diag_get_req_dump(u32 *addr_lo, u32 *addr_hi, u32 *sizeof_req, unsigned char *out48)
{
    *addr_lo = sCtx.diag_last_req_addr_lo;
    *addr_hi = sCtx.diag_last_req_addr_hi;
    *sizeof_req = sCtx.diag_last_req_sizeof;
    memcpy(out48, sCtx.diag_last_req_bytes, sizeof(sCtx.diag_last_req_bytes));
}

/* Temporary bring-up diagnostic accessor: count of outbound sends dropped
 * by the dst_msw > 0xFFFF invariant check in gem2_packet_send() — see that
 * check's comment. */
u32 gem2_diag_get_tx_dropped_bad_dst(void)
{
    return sCtx.diag_tx_dropped_bad_dst;
}

/* Temporary bring-up diagnostic accessor: this driver's own ARP cache —
 * see gem2_arp_learn()/gem2_arp_lookup() above gem2_rx_process(). */
void gem2_diag_get_arp_cache(u32 *count, u32 *ip, u32 *msw, u32 *lsw)
{
    *count = sCtx.arp_cache_count;
    for (u32 i = 0U; i < GEM2_ARP_CACHE_SIZE; i++) {
        ip[i] = sCtx.arp_cache_ip[i];
        msw[i] = sCtx.arp_cache_msw[i];
        lsw[i] = sCtx.arp_cache_lsw[i];
    }
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
static void gem2_arp_learn(u32 ip, u32 msw, u32 lsw);
static UINT gem2_arp_lookup(u32 ip, u32 *msw, u32 *lsw);

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
#define KSZ9131_DLL_TAP_SEL_SHIFT    6U
#define KSZ9131_DLL_TAP_SEL_MASK     (0x3FU << KSZ9131_DLL_TAP_SEL_SHIFT)

/* Empirical RGMII skew sweep (bring-up only, see ORBTRACE_TEST_REPORT's
 * "Suggested next step" for follow-up session 3): no oscilloscope/protocol
 * analyzer is available to characterize the link directly, so this narrows
 * on a working tap_sel by testing link reliability at each candidate value
 * on real hardware. 0x1B (27) is the chip's power-on default (§5.3.70) —
 * override via -DGEM2_TX_DLL_TAP_SEL=<0-63> to sweep. RX left at its
 * default; only TX has shown corruption/loss symptoms so far. */
#ifndef GEM2_TX_DLL_TAP_SEL
#define GEM2_TX_DLL_TAP_SEL 0x1BU
#endif
#ifndef GEM2_RX_DLL_TAP_SEL
#define GEM2_RX_DLL_TAP_SEL 0x1BU
#endif

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
    u16 rx_enabled = (u16)(rx_before & ~KSZ9131_BYPASS_RXDLL_BIT & ~KSZ9131_DLL_TAP_SEL_MASK);
    rx_enabled = (u16)(rx_enabled | ((GEM2_RX_DLL_TAP_SEL << KSZ9131_DLL_TAP_SEL_SHIFT) & KSZ9131_DLL_TAP_SEL_MASK));
    gem2_mmd_reset_pulse(mac, phy_addr, KSZ9131_RX_DLL_CTRL_REG, rx_enabled, KSZ9131_RXDLL_RESET_BIT);

    u16 before = gem2_mmd_read(mac, phy_addr, KSZ9131_MMD_DEV_PCS_EXT, KSZ9131_TX_DLL_CTRL_REG);
    u16 enabled = (u16)(before & ~KSZ9131_BYPASS_TXDLL_BIT & ~KSZ9131_DLL_TAP_SEL_MASK);
    enabled = (u16)(enabled | ((GEM2_TX_DLL_TAP_SEL << KSZ9131_DLL_TAP_SEL_SHIFT) & KSZ9131_DLL_TAP_SEL_MASK));
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
    sCtx.diag_driver_cmd_count++;
    sCtx.diag_last_driver_cmd = (u32)req->nx_ip_driver_command;

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
    sCtx.diag_last_driver_status = (u32)req->nx_ip_driver_status;
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
    /* See GEM2_RX_OFFSET: pad every received frame by that many bytes so
     * the IP header lands 4-byte aligned for NetX's checksum routine. */
    nwcfg &= ~XEMACPS_NWCFG_RXOFFS_MASK;
    nwcfg |= (GEM2_RX_OFFSET << 14U);
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

    /* Standard Ethernet IP MTU. Left unset, this field defaults to 0 from
     * NetX's zero-initialized interface struct — ARP still works (it
     * bypasses IP-layer fragmentation/MTU checks entirely), but every
     * outbound IP packet (TCP SYN-ACK, ICMP, ...) silently fails NetX's
     * own MTU check before ever reaching gem2_packet_send(), which looks
     * identical on the wire to the driver just not replying. */
    req->nx_ip_driver_interface->nx_interface_ip_mtu_size = 1500U;

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

    /* 5b. Arm every TX BD to a safe "already consumed" idle state before the
     * DMA is ever started. XEmacPs_BdRingCreate() only memset()s the ring to
     * all-zero (see xemacps_bdring.c) — it does not set USED on any
     * descriptor, and WRAP (a bit inside the TX STAT word, unlike RX where
     * it lives in the ADDR word) is never set on the last slot either. RX
     * doesn't have this problem because gem2_alloc_rx_packet() explicitly
     * arms every RX slot (including WRAP on the last one) in the loop below
     * before Start(); TX had no equivalent step. Root-caused on real
     * hardware: the first send (always landing in slot 0) worked because
     * XEmacPs_Start() points TXQBASE directly at slot 0 with real data. But
     * once that frame completed, the DMA's internal descriptor pointer
     * advanced into slots 1-3, which were still all-zero (USED=0, LAST=0,
     * WRAP=0) — not a valid "nothing to do, stop cleanly" marker — so it
     * wandered off following bogus zero-length "buffers" instead of
     * recognizing an idle ring. tx_count then grew as gem2_packet_send()
     * queued real frames into those slots, but the DMA's pointer had already
     * desynced from software's view and never came back, exactly mirroring
     * the RXUSED desync this driver already had to work around for RX (see
     * gem2_irq_handler()'s XEMACPS_IXR_RXUSED_MASK branch) — except TX had
     * no equivalent recovery and, worse, was never armed correctly to begin
     * with. Fixed the idiomatic way: XEmacPs_BdRingClone() with a
     * USED-marked template, exactly as Xilinx's own reference driver does
     * (xemacps_example_intr_dma.c's TxBD setup) — it clones the template
     * across every slot and automatically sets WRAP on the last one. */
    {
        XEmacPs_Bd bd_template;
        XEmacPs_BdClear(&bd_template);
        XEmacPs_BdSetStatus(&bd_template, XEMACPS_TXBUF_USED_MASK);
        rc = XEmacPs_BdRingClone(&XEmacPs_GetTxRing(&sCtx.mac), &bd_template, XEMACPS_SEND);
        if (rc != XST_SUCCESS) {
            xil_printf("gem2: TX BdRingClone failed rc=%ld\r\n", rc);
            req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
            return;
        }
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

    /* Enable RX, TX-complete, RX-buffer-unavailable, and TX-buffer-used
     * interrupts in the MAC. RXUSED/TXUSED must be enabled here, not just
     * handled in the ISR: the DMA engine halts scanning at the hardware
     * level the moment it finds a descriptor still marked used, regardless
     * of whether that condition is masked into an interrupt — masking it
     * just means the CPU is never told the DMA stopped, so
     * gem2_irq_handler() never runs again and RX/TX goes silently and
     * permanently dead. TXUSED (TXSR bit 0, "Tx buffer used bit read") is
     * the TX-side mirror of RXUSED: confirmed on real hardware that TX
     * completes exactly once after link-up and then stalls forever with
     * tx_count growing unboundedly — the same DMA-halts-on-used-bit
     * behavior seen on RX, just never wired up on the TX side. */
    XEmacPs_IntEnable(&sCtx.mac,
                      XEMACPS_IXR_FRAMERX_MASK | XEMACPS_IXR_TXCOMPL_MASK |
                      XEMACPS_IXR_RXUSED_MASK | XEMACPS_IXR_TXUSED_MASK);

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

    sCtx.diag_last_tx_dst_msw = (u32)dst_msw;
    sCtx.diag_last_tx_dst_lsw = (u32)dst_lsw;
    sCtx.diag_last_tx_cmd = (u32)req->nx_ip_driver_command;
    sCtx.diag_last_req_addr_lo = (u32)((UINTPTR)req & 0xFFFFFFFFU);
    sCtx.diag_last_req_addr_hi = (u32)(((u64)(UINTPTR)req) >> 32);
    sCtx.diag_last_req_sizeof = (u32)sizeof(*req);
    memcpy(sCtx.diag_last_req_bytes, (const void *)req, sizeof(sCtx.diag_last_req_bytes));

    /* Root-caused on real hardware (see ORBTRACE_TEST_REPORT session 6):
     * under TCP SYN-retry load, req->nx_ip_driver_physical_address_msw/lsw
     * occasionally arrives holding garbage that decodes as raw bytes from
     * the *received* packet's own TCP header (src_port:dst_port as msw,
     * sequence number as lsw) rather than a resolved MAC. Traced this to
     * real hardware ground truth: a live JTAG memory read of NetX's own ARP
     * table entry for the destination showed it was genuinely correct
     * (checked shortly after), and disassembling the compiled
     * _nx_ip_driver_packet_send() confirmed the ARP-resolved branch copies
     * arp_ptr's msw/lsw fields into this struct correctly (a single 8-byte
     * ldur/str, byte-for-byte matching the struct layout) — so the bug is
     * not in this driver's read, nor in that copy. The remaining plausible
     * explanation is a genuine race in NetX's own vendored
     * _nx_arp_packet_receive.c: the "create new ARP entry" branch writes
     * nx_arp_ip_address, then nx_arp_physical_address_msw, then _lsw as
     * three separate, TX_DISABLE-unprotected statements — unlike the
     * sibling "update existing entry" branch a few lines above it, which
     * *does* wrap the same kind of update in TX_DISABLE/TX_RESTORE. A
     * concurrent reader can observe the IP address already matching (hash
     * lookup succeeds) while the MAC fields still hold stale pool memory
     * from that slot's previous use. This is upstream vendored-library
     * behavior, not something to patch directly here.
     *
     * A first attempt at defending against this checked "msw <= 0xFFFF"
     * (every legitimate source of this field — ARP-resolved unicast, IPv4
     * broadcast, class D multicast, see nx_ip_driver_packet_send.c's three
     * driver-invoking branches — only ever holds the top 16 bits of a
     * 48-bit MAC, or the 0x0100 IANA multicast prefix, or 0xFFFF
     * broadcast). That caught most garbage on real hardware but not all of
     * it — confirmed on the wire: one instance (msw=0xB974) coincidentally
     * fell inside the valid 16-bit range and still went out with a wrong
     * destination MAC. See gem2_arp_learn()/gem2_arp_lookup() above:
     * this driver keeps its own tiny IP-to-MAC cache, fed directly from
     * ARP frames as *this driver* parses them (the same extraction
     * independently verified correct via the wire-level ARP reply),
     * entirely bypassing NetX's internal ARP table and its race. Prefer
     * that cache for IP sends; only fall back to req's (possibly racy)
     * fields — still gated by the msw<=0xFFFF sanity check as a last
     * resort — when this driver has never observed the destination on the
     * wire yet (e.g. sending through a gateway this driver hasn't ARPed
     * for directly). ARP/RARP sends themselves are unaffected — those
     * commands supply the destination address directly (broadcast, or the
     * synchronously-extracted sender address for a reply), never through
     * this same ARP-table lookup, so they were never at risk. */
    if (ether_type == NX_ETHERNET_IP) {
        u32 dest_ip = ((u32)eth[NX_ETHERNET_SIZE + 16] << 24) | ((u32)eth[NX_ETHERNET_SIZE + 17] << 16) |
                      ((u32)eth[NX_ETHERNET_SIZE + 18] << 8) | (u32)eth[NX_ETHERNET_SIZE + 19];
        u32 cached_msw, cached_lsw;
        if (gem2_arp_lookup(dest_ip, &cached_msw, &cached_lsw) == NX_SUCCESS) {
            dst_msw = cached_msw;
            dst_lsw = cached_lsw;
        } else if (dst_msw > 0xFFFFUL) {
            /* No cache entry and req's value fails the sanity check —
             * drop and let TCP's own retransmission timer retry later,
             * exactly how the ring-full case above handles a transient
             * send failure. */
            sCtx.diag_tx_dropped_bad_dst++;
            nx_packet_transmit_release(pkt);
            req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
            return;
        }
    }

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
    XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.tx_bd + slot * GEM2_BD_STRIDE);
    memset(bd, 0, GEM2_BD_STRIDE);
    XEmacPs_BdSetAddressTx(bd, (UINTPTR)(sCtx.tx_buf[slot]));
    XEmacPs_BdWrite(bd, XEMACPS_BD_STAT_OFFSET,
                    (total & 0x3FFFU)       |
                    XEMACPS_TXBUF_LAST_MASK |
                    /* set WRAP on last BD in ring */
                    ((slot == GEM2_TX_BD_COUNT - 1U) ? XEMACPS_TXBUF_WRAP_MASK : 0U));

    Xil_DCacheFlushRange((UINTPTR)sCtx.tx_buf[slot], total);
    Xil_DCacheFlushRange((UINTPTR)bd, GEM2_BD_STRIDE);

    sCtx.tx_pkts[slot] = pkt;
    sCtx.tx_head = (slot + 1U) % GEM2_TX_BD_COUNT;
    sCtx.tx_count++;
    sCtx.diag_tx_frames++;

    XEmacPs_Transmit(&sCtx.mac);  /* set STARTTX in NWCTRL */

    req->nx_ip_driver_status = NX_SUCCESS;
}

/* ── Driver-local ARP cache ───────────────────────────────────────────────
 *
 * Root-caused on real hardware (see ORBTRACE_TEST_REPORT session 6): NetX's
 * own vendored _nx_arp_packet_receive.c populates a brand-new ARP table
 * entry via three separate, TX_DISABLE-unprotected statements (IP address,
 * then physical_address_msw, then _lsw) — unlike the sibling "update an
 * existing entry" branch a few lines above it, which does wrap the
 * equivalent update in TX_DISABLE/TX_RESTORE. A concurrent reader (this
 * driver's own TCP send, moments after the ARP request that resolves the
 * peer, is a fast, realistic case) can observe the IP address already
 * matching while the MAC fields still hold stale pool memory. Confirmed
 * multiple ways on real hardware this session: a live JTAG memory read of
 * the ARP table entry itself was correct (checked well after the fact), a
 * disassembly of the compiled _nx_ip_driver_packet_send() confirmed the
 * ARP-resolved branch copies arp_ptr's fields correctly, and yet
 * req->nx_ip_driver_physical_address_msw/lsw was repeatedly observed
 * holding garbage at actual send time — including values that pass a
 * naive "msw <= 0xFFFF" sanity filter (a real MAC's msw is always in that
 * range) purely by chance, so that filter alone isn't sufficient.
 *
 * This is upstream vendored-library behavior, not something to patch
 * directly. Instead, this driver keeps its own tiny, independent
 * IP-to-MAC cache, fed directly from ARP frames as *this driver* parses
 * them on the way to NetX (the same extraction already independently
 * verified correct via gem2_diag_get_arp_dump() / the wire-level ARP
 * reply) — entirely bypassing NetX's internal ARP table and its race.
 * gem2_packet_send() consults this cache for IP sends and only falls back
 * to req's (possibly racy) fields if this driver has never seen the
 * destination on the wire yet. */

static void gem2_arp_learn(u32 ip, u32 msw, u32 lsw)
{
    for (u32 i = 0U; i < sCtx.arp_cache_count; i++) {
        if (sCtx.arp_cache_ip[i] == ip) {
            sCtx.arp_cache_msw[i] = msw;
            sCtx.arp_cache_lsw[i] = lsw;
            return;
        }
    }
    u32 slot = (sCtx.arp_cache_count < GEM2_ARP_CACHE_SIZE)
                   ? sCtx.arp_cache_count++
                   : 0U;  /* cache full — evict the oldest (slot 0) */
    sCtx.arp_cache_ip[slot] = ip;
    sCtx.arp_cache_msw[slot] = msw;
    sCtx.arp_cache_lsw[slot] = lsw;
}

static UINT gem2_arp_lookup(u32 ip, u32 *msw, u32 *lsw)
{
    for (u32 i = 0U; i < sCtx.arp_cache_count; i++) {
        if (sCtx.arp_cache_ip[i] == ip) {
            *msw = sCtx.arp_cache_msw[i];
            *lsw = sCtx.arp_cache_lsw[i];
            return NX_SUCCESS;
        }
    }
    return NX_NOT_SUCCESSFUL;
}

/* ── RX processing (called from ISR) ──────────────────────────────────── */

static void gem2_rx_process(void)
{
    for (u32 i = 0U; i < GEM2_BD_COUNT; i++) {
        u32 slot = sCtx.rx_tail;
        XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.rx_bd + slot * GEM2_BD_STRIDE);

        Xil_DCacheInvalidateRange((UINTPTR)bd, GEM2_BD_STRIDE);
        u32 addr_word = XEmacPs_BdRead(bd, XEMACPS_BD_ADDR_OFFSET);

        if (!(addr_word & XEMACPS_RXBUF_NEW_MASK)) {
            break;  /* DMA still owns this BD */
        }

        /* sCtx.rx_pkts[slot] == NULL marks a slot whose frame was already
         * delivered to NetX on a previous call, but whose refill failed
         * (pool exhausted — see below) and has not yet been retried
         * successfully. The BD's NEW bit stays set by hardware in exactly
         * this same state as "genuine unprocessed frame waiting," so NEW
         * alone cannot tell the two apart; only re-attempt the refill here,
         * do not re-extract/re-deliver — the frame data in this slot's
         * buffer was already consumed and pkt already handed to NetX. */
        if (sCtx.rx_pkts[slot] == NX_NULL) {
            if (gem2_alloc_rx_packet(slot) != NX_SUCCESS) {
                break;  /* still no packet available — retry this same slot next time */
            }
            sCtx.rx_tail = (slot + 1U) % GEM2_BD_COUNT;
            continue;
        }

        u32 stat_word = XEmacPs_BdRead(bd, XEMACPS_BD_STAT_OFFSET);
        u32 frame_len = stat_word & XEMACPS_RXBUF_LEN_MASK;
        sCtx.diag_rx_frames++;

        NX_PACKET *pkt = sCtx.rx_pkts[slot];
        Xil_DCacheInvalidateRange((UINTPTR)pkt->nx_packet_prepend_ptr, GEM2_RX_OFFSET + frame_len);

        /* The DMA engine wrote GEM2_RX_OFFSET pad bytes before the actual
         * frame (NWCFG.RXOFFS) — see GEM2_RX_OFFSET. */
        u8  *eth  = (u8 *)pkt->nx_packet_prepend_ptr + GEM2_RX_OFFSET;
        u16  etype = (u16)((eth[12] << 8) | eth[13]);
        sCtx.diag_last_etype = etype;
        sCtx.diag_last_len = frame_len;

        if (etype == NX_ETHERNET_IP) {
            u32 dump_len = (frame_len - NX_ETHERNET_SIZE < sizeof(sCtx.diag_ip_header_dump))
                           ? (frame_len - NX_ETHERNET_SIZE) : sizeof(sCtx.diag_ip_header_dump);
            memcpy(sCtx.diag_ip_header_dump, eth + NX_ETHERNET_SIZE, dump_len);
        }

        /* Adjust pointers past the pad and the Ethernet header — the result
         * (GEM2_RX_OFFSET + NX_ETHERNET_SIZE = 16, a multiple of 4) lands
         * exactly on the IP header, 4-byte aligned as NetX's checksum
         * routine requires. */
        pkt->nx_packet_prepend_ptr += GEM2_RX_OFFSET + NX_ETHERNET_SIZE;
        pkt->nx_packet_length       = frame_len - NX_ETHERNET_SIZE;
        pkt->nx_packet_append_ptr   = pkt->nx_packet_prepend_ptr + pkt->nx_packet_length;

        if (etype == NX_ETHERNET_ARP && pkt->nx_packet_length >= sizeof(sCtx.diag_arp_dump)) {
            memcpy(sCtx.diag_arp_dump, (u8 *)pkt->nx_packet_prepend_ptr, sizeof(sCtx.diag_arp_dump));
            sCtx.diag_arp_dump_valid = 1U;

            /* Feed this driver's own ARP cache — see gem2_arp_learn()'s
             * comment (above gem2_rx_process()) for why this bypasses a
             * real race in NetX's own ARP table. Standard ARP layout: SHA
             * (sender hardware address) at bytes 8-13, SPA (sender
             * protocol address) at bytes 14-17 — identical offsets for
             * both REQUEST and RESPONSE messages, so this learns from
             * either. */
            u32 sha_msw = ((u32)sCtx.diag_arp_dump[8] << 8) | (u32)sCtx.diag_arp_dump[9];
            u32 sha_lsw = ((u32)sCtx.diag_arp_dump[10] << 24) | ((u32)sCtx.diag_arp_dump[11] << 16) |
                          ((u32)sCtx.diag_arp_dump[12] << 8) | (u32)sCtx.diag_arp_dump[13];
            u32 spa = ((u32)sCtx.diag_arp_dump[14] << 24) | ((u32)sCtx.diag_arp_dump[15] << 16) |
                      ((u32)sCtx.diag_arp_dump[16] << 8) | (u32)sCtx.diag_arp_dump[17];
            if (spa != 0U) {
                gem2_arp_learn(spa, sha_msw, sha_lsw);
            }
        }

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

        /* Mark this slot's packet consumed *before* attempting refill —
         * root-caused on real hardware: gem2_alloc_rx_packet() can fail
         * under pool pressure (observed live: pool_available dropping from
         * 14 to 6 during a TCP SYN-retry burst), and its previous caller
         * silently ignored that failure while still unconditionally
         * advancing rx_tail. That left this exact slot's BD with NEW still
         * set (hardware's "unprocessed frame" marker, never cleared) and
         * sCtx.rx_pkts[slot] still pointing at the packet *already handed
         * to NetX* above. The next time software's rx_tail rotation came
         * back around to this slot, the NEW-bit check alone couldn't
         * distinguish "stale, already-delivered" from "genuinely new" —
         * so the exact same NX_PACKET got extracted and handed to NetX's
         * deferred-receive queue a *second* time, corrupting whatever
         * internal linked-list bookkeeping (nx_packet_queue_next) NetX was
         * using it for. That is a highly plausible root cause for
         * seemingly-unrelated NetX-internal corruption observed under load
         * this session (a garbage ARP table entry with bit patterns
         * provably impossible from the correct extraction path) —
         * confirmed only circumstantially (pool pressure was present when
         * it appeared), not by direct instrumentation of the double-
         * delivery itself, so treat as the leading hypothesis fixed here
         * rather than a fully proven root cause. Setting this to NULL
         * immediately makes "already consumed, awaiting refill" an
         * explicit, checkable state (see the NULL check above) instead of
         * an accident of an unchecked return value. */
        sCtx.rx_pkts[slot] = NX_NULL;

        /* Refill the slot and reset BD ownership to DMA */
        if (gem2_alloc_rx_packet(slot) != NX_SUCCESS) {
            break;  /* no packet available — retry this same slot next time, don't advance */
        }

        /* Advance tail */
        sCtx.rx_tail = (slot + 1U) % GEM2_BD_COUNT;
    }
}

/* ── TX completion cleanup (called from ISR) ───────────────────────────── */

static void gem2_tx_cleanup(void)
{
    while (sCtx.tx_count > 0U) {
        u32 slot = sCtx.tx_tail;
        XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.tx_bd + slot * GEM2_BD_STRIDE);

        Xil_DCacheInvalidateRange((UINTPTR)bd, GEM2_BD_STRIDE);
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

    XEmacPs_Bd *bd = (XEmacPs_Bd *)(sCtx.rx_bd + slot * GEM2_BD_STRIDE);

    /* Clear stat word; set BD address with DMA-owns (bit 0 = 0) */
    XEmacPs_BdWrite(bd, XEMACPS_BD_STAT_OFFSET, 0U);
    XEmacPs_BdSetAddressRx(bd, (UINTPTR)pkt->nx_packet_prepend_ptr);

    /* Preserve WRAP bit on the last BD so the ring is circular */
    if (slot == GEM2_BD_COUNT - 1U) {
        u32 addr = XEmacPs_BdRead(bd, XEMACPS_BD_ADDR_OFFSET);
        XEmacPs_BdWrite(bd, XEMACPS_BD_ADDR_OFFSET, addr | XEMACPS_RXBUF_WRAP_MASK);
    }

    /* XEmacPs_BdSetAddressRx() only touches the address bits (see
     * XEMACPS_RXBUF_ADD_MASK) and deliberately preserves NEW/WRAP, so the
     * "new" bit hardware set when it wrote the frame we just consumed is
     * still 1 here. Without explicitly clearing it, this slot stays
     * permanently "owned by software" from the DMA engine's point of view
     * and never gets reused — once every slot in the ring has been used
     * once, RX silently stalls forever. */
    XEmacPs_BdClearRxNew(bd);

    Xil_DCacheFlushRange((UINTPTR)bd, GEM2_BD_STRIDE);
    Xil_DCacheFlushRange((UINTPTR)pkt->nx_packet_prepend_ptr, GEM2_RX_BUFSIZE);

    return NX_SUCCESS;
}

/* ── TX stall recovery (polled from main.c's diag thread) ──────────────── */

/* Resync TXQBASE to software's tx_tail and re-kick, exactly mirroring the
 * RXUSED recovery in gem2_irq_handler() below. */
static void gem2_tx_stall_recover(void)
{
    sCtx.diag_tx_recover_attempts++;
    sCtx.diag_tx_recover_txqbase_before =
        XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_TXQBASE_OFFSET);
    u32 ctrl = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET);
    XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET,
                      ctrl & ~XEMACPS_NWCTRL_TXEN_MASK);
    UINTPTR next_bd = (UINTPTR)(sCtx.tx_bd + sCtx.tx_tail * GEM2_BD_STRIDE);
    XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_TXQBASE_OFFSET,
                      (u32)(next_bd & 0xFFFFFFFFU));
#if defined(__aarch64__)
    XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_MSBBUF_TXQBASE_OFFSET,
                      (u32)((u64)next_bd >> 32));
#endif
    XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET,
                      ctrl | XEMACPS_NWCTRL_TXEN_MASK);
    XEmacPs_Transmit(&sCtx.mac);  /* re-kick STARTTX now TXQBASE is resynced */
    sCtx.diag_tx_recover_txqbase_after =
        XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_TXQBASE_OFFSET);
}

/* Called once a second from main.c's diag thread (a non-ISR context).
 *
 * Root-caused on real hardware across two failed attempts before this one:
 * the DMA halts TXQ0 scanning the moment it walks into an already-USED
 * descriptor (same failure class as RXUSED — see that branch below) and
 * does *not* reliably raise a further interrupt to report the ongoing
 * stall: confirmed via isr_calls/txused_count both going flat while
 * tx_count sat stuck at GEM2_TX_BD_COUNT for the rest of a session. So
 * this cannot be driven from the ISR at all — there is nothing to trigger
 * on. Polling is the only signal left.
 *
 * The first version of this recovery *was* ISR-triggered (on
 * XEMACPS_IXR_TXUSED_MASK) with a one-shot "already kicked" latch that
 * gem2_packet_send() cleared on every new frame queued. That was actively
 * wrong, not just ineffective: TXUSED fires on essentially every real send
 * too (the ZynqMP TXQ1-priority-queue scan quirk documented in the
 * XEMACPS_IXR_TXUSED_MASK branch below), so clearing the latch right
 * before each send's own benign TXUSED meant this recovery — which toggles
 * TXEN off — ran on almost every transmit, quite possibly aborting a
 * legitimate in-flight DMA transfer for the frame that had just triggered
 * it. Confirmed on real hardware to still fail identically to the
 * no-recovery baseline (TXQBASE never moved from tx_bd_base across a full
 * stall) — consistent with the recovery firing but being immediately
 * undone or corrupted by racing real hardware activity, not with it simply
 * not running.
 *
 * This version instead tracks whether *any* TX completion has happened
 * between one poll tick and the next (via diag_tx_complete, a
 * monotonically-increasing counter): if frames are queued (tx_count > 0)
 * and zero completions occurred in the last full second, the ring is
 * declared stalled and recovered. This is decoupled from send/interrupt
 * timing entirely, so it can't race a legitimate in-flight transfer the
 * way the ISR-triggered version did — a real transmission completes in
 * well under a second. Retries every second for as long as the ring stays
 * stuck, rather than a permanent one-shot latch that a failed first
 * attempt could never recover from. */
void gem2_tx_poll_recover(void)
{
    static u32 last_tx_complete;
    static u32 last_seen_progress = 1U;  /* assume healthy until proven otherwise */

    if (sCtx.tx_count == 0U) {
        last_tx_complete = sCtx.diag_tx_complete;
        last_seen_progress = 1U;
        return;
    }

    if (sCtx.diag_tx_complete != last_tx_complete) {
        last_tx_complete = sCtx.diag_tx_complete;
        last_seen_progress = 1U;
        return;
    }

    if (!last_seen_progress) {
        gem2_tx_stall_recover();
    }
    last_seen_progress = 0U;
}

/* ── GEM2 IRQ handler (overrides weak no-op in board/a53/timer.c) ──────── */

void gem2_irq_handler(void)
{
    if (!sCtx.initialized) { return; }
    sCtx.diag_isr_calls++;

    u32 isr = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_ISR_OFFSET);
    sCtx.diag_last_isr = isr;

    /* Clear interrupts by writing 1s to the ISR (write-to-clear) */
    XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_ISR_OFFSET, isr);

    if (isr & (XEMACPS_IXR_FRAMERX_MASK | XEMACPS_IXR_RXUSED_MASK)) {
        gem2_rx_process();
    }
    if (isr & XEMACPS_IXR_RXUSED_MASK) {
        sCtx.diag_rxused_count++;
        /* RXUSED ("Rx buffer used bit read") means the DMA engine's own
         * internal descriptor pointer walked into a BD still marked used
         * and halted receive scanning entirely. That pointer is private to
         * the hardware — it is not the same thing as our rx_tail — so once
         * it has desynced from software's view of the ring, merely freeing
         * a descriptor (above) or toggling RXEN (which just re-reads
         * whatever the hardware pointer already was) does not bring it
         * back in sync, and RXUSED keeps re-firing forever. Re-pointing the
         * RX queue base at software's current rx_tail slot forces the DMA
         * engine to resume from a position we know is actually free.
         *
         * XEmacPs_SetQueuePtr() itself cannot be used here: it silently
         * no-ops ("if already started, then there is nothing to do") once
         * XEmacPs_Start() has run, which it always has by the time RXUSED
         * can fire. Every past invocation of this recovery path (since it
         * was added) has therefore done nothing at all — confirmed on real
         * hardware via a raw RXQBASE dump during a stall, which never moved
         * off its post-Start() value. Write the same two registers that
         * function would have written, directly, bypassing the guard. */
        u32 ctrl = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET);
        XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET,
                          ctrl & ~XEMACPS_NWCTRL_RXEN_MASK);
        UINTPTR next_bd = (UINTPTR)(sCtx.rx_bd + sCtx.rx_tail * GEM2_BD_STRIDE);
        XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_RXQBASE_OFFSET,
                          (u32)(next_bd & 0xFFFFFFFFU));
#if defined(__aarch64__)
        XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_MSBBUF_RXQBASE_OFFSET,
                          (u32)((u64)next_bd >> 32));
#endif
        XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET,
                          ctrl | XEMACPS_NWCTRL_RXEN_MASK);
    }
    if (isr & XEMACPS_IXR_TXCOMPL_MASK) {
        gem2_tx_cleanup();
    }
    if (isr & XEMACPS_IXR_TXUSED_MASK) {
        sCtx.diag_txused_count++;
        /* TXUSED ("Tx buffer used bit read") fires on real hardware every
         * time the DMA engine's descriptor scan reads the permanently
         * USED|WRAP "dummy" BD parked at TXQ1 (see the ZynqMP TXQ1
         * workaround at the top of this file) — ZynqMP GEM checks TXQ1
         * before TXQ0 on *every* transmit cycle, not just once at reset, so
         * most firings are expected/benign, not evidence of a genuine
         * stall, and it also fires (at most once) when TXQ0 itself
         * genuinely halts on a used descriptor — see gem2_tx_poll_recover()
         * for that failure mode and its recovery.
         *
         * Deliberately just acknowledge TXSR here; do not attempt recovery
         * from this interrupt. Two earlier versions of this handler tried
         * that and both failed on real hardware for related reasons: (1)
         * reprogramming TXQBASE and re-kicking STARTTX in response to
         * *every* TXUSED re-triggers the same TXQ1 dummy scan and spins
         * into an interrupt storm (isr_calls/txused_count climbing into
         * the millions per second); (2) gating that recovery with a
         * latch cleared on every newly-queued frame does avoid the storm,
         * but since TXUSED also fires as a benign side effect of *every*
         * real send, the latch gets cleared right before that send's own
         * benign TXUSED fires again — so the recovery (which toggles TXEN
         * off) ends up running on almost every transmit, quite possibly
         * aborting a legitimate in-flight DMA transfer for the frame that
         * just triggered it. Confirmed on real hardware: TXQBASE never
         * moved off tx_bd_base across a full stall with that version
         * either, consistent with the recovery firing but being undone or
         * corrupted by racing real hardware activity. gem2_tx_poll_recover()
         * (called once a second from main.c's diag thread, a non-ISR
         * context decoupled from send/interrupt timing) is the only
         * recovery path now — see its comment for the full history. */
        u32 txsr = XEmacPs_ReadReg(sCtx.mac.Config.BaseAddress, XEMACPS_TXSR_OFFSET);
        sCtx.diag_last_txsr = txsr;
        XEmacPs_WriteReg(sCtx.mac.Config.BaseAddress, XEMACPS_TXSR_OFFSET, txsr);
    }
}
