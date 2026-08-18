/*
 * itm.h — ARMv7-M CoreSight ITM/TPIU register map.
 *
 * Standard architectural addresses (System Control Space + Private
 * Peripheral Bus, same on every Cortex-M3 implementation including Arm
 * DesignStart), not board- or IP-specific.
 */
#ifndef ZUB_SDK_BSP_M3_ITM_H
#define ZUB_SDK_BSP_M3_ITM_H

#include <stdint.h>

#define M3_DEMCR (*(volatile uint32_t *)0xE000EDFCUL)
#define M3_DEMCR_TRCENA (1u << 24) /* Must be set before ITM/TPIU are usable. */

#define M3_ITM_STIM(n) (*(volatile uint32_t *)(0xE0000000UL + 4u * (n)))
#define M3_ITM_STIM8(n) (*(volatile uint8_t *)(0xE0000000UL + 4u * (n)))
#define M3_ITM_STIM16(n) (*(volatile uint16_t *)(0xE0000000UL + 4u * (n)))
#define M3_ITM_TER (*(volatile uint32_t *)0xE0000E00UL) /* Trace Enable Register (per-port) */
#define M3_ITM_TCR (*(volatile uint32_t *)0xE0000E80UL) /* Trace Control Register */
#define M3_ITM_TCR_ITMENA (1u << 0)
#define M3_ITM_TCR_TXENA (1u << 3)
#define M3_ITM_TCR_TRACEBUSID_SHIFT 16u
#define M3_ITM_LAR (*(volatile uint32_t *)0xE0000FB0UL) /* Lock Access Register */
#define M3_ITM_LAR_UNLOCK 0xC5ACCE55u

#define M3_TPIU_SSPSR (*(volatile uint32_t *)0xE0040000UL) /* Supported parallel port sizes (RO, silicon-fixed) */
#define M3_TPIU_ACPR (*(volatile uint32_t *)0xE0040010UL)  /* Async clock prescaler (SWO baud) */
#define M3_TPIU_SPPR (*(volatile uint32_t *)0xE00400F0UL)  /* Selected pin protocol */
#define M3_TPIU_SPPR_PARALLEL 0u
#define M3_TPIU_SPPR_SWO_MANCHESTER 1u
#define M3_TPIU_SPPR_SWO_NRZ 2u
#define M3_TPIU_CSPSR (*(volatile uint32_t *)0xE0040004UL) /* Current sync port size (1/2/4-bit trace port) */
#define M3_TPIU_FFSR (*(volatile uint32_t *)0xE0040300UL)  /* Formatter and flush status (RO) */
#define M3_TPIU_FFCR (*(volatile uint32_t *)0xE0040304UL)  /* Formatter and flush control */

/* Enables tracing and stimulus ports 0-7 with the default synchronous
 * parallel trace protocol, matching orbtrace_pl's DDR capture chain. */
static inline void m3_itm_init(uint32_t port_width_select) {
    M3_DEMCR |= M3_DEMCR_TRCENA;
    M3_ITM_LAR = M3_ITM_LAR_UNLOCK;
    M3_TPIU_SPPR = M3_TPIU_SPPR_PARALLEL;
    /* TPIU_CSPSR's PORT_SIZE field is one-hot at bit (width-1): 1-bit=>bit0,
     * 2-bit=>bit1, 4-bit=>bit3 (bit2 is not a legal port size, so a plain
     * "1u << port_width_select" is wrong for the 4-bit case). */
    uint32_t cspsr_bit = port_width_select == 2u ? 3u : port_width_select; /* 0=>1-bit, 1=>2-bit, 2=>4-bit */
    M3_TPIU_CSPSR = 1u << cspsr_bit;
    M3_ITM_TER = 0xffu; /* stimulus ports 0-7 */
    M3_ITM_TCR = M3_ITM_TCR_ITMENA | M3_ITM_TCR_TXENA | (1u << M3_ITM_TCR_TRACEBUSID_SHIFT);
}

/* Configure asynchronous NRZ SWO. `acpr` is the architectural divider-minus-
 * one, so with the 10 MHz M3 HCLK, acpr=18 yields about 526 kHz -- deliberately
 * conservative for the PL receiver. */
static inline void m3_itm_init_swo_nrz(uint32_t acpr) {
    M3_DEMCR |= M3_DEMCR_TRCENA;
    M3_ITM_LAR = M3_ITM_LAR_UNLOCK;
    M3_TPIU_ACPR = acpr;
    M3_TPIU_SPPR = M3_TPIU_SPPR_SWO_NRZ;
    M3_ITM_TER = 0xffu;
    M3_ITM_TCR = M3_ITM_TCR_ITMENA | M3_ITM_TCR_TXENA | (1u << M3_ITM_TCR_TRACEBUSID_SHIFT);
}

/* Blocks until the stimulus port FIFO has room, then writes one word. */
static inline void m3_itm_write(uint32_t port, uint32_t value) {
    while (!(M3_ITM_STIM(port) & 1u)) {
    }
    M3_ITM_STIM(port) = value;
}

/* As m3_itm_write, but writes 1/2/4 bytes to produce the correspondingly
 * sized ITM packet (1/2/4-byte stimulus writes are architecturally
 * distinct ITM packet sizes, not just truncated 4-byte ones). */
static inline void m3_itm_write_width(uint32_t port, uint32_t value, uint8_t width) {
    while (!(M3_ITM_STIM(port) & 1u)) {
    }
    if (width == 1) {
        M3_ITM_STIM8(port) = (uint8_t)value;
    } else if (width == 2) {
        M3_ITM_STIM16(port) = (uint16_t)value;
    } else {
        M3_ITM_STIM(port) = value;
    }
}

#endif
