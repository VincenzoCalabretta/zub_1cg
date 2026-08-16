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
    /* 4-bit TPIU trace port, matching applications/orbtrace/vivado/
     * create_bd.tcl's PSU__TRACE__WIDTH default so the same trace_format
     * register values apply to both the PS and M3 sources. */
    m3_itm_init(2);

    for (;;) {
        emit_next();
        for (volatile uint32_t i = 0; i < 10000u; i++) {
        }
    }
}
