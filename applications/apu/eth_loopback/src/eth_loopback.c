/* SPDX-License-Identifier: MIT
 *
 * GEM2 (XEmacPs) internal MAC loopback test — polled, bare-metal, no RTOS.
 *
 * Sends one 60-byte Ethernet frame through GEM2's local loopback path and
 * verifies every received byte matches.  No PHY or external cable is needed;
 * the loopback is inside the MAC itself (NWCTRL.LOOPEN bit).
 *
 * Expected UART output on success:
 *   GEM2 internal loopback test
 *   frame sent, polling...
 *   RX len=60
 *   PASS: loopback OK
 */

#include <string.h>

#include "xemacps.h"
#include "xemacps_hw.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xstatus.h"

/* ── Configuration ────────────────────────────────────────────────────────── */

/* GEM2 base address from xparameters.h (0xFF0D0000) */
#define EMACPS_BASEADDR  XPAR_XEMACPS_0_BASEADDR

/*
 * BD ring: 1 descriptor per direction, 64-byte (cache-line) stride.
 * XEmacPs_BdRingMemCalc(64, 1) = 64 * (1+1) = 128 bytes needed per ring.
 */
#define BD_ALIGNMENT     64U
#define BD_RING_BYTES    128U

/*
 * Minimum valid Ethernet frame without FCS: 14-byte header + 46-byte payload.
 * MAC inserts FCS on TX (FCS_INSERT_OPTION) and strips it on RX
 * (FCS_STRIP_OPTION), so the received BD length matches FRAME_LEN.
 */
#define FRAME_LEN        60U

/* Worst-case RX buffer: full-MTU frame */
#define RXBUF_LEN        1600U

/* Poll iterations before declaring timeout */
#define POLL_TIMEOUT     2000000U

/* ── Static storage ───────────────────────────────────────────────────────── */

/*
 * BD ring memory.  Cache-line aligned so a single Xil_DCacheFlushRange /
 * Xil_DCacheInvalidateRange call covers the full BD slot without touching
 * adjacent data.
 */
static u8 TxBdMem[BD_RING_BYTES]  __attribute__((aligned(BD_ALIGNMENT)));
static u8 RxBdMem[BD_RING_BYTES]  __attribute__((aligned(BD_ALIGNMENT)));
static u8 TxQ1Dummy[BD_ALIGNMENT] __attribute__((aligned(BD_ALIGNMENT)));

static u8 TxFrame[FRAME_LEN]  __attribute__((aligned(64)));
static u8 RxFrame[RXBUF_LEN]  __attribute__((aligned(64)));

static XEmacPs Emac;

/* ── Small helpers ────────────────────────────────────────────────────────── */

/*
 * Configure NWCFG for 100 Mbps + full-duplex + MDC clock ÷48.
 *
 * In internal-loopback mode the physical signalling rate is irrelevant, but
 * the MAC still needs a valid speed setting.  MDC divisor keeps the MDIO
 * clock inside the 2.5 MHz spec (125 MHz PCLK ÷ 48 ≈ 2.6 MHz).
 */
static void set_100_full_duplex(void)
{
    u32 Reg = XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_NWCFG_OFFSET);
    Reg &= ~XEMACPS_NWCFG_MDCCLKDIV_MASK;
    Reg |= (6U << 18U);                             /* MDC ÷48 */
    Reg |= XEMACPS_NWCFG_100_MASK | XEMACPS_NWCFG_FDEN_MASK;
    XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_NWCFG_OFFSET, Reg);
}

/*
 * Enable GEM2 local (internal) loopback.
 *
 * XEmacPs_Start() does a read-modify-write on NWCTRL to set TXEN/RXEN, so
 * calling this before Start() is safe — the LOOPEN bit is preserved.
 */
static void enable_local_loopback(void)
{
    u32 Reg = XEmacPs_ReadReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET);
    Reg |= XEMACPS_NWCTRL_LOOPEN_MASK;
    XEmacPs_WriteReg(Emac.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET, Reg);
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

