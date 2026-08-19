#include "itm.h"

/*
 * Deterministic ITM stimulus, hand-ported from the Rust reference model in
 * //applications/orbtrace/firmware/m3 (kept as a separate host-testable
 * crate; no cross-language golden-vector test ties the two together, unlike
 * the wire-format vectors in firmware/common). Emits genuine CoreSight
 * ITM/TPIU packets via sdk/bsp/m3/itm.h, replacing the synthetic
 * orbtrace_test_source pattern previously wired to source_select==0.
 */

static uint32_t state = 7;
static uint32_t sequence = 0;

/* Latched here (not traced out — the STIM path is exactly what's under
 * test and may block forever) so a JTAG halt-and-read can recover which
 * parallel port widths this synthesized TPIU instance actually supports,
 * independent of what TPIU_CSPSR was told to select. See
 * M3_TRACE_VERIFICATION_PLAN.md's Phase E "Not yet determined" section. */
volatile uint32_t g_tpiu_sspsr_at_boot;

/* Same rationale as g_tpiu_sspsr_at_boot, extended 2026-08-17: SSPSR now
 * confirms this instance genuinely supports the 4-bit port
 * (M3_TRACE_VERIFICATION_PLAN.md), so the STIM-FIFO-never-ready stall
 * isn't a port-width problem. These latch more of the ITM/TPIU state
 * right after m3_itm_init() -- before the first blocking STIM write --
 * to see whether the enable sequence actually took effect (TCR readback)
 * and whether the formatter reports anything unusual even this early
 * (FFSR), all without needing the M3's own JTAG-DAP (Phase G, not wired
 * up yet) since PPB registers aren't reachable via the A53's AXI window. */
volatile uint32_t g_itm_tcr_at_boot;
volatile uint32_t g_itm_stim0_at_boot;
volatile uint32_t g_tpiu_ffsr_at_boot;
volatile uint32_t g_tpiu_ffcr_at_boot;
/* 2026-08-18: a real ILA capture on the PL side shows trace_data_m3 frozen
 * at a constant idle value for the entire capture regardless of real STIM
 * traffic (see M3_TRACE_VERIFICATION_PLAN.md). ITCTRL != 0 (integration
 * test mode) would produce exactly this symptom -- normal trace output
 * disabled, pins driven from the integration data registers instead. */
volatile uint32_t g_tpiu_itctrl_at_boot;
/* Neither of these was ever independently read back before -- SSPSR only
 * proves the silicon supports the widths, not that these two specific
 * writes actually stuck. */
volatile uint32_t g_tpiu_sppr_at_boot;
volatile uint32_t g_tpiu_cspsr_at_boot;
/* TER masks individual stimulus ports independently of TCR's master
 * enable -- never independently verified before. If it reads back 0, every
 * STIM write succeeds at the FIFO but is silently discarded by ITM before
 * ever reaching the formatter, matching the frozen-output ILA finding. */
volatile uint32_t g_itm_ter_at_boot;

/* Decisive test, 2026-08-17: g_itm_stim0_at_boot reads ready(1) right
 * after init, contradicting the 2026-08-16 ILA finding that the CPU
 * never returns from its first blocking m3_itm_write(). Two reads of
 * this counter with a delay in between, both via the A53's AXI window,
 * settle it directly: incrementing means the CPU is genuinely completing
 * emit_next() calls (including their STIM writes) in a normal loop, not
 * stuck at all. */
volatile uint32_t g_heartbeat;
/* Two JTAG reads of g_dwt_cyccnt_latest at different wall-clock times
 * settle whether M3_DWT_CTRL's write actually enabled cycle counting (PPB
 * registers, including DWT_CYCCNT itself, aren't reachable via the A53's
 * AXI window -- same reason g_heartbeat exists instead of reading the real
 * counter directly). */
volatile uint32_t g_dwt_ctrl_at_boot;
volatile uint32_t g_dwt_cyccnt_latest;

static void emit_next(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    sequence++;

    uint32_t n = sequence & 15u;
    switch (n) {
    case 0:
        m3_itm_write(0, sequence); /* Timestamp */
        break;
    case 1:
        m3_itm_write(0, (state & 0x3ffu) + 1u); /* Idle */
        break;
    case 2:
        m3_itm_write_width(0, state & 0xffu, 1); /* Malformed */
        break;
    case 3:
        m3_itm_write(0, 0xf0010000u | sequence); /* Fault */
        break;
    default: {
        uint32_t channel = (n % 7u) + 1u;
        uint8_t widths[3] = {1, 2, 4};
        m3_itm_write_width(channel, state, widths[n % 3u]);
        break;
    }
    }
}

int main(void) {
    /* 10 MHz Parallel/4-bit TPIU bandwidth A/B test. The M3 core's HCLK
     * now comes from the dedicated fabric Clocking Wizard, while the PL
     * capture/DMA domain remains at 100 MHz. */
    m3_itm_init(2);
    g_tpiu_sspsr_at_boot = M3_TPIU_SSPSR; /* DEMCR.TRCENA must be set first (done above) */
    g_itm_tcr_at_boot = M3_ITM_TCR;
    g_itm_stim0_at_boot = M3_ITM_STIM(0);
    g_tpiu_itctrl_at_boot = M3_TPIU_ITCTRL;
    g_tpiu_sppr_at_boot = M3_TPIU_SPPR;
    g_tpiu_cspsr_at_boot = M3_TPIU_CSPSR;
    g_itm_ter_at_boot = M3_ITM_TER;
    g_tpiu_ffsr_at_boot = M3_TPIU_FFSR;
    g_tpiu_ffcr_at_boot = M3_TPIU_FFCR;
    g_dwt_ctrl_at_boot = M3_DWT_CTRL;

    for (;;) {
        emit_next();
        g_heartbeat++;
        g_dwt_cyccnt_latest = M3_DWT_CYCCNT;
        for (volatile uint32_t i = 0; i < 10000u; i++) {
        }
    }
}
