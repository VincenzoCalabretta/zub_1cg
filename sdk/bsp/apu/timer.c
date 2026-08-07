#include "timer.h"
#include <stdint.h>

/*
 * GIC400 register map (ZynqMP TRM, UG1085, GIC chapter).
 * The same GIC400 serves both RPU (R5) and APU (A53); CPU interface
 * offset 0x1000 is for A53 core 0.
 */
#define GIC_DIST_BASE       0xF9010000UL
#define GIC_CPU_BASE        0xF9020000UL

#define REG32(a)            (*((volatile uint32_t *)(a)))

#define GICD_CTLR           REG32(GIC_DIST_BASE + 0x000)
#define GICD_ISENABLER(n)   REG32(GIC_DIST_BASE + 0x100 + (n) * 4)
#define GICD_IPRIORITYR(n)  REG32(GIC_DIST_BASE + 0x400 + (n) * 4)
#define GICD_ITARGETSR(n)   REG32(GIC_DIST_BASE + 0x800 + (n) * 4)

#define GICC_CTLR           REG32(GIC_CPU_BASE + 0x000)
#define GICC_PMR            REG32(GIC_CPU_BASE + 0x004)
#define GICC_IAR            REG32(GIC_CPU_BASE + 0x00C)
#define GICC_EOIR           REG32(GIC_CPU_BASE + 0x010)

/*
 * TTC0 channel 0 register map (UG1085, Triple Timer Counter chapter).
 * TTC0 base: 0xFF110000.  Each register has a 4-byte stride per channel
 * (ch0 at offset, ch1 at offset+4, ch2 at offset+8).
 */
#define TTC0_BASE           0xFF110000UL

#define TTC_CLK_CNTRL       REG32(TTC0_BASE + 0x00)
#define TTC_CNT_CNTRL       REG32(TTC0_BASE + 0x0C)
#define TTC_INTERVAL_VAL    REG32(TTC0_BASE + 0x24)
#define TTC_ISR             REG32(TTC0_BASE + 0x54)
#define TTC_IER             REG32(TTC0_BASE + 0x60)

/* TTC_CLK_CNTRL bits */
#define CLK_PSDIV_EN        (1U << 0)
#define CLK_PSDIV_SHIFT     1

/* TTC_CNT_CNTRL bits */
#define CNT_DIS             (1U << 0)
#define CNT_INTERVAL        (1U << 1)
#define CNT_RESET           (1U << 4)

/* TTC_ISR / TTC_IER bits */
#define TTC_INT_INTERVAL    (1U << 0)

/*
 * TTC ref clock: LPD_LSBUS typically 100 MHz (configured by FSBL).
 * Prescaler: PSDIV=3 → divide by 2^(3+1) = 16 → counter at 6.25 MHz.
 * Interval for 100 Hz: 6 250 000 / 100 = 62 500 (fits 16-bit counter).
 */
#define TTC_REF_CLK_HZ      100000000UL
#define TX_TICKS_PER_SEC    100U
#define TTC_PSDIV           3U
#define TTC_COUNTER_CLK_HZ  (TTC_REF_CLK_HZ / (1U << (TTC_PSDIV + 1U)))
#define TTC_INTERVAL_CNT    (TTC_COUNTER_CLK_HZ / TX_TICKS_PER_SEC)

/* TTC0 ch0 GIC INTID: SPI #36 → GIC ID 36 + 32 = 68 (UG1085 Table 13-1) */
#define TTC0_CH0_INTID      68U
#define GIC_ENABLE_WORD(id) ((id) / 32U)
#define GIC_ENABLE_BIT(id)  (1U << ((id) % 32U))
#define TTC0_GIC_PRIORITY   0xA0U   /* mid-range; lower = higher priority */

/*
 * GEM2 GIC INTID: SPI #61 → GIC ID 61 + 32 = 93 (UG1085 Table 13-1).
 * Same priority as TTC0 — Ethernet does not need higher precedence than the
 * kernel tick.
 */
#define GEM2_INTID          93U
#define GEM2_GIC_PRIORITY   0xA0U

/* Defined in ports/cortex_a53/gnu/src/tx_timer_interrupt.S */
extern void _tx_timer_interrupt(void);