int main(void)
{
    LONG Status;
    XEmacPs_Config *Cfg;

    /* Locally-administered MAC address (first octet bit 1 set = LA) */
    u8 MacAddr[6] = { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };

    xil_printf("\r\nGEM2 internal loopback test\r\n");

    /* ── 1. Initialise driver ───────────────────────────────────────────── */
    Cfg = XEmacPs_LookupConfig(EMACPS_BASEADDR);
    if (Cfg == NULL) {
        xil_printf("FAIL: XEmacPs_LookupConfig returned NULL\r\n");
        return XST_FAILURE;
    }

    Status = XEmacPs_CfgInitialize(&Emac, Cfg, Cfg->BaseAddress);
    if (Status != XST_SUCCESS) {
        xil_printf("FAIL: XEmacPs_CfgInitialize %ld\r\n", Status);
        return (int)Status;
    }

    /* Polled mode — mask every MAC interrupt source */
    XEmacPs_IntDisable(&Emac, 0x7FFFFFFFU);

    /* ── 2. MAC / network configuration ────────────────────────────────── */
    XEmacPs_SetMacAddress(&Emac, (void *)MacAddr, 1);
    set_100_full_duplex();

    /*
     * Options set here affect NWCFG only — TX/RX enable comes from
     * XEmacPs_Start() so the BD queue pointers are written to HW first.
     */
    Status = XEmacPs_SetOptions(&Emac,
        XEMACPS_PROMISC_OPTION    |   /* accept every incoming frame */
        XEMACPS_FCS_INSERT_OPTION |   /* MAC appends CRC on transmit */
        XEMACPS_FCS_STRIP_OPTION);    /* MAC strips CRC on receive   */
    if (Status != XST_SUCCESS) {
        xil_printf("FAIL: XEmacPs_SetOptions %ld\r\n", Status);
        return (int)Status;
    }

    /* ── 3. TX BD ring (1 descriptor) ──────────────────────────────────── */
    Status = XEmacPs_BdRingCreate(
        &XEmacPs_GetTxRing(&Emac),
        (UINTPTR)TxBdMem, (UINTPTR)TxBdMem,
        BD_ALIGNMENT, 1U);
    if (Status != XST_SUCCESS) {
        xil_printf("FAIL: TxBdRingCreate %ld\r\n", Status);
        return (int)Status;
    }

    /* ── 4. RX BD ring (1 descriptor) ──────────────────────────────────── */
    Status = XEmacPs_BdRingCreate(
        &XEmacPs_GetRxRing(&Emac),
        (UINTPTR)RxBdMem, (UINTPTR)RxBdMem,
        BD_ALIGNMENT, 1U);
    if (Status != XST_SUCCESS) {
        xil_printf("FAIL: RxBdRingCreate %ld\r\n", Status);
        return (int)Status;
    }

    /* ── 5. Point the RX BD at the receive buffer ───────────────────────── */
    {
        XEmacPs_Bd *RxBdPtr = (XEmacPs_Bd *)RxBdMem;
        /*
         * BdRingCreate set RXBUF_WRAP_MASK (bit 1) in the address word of
         * the last (only) BD.  XEmacPs_BdSetAddressRx preserves that bit and
         * clears bit 0 (ownership = 0 → DMA owns the BD, may write into it).
         */
        XEmacPs_BdSetAddressRx(RxBdPtr, (UINTPTR)RxFrame);
        Xil_DCacheFlushRange((UINTPTR)RxBdPtr, BD_ALIGNMENT);

        /*
         * The BSS-clear at startup dirtied the RxFrame cache lines with zeros.
         * Xil_DCacheFlushRange / Xil_DCacheInvalidateRange = CIVAC (clean AND
         * invalidate), so if we called it AFTER the DMA wrote the received
         * frame to memory those dirty zeros would overwrite the DMA data.
         * Flushing RxFrame HERE, before the DMA starts, cleans the zeros back
         * to memory (no-op: memory already holds zeros) and marks the lines
         * invalid.  The DMA then writes into DRAM; the step-11 CIVAC sees no
         * dirty lines to flush and the CPU reads the fresh DMA data.
         */
        Xil_DCacheFlushRange((UINTPTR)RxFrame, RXBUF_LEN);
    }

    /* ── 6. Enable local loopback, then start the MAC ───────────────────── */
    enable_local_loopback();

    /*
     * On ZynqMP (GEM version > 2) XEmacPs_Start() does NOT write TXQBASE /
     * RXQBASE — that requires explicit XEmacPs_SetQueuePtr() calls.
     * Direction 1 = TX, Direction 0 = RX.  QueueNum 0 = primary, 1 = priority.
     *
     * ZynqMP GEM processes TXQ1 (priority queue) before TXQ0.  The reset
     * leaves TXQ1BASE pointing at a stale address in OCM/exception-vector space
     * (0x380), so the DMA spins forever on queue 1 and never reaches our frame.
     * Point TXQ1 at a dummy BD with USED=1|WRAP=1 so the engine skips it
     * immediately and falls through to queue 0.
     */
    XEmacPs_BdWrite((XEmacPs_Bd *)TxQ1Dummy, XEMACPS_BD_STAT_OFFSET,
        XEMACPS_TXBUF_USED_MASK | XEMACPS_TXBUF_WRAP_MASK);
    Xil_DCacheFlushRange((UINTPTR)TxQ1Dummy, BD_ALIGNMENT);
    XEmacPs_SetQueuePtr(&Emac, (UINTPTR)TxQ1Dummy, 1U, 1U);

    XEmacPs_SetQueuePtr(&Emac, (UINTPTR)TxBdMem, 0U, 1U);
    XEmacPs_SetQueuePtr(&Emac, (UINTPTR)RxBdMem, 0U, 0U);

    XEmacPs_Start(&Emac);

    /* ── 7. Build the test frame ────────────────────────────────────────── */
    memset(TxFrame, 0xFFU, 6U);            /* dst MAC: broadcast              */
    memcpy(TxFrame + 6U, MacAddr, 6U);     /* src MAC: our locally-admin addr */
    TxFrame[12] = 0x08U; TxFrame[13] = 0x00U;  /* EtherType 0x0800           */
    for (u32 i = 14U; i < FRAME_LEN; i++)
        TxFrame[i] = (u8)(i - 14U);        /* payload: 0x00 0x01 0x02 …      */

    Xil_DCacheFlushRange((UINTPTR)TxFrame, FRAME_LEN);

    /* ── 8. Set up TX BD and kick the transmitter ───────────────────────── */
    {
        XEmacPs_Bd *TxBdPtr = (XEmacPs_Bd *)TxBdMem;

        /*
         * Clear the full 64-byte BD slot.  BdRingCreate left USED=1
         * (SW-owned) and WRAP=1; we re-write both below.
         */
        memset(TxBdPtr, 0, BD_ALIGNMENT);

        XEmacPs_BdSetAddressTx(TxBdPtr, (UINTPTR)TxFrame);

        /*
         * Status word:
         *   [13:0]  = frame length
         *   [15]    = LAST  (this is the only buffer for this frame)
         *   [30]    = WRAP  (this is the last BD in the ring)
         *   [31]    = USED  = 0  → DMA owns the BD and will transmit it
         */
        XEmacPs_BdWrite(TxBdPtr, XEMACPS_BD_STAT_OFFSET,
            FRAME_LEN                 |
            XEMACPS_TXBUF_LAST_MASK   |
            XEMACPS_TXBUF_WRAP_MASK);

        Xil_DCacheFlushRange((UINTPTR)TxBdPtr, BD_ALIGNMENT);

        /* Set STARTTX in NWCTRL — DMA begins reading the TX BD ring */
        XEmacPs_Transmit(&Emac);
    }

    xil_printf("frame sent, polling...\r\n");

    /* ── 9. Wait for TX completion ──────────────────────────────────────── */
    /*
     * After the MAC finishes transmitting it sets TXBUF_USED_MASK (bit 31)
     * in the TX BD status word to hand ownership back to software.
     */
    {
        u32 Timeout = POLL_TIMEOUT;
        u32 Stat;
        do {
            Xil_DCacheInvalidateRange((UINTPTR)TxBdMem, BD_ALIGNMENT);
            Stat = XEmacPs_BdRead((XEmacPs_Bd *)TxBdMem, XEMACPS_BD_STAT_OFFSET);
            if (--Timeout == 0U) {
                xil_printf("FAIL: TX timeout (stat=0x%08x)\r\n", (unsigned)Stat);
                return XST_FAILURE;
            }
        } while (!(Stat & XEMACPS_TXBUF_USED_MASK));
    }

    /* ── 10. Wait for RX completion ─────────────────────────────────────── */
    /*
     * After the MAC receives a frame it sets RXBUF_NEW_MASK (bit 0, "used")
     * in the RX BD address word to hand ownership back to software.
     */
    {
        u32 Timeout = POLL_TIMEOUT;
        u32 Addr;
        do {
            Xil_DCacheInvalidateRange((UINTPTR)RxBdMem, BD_ALIGNMENT);
            Addr = XEmacPs_BdRead((XEmacPs_Bd *)RxBdMem, XEMACPS_BD_ADDR_OFFSET);
            if (--Timeout == 0U) {
                xil_printf("FAIL: RX timeout (addr=0x%08x)\r\n", (unsigned)Addr);
                return XST_FAILURE;
            }
        } while (!(Addr & XEMACPS_RXBUF_NEW_MASK));
    }

    /* ── 11. Verify received frame ──────────────────────────────────────── */
    {
        Xil_DCacheInvalidateRange((UINTPTR)RxFrame, FRAME_LEN);

        u32 StatWord = XEmacPs_BdRead((XEmacPs_Bd *)RxBdMem,
                                       XEMACPS_BD_STAT_OFFSET);
        u32 RxLen = StatWord & XEMACPS_RXBUF_LEN_MASK;

        xil_printf("RX len=%u\r\n", (unsigned)RxLen);

        if (RxLen != FRAME_LEN) {
            xil_printf("FAIL: length mismatch (got %u, want %u)\r\n",
                       (unsigned)RxLen, (unsigned)FRAME_LEN);
            return XST_FAILURE;
        }

        if (memcmp(TxFrame, RxFrame, FRAME_LEN) != 0) {
            xil_printf("FAIL: payload mismatch\r\n");
            for (u32 i = 0U; i < FRAME_LEN; i++) {
                if (TxFrame[i] != RxFrame[i])
                    xil_printf("  byte[%u]: tx=0x%02x rx=0x%02x\r\n",
                               (unsigned)i,
                               (unsigned)TxFrame[i],
                               (unsigned)RxFrame[i]);
            }
            return XST_FAILURE;
        }
    }

    xil_printf("PASS: loopback OK\r\n");
    return XST_SUCCESS;
}