/*
 * GEM2 IRQ handler — weak symbol so linking without the NetX GEM2 driver is
 * harmless.  ThreadXGEM2Driver.c provides the strong override.
 */
__attribute__((weak)) void gem2_irq_handler(void) {}

/* ── GIC initialisation ──────────────────────────────────────────────────── */

static void gic_init(void)
{
    GICD_CTLR = 0;  /* disable distributor during configuration */

    /* Route TTC0 ch0 interrupt to CPU0 (byte field in ITARGETSR) */
    uint32_t reg        = TTC0_CH0_INTID / 4U;
    uint32_t byte_shift = (TTC0_CH0_INTID % 4U) * 8U;
    volatile uint32_t *target = &GICD_ITARGETSR(reg);
    *target = (*target & ~(0xFFU << byte_shift)) | (0x01U << byte_shift);

    /* Set priority (byte field in IPRIORITYR) */
    volatile uint32_t *prio = &GICD_IPRIORITYR(TTC0_CH0_INTID / 4U);
    *prio = (*prio & ~(0xFFU << byte_shift)) | (TTC0_GIC_PRIORITY << byte_shift);

    /* Enable TTC0 ch0 in the distributor */
    GICD_ISENABLER(GIC_ENABLE_WORD(TTC0_CH0_INTID)) = GIC_ENABLE_BIT(TTC0_CH0_INTID);

    /* Route GEM2 interrupt to CPU0 and set priority */
    uint32_t gem2_reg        = GEM2_INTID / 4U;
    uint32_t gem2_byte_shift = (GEM2_INTID % 4U) * 8U;
    volatile uint32_t *gem2_target = &GICD_ITARGETSR(gem2_reg);
    *gem2_target = (*gem2_target & ~(0xFFU << gem2_byte_shift)) | (0x01U << gem2_byte_shift);
    volatile uint32_t *gem2_prio = &GICD_IPRIORITYR(GEM2_INTID / 4U);
    *gem2_prio = (*gem2_prio & ~(0xFFU << gem2_byte_shift)) | (GEM2_GIC_PRIORITY << gem2_byte_shift);
    GICD_ISENABLER(GIC_ENABLE_WORD(GEM2_INTID)) = GIC_ENABLE_BIT(GEM2_INTID);

    GICD_CTLR = 1;  /* re-enable distributor */

    GICC_PMR  = 0xFF;   /* allow all priority levels */
    GICC_CTLR = 1;      /* enable CPU interface signalling */
}

/* ── TTC0 channel 0 initialisation ──────────────────────────────────────── */

static void ttc0_init(void)
{
    TTC_CNT_CNTRL    = CNT_DIS;  /* stop counter */
    TTC_CLK_CNTRL    = CLK_PSDIV_EN | (TTC_PSDIV << CLK_PSDIV_SHIFT);
    TTC_INTERVAL_VAL = (uint32_t)TTC_INTERVAL_CNT;
    TTC_CNT_CNTRL    = CNT_INTERVAL | CNT_RESET;  /* CNT_DIS=0 starts it */
    (void)TTC_ISR;       /* read-to-clear any stale interrupt flag */
    TTC_IER = TTC_INT_INTERVAL;
}

/*
 * IRQ dispatcher — called from _tx_a53_irq_handler (in tx_initialize_low_level.S)
 * after _tx_thread_context_save has saved the interrupted thread's state.
 *
 * Naming convention: the ThreadX cortex_a53 port's IRQ stub branches to a C
 * function named IRQHandler().
 */
void IRQHandler(void)
{
    uint32_t intid = GICC_IAR & 0x3FFU;  /* bits [9:0] = interrupt ID */

    if (intid == TTC0_CH0_INTID) {
        (void)TTC_ISR;              /* clear TTC pending flag */
        _tx_timer_interrupt();      /* advance ThreadX tick counter */
    } else if (intid == GEM2_INTID) {
        gem2_irq_handler();         /* NetX GEM2 driver or no-op if not linked */
    }

    GICC_EOIR = intid;              /* signal end-of-interrupt to GIC */
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void timer_init(void)
{
    gic_init();
    ttc0_init();
    /*
     * Do NOT enable CPU IRQs here.  ThreadX enables them automatically via
     * ERET (SPSR_EL3 with DAIF.I=0) when it starts the first thread.
     */
}
