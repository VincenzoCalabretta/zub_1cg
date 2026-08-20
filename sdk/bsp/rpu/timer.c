#include "timer.h"
#include <stdint.h>

/*
 * ─── RPU PL390 register map ─────────────────────────────────────────────────
 * Reference: UG1085 Zynq UltraScale+ TRM, GIC chapter.
 *            ARM IHI0048B GIC Architecture Specification.
 *
 * RPU PL390 distributor : 0xF900_0000
 * RPU CPU interface     : 0xF900_1000
 *
 * 0xF901_0000/0xF902_0000 are the A53-only GIC-400 windows. Accessing
 * those from the R5 causes an abort before ThreadX can start.
 */
#define GIC_DIST_BASE   0xF9000000UL
#define GIC_CPU_BASE    0xF9001000UL

#define REG32(a)  (*((volatile uint32_t *)(a)))

/* Distributor registers */
#define GICD_CTLR           REG32(GIC_DIST_BASE + 0x000)
#define GICD_ISENABLER(n)   REG32(GIC_DIST_BASE + 0x100 + (n)*4)
#define GICD_IPRIORITYR(n)  REG32(GIC_DIST_BASE + 0x400 + (n)*4)
#define GICD_ITARGETSR(n)   REG32(GIC_DIST_BASE + 0x800 + (n)*4)
#define GICD_ICFGR(n)       REG32(GIC_DIST_BASE + 0xC00 + (n)*4)

/* CPU Interface registers */
#define GICC_CTLR           REG32(GIC_CPU_BASE + 0x000)
#define GICC_PMR            REG32(GIC_CPU_BASE + 0x004)  /* priority mask */
#define GICC_IAR            REG32(GIC_CPU_BASE + 0x00C)  /* interrupt acknowledge */
#define GICC_EOIR           REG32(GIC_CPU_BASE + 0x010)  /* end of interrupt */
#define GICC_RPR            REG32(GIC_CPU_BASE + 0x014)  /* running priority (diag) */
#define GICC_HPPIR          REG32(GIC_CPU_BASE + 0x018)  /* highest pending, no side effects (diag) */
#define GICD_ISPENDR(n)     REG32(GIC_DIST_BASE + 0x200 + (n)*4)  /* set-pending (diag) */
#define GICD_ISACTIVER(n)   REG32(GIC_DIST_BASE + 0x300 + (n)*4)  /* active bit (diag) */
#define GICD_ICFGR(n)       REG32(GIC_DIST_BASE + 0xC00 + (n)*4)  /* edge/level config, 2 bits/id, 16 ids/reg (diag) */

/*
 * ─── TTC0 channel 0 register map ────────────────────────────────────────────
 * Reference: UG1085 chapter Triple Timer Counter.
 * TTC0 base : 0xFF11_0000
 * Channel 0 registers start at offset 0x000 (ch1 at 0x004, ch2 at 0x008
 * within each register's stride).
 */
#define TTC0_BASE           0xFF110000UL

#define TTC_CLK_CNTRL       REG32(TTC0_BASE + 0x00)  /* clock control ch0 */
#define TTC_CNT_CNTRL       REG32(TTC0_BASE + 0x0C)  /* count control ch0 */
#define TTC_COUNT_VAL       REG32(TTC0_BASE + 0x18)  /* current count ch0 */
#define TTC_INTERVAL_VAL    REG32(TTC0_BASE + 0x24)  /* interval register ch0 */
#define TTC_MATCH_0         REG32(TTC0_BASE + 0x30)
#define TTC_ISR             REG32(TTC0_BASE + 0x54)  /* interrupt status ch0 */
#define TTC_IER             REG32(TTC0_BASE + 0x60)  /* interrupt enable ch0 */

/* TTC_CLK_CNTRL bits */
#define CLK_PSDIV_EN        (1U << 0)   /* enable prescaler */
#define CLK_PSDIV_SHIFT     1           /* prescale bits [5:1] → div = 2^(val+1) */

/* TTC_CNT_CNTRL bits */
#define CNT_DIS             (1U << 0)   /* disable counter (1=stopped) */
#define CNT_INTERVAL        (1U << 1)   /* interval mode (resets at INTERVAL_VAL) */
#define CNT_DECR            (1U << 2)   /* count down */
#define CNT_RESET           (1U << 4)   /* reset counter */

/* TTC_ISR / TTC_IER bits */
#define TTC_INT_INTERVAL    (1U << 0)   /* interval interrupt */

/*
 * TTC reference clock frequency.  The FSBL configures CRL_APB_TTC0_REF_CTRL
 * to use LPD_LSBUS which is typically 100 MHz.  Adjust if your BSP differs.
 */
#define TTC_REF_CLK_HZ      100000000UL

/* ThreadX tick rate: 100 ticks/second = 10 ms per tick */
#define TX_TICKS_PER_SEC    100U

/*
 * Prescaler: divide clock by 2^(PSDIV+1).
 * PSDIV = 3 → divide by 16 → counter clock = 100 MHz / 16 = 6.25 MHz
 * Interval for 100 Hz = 6 250 000 / 100 = 62 500  (fits in 16-bit TTC counter)
 */
#define TTC_PSDIV           3U
#define TTC_COUNTER_CLK_HZ  (TTC_REF_CLK_HZ / (1U << (TTC_PSDIV + 1)))
#define TTC_INTERVAL        (TTC_COUNTER_CLK_HZ / TX_TICKS_PER_SEC)

/*
 * GIC INTID for TTC0 channel 0.
 * Zynq UltraScale+ TRM Table 13-1: TTC0 interrupt 0 → SPI ID 36
 * → GIC INTID = 36 + 32 = 68.
 */
#define TTC0_CH0_INTID      68U

/* GIC helper: which ISENABLER word and bit for a given INTID */
#define GIC_ENABLE_WORD(id)  ((id) / 32)
#define GIC_ENABLE_BIT(id)   (1U << ((id) % 32))

/* GIC priority: lower value = higher priority. 0xA0 = mid-range. */
#define TTC0_GIC_PRIORITY   0xA0U

/* ThreadX timer tick function (defined in tx_timer_interrupt.S) */
extern void _tx_timer_interrupt(void);

/* ── GIC initialisation ─────────────────────────────────────────────── */

static void gic_init(void)
{
    /* Disable distributor while configuring */
    GICD_CTLR = 0;

    /* Set target CPU0 for TTC0 channel 0 interrupt (byte field in ITARGETSR) */
    uint32_t reg = TTC0_CH0_INTID / 4;
    uint32_t byte_shift = (TTC0_CH0_INTID % 4) * 8;
    volatile uint32_t *target_reg = &GICD_ITARGETSR(reg);
    *target_reg = (*target_reg & ~(0xFFU << byte_shift)) | (0x01U << byte_shift);

    /* Set priority (byte field in IPRIORITYR) */
    volatile uint32_t *prio_reg = &GICD_IPRIORITYR(TTC0_CH0_INTID / 4);
    *prio_reg = (*prio_reg & ~(0xFFU << byte_shift)) | (TTC0_GIC_PRIORITY << byte_shift);
}

/* ── TTC0 initialisation ────────────────────────────────────────────── */

static void ttc0_init(void)
{
    /* Stop counter and reset it */
    TTC_CNT_CNTRL = CNT_DIS;

    /* Prescaler: divide by 2^(PSDIV+1) */
    TTC_CLK_CNTRL = CLK_PSDIV_EN | (TTC_PSDIV << CLK_PSDIV_SHIFT);

    /* Program interval mode with the counter held stopped. */
    TTC_INTERVAL_VAL = (uint32_t)TTC_INTERVAL;
    TTC_CNT_CNTRL = CNT_DIS | CNT_INTERVAL;

    /* TTC ISR is read-to-clear. Keep its source disabled until the
     * initial ThreadX thread has a valid context. */
    (void)TTC_ISR;
}

/*
 * ISR dispatch called by ThreadX context save assembly after saving the
 * interrupted thread's registers.  ThreadX defines the symbol
 * _tx_thread_irq_processing_return; this C function is reached through
 * the vectored interrupt mechanism defined in tx_thread_context_save.S.
 *
 * Naming convention: ThreadX cortex_r5 port expects a C function named
 * IRQHandler() that it branches to from within _tx_thread_irq_handler.
 * If your ThreadX version uses a different dispatch symbol, rename this.
 */

/* Diagnostic-only (PS_CORESIGHT_TRACE_PLAN.md section 20/21): unconditional,
 * ThreadX-independent evidence of whether IRQHandler() is ever entered at
 * all, and with what INTID, so a JTAG readback can distinguish "GIC/TTC
 * signal path broken" from "ThreadX's own tick/wake logic broken". */
volatile uint32_t g_irq_entries;
volatile uint32_t g_irq_last_intid;
volatile uint32_t g_irq_last_rpr;
volatile uint32_t g_irq_ttc_pending_bit;
volatile uint32_t g_irq_ttc_active_bit;
/* Word 0 covers PPI IDs 16-31, including UG1085's "Legacy IRQ signal (from
 * PL)" at ID 31 -- a candidate source for IRQ entries that never show up as
 * a real GICC_IAR value, since it's a separate, GIC-SPI-independent path
 * ("the FIQ signal and IRQ signal from the PL are inverted and then sent to
 * the interrupt controller"). */
volatile uint32_t g_irq_ppi_pending_word0;
volatile uint32_t g_irq_ppi_active_word0;
/* HPPIR has no read side effects (unlike IAR, which acknowledges/consumes),
 * so reading it first gives the cleanest possible snapshot of what the GIC
 * believed was pending right as this exception was taken. */
volatile uint32_t g_irq_last_hppir;
/* PS_CORESIGHT_TRACE_PLAN.md section 23: edge (1) vs level (0) sensitivity
 * for TTC0_CH0_INTID, 2 bits per interrupt ID, 16 IDs per ICFGR word --
 * never configured by this BSP, a candidate explanation for why the
 * distributor/CPU-interface never shows this interrupt as pending even
 * though it's proven causally necessary (section 23's disable test). */
volatile uint32_t g_irq_ttc_icfgr_bits;
/* PS_CORESIGHT_TRACE_PLAN.md section 24 next-steps items 1-2: every prior
 * TTC_ISR read in this investigation was an external JTAG read, tens of
 * milliseconds after the exception -- too slow to rule out a condition that
 * self-clears faster than that. This is the first read of TTC_ISR from
 * *inside* IRQHandler() itself, unconditional (not gated behind
 * intid==TTC0_CH0_INTID, which never true so far), giving the earliest
 * possible vantage point on whether the interval bit (or any other TTC_ISR
 * bit -- match/overflow/event) is actually set at exception entry.
 * TTC_COUNT_VAL alongside it settles the counter-direction question
 * (increment vs decrement -- CNT_DECR is never set by ttc0_init()). */
volatile uint32_t g_irq_ttc_isr_bits;
volatile uint32_t g_irq_ttc_count_val;

void IRQHandler(void)
{
    uint32_t hppir = GICC_HPPIR & 0x3FFU; /* read before IAR: no side effects */
    uint32_t intid = GICC_IAR & 0x3FFU;   /* bottom 10 bits = INTID */
    uint32_t ttc_isr = TTC_ISR;           /* read-to-clear: earliest possible vantage point */

    g_irq_entries++;
    g_irq_last_hppir = hppir;
    g_irq_last_intid = intid;
    g_irq_last_rpr = GICC_RPR;
    g_irq_ttc_isr_bits = ttc_isr;
    g_irq_ttc_count_val = TTC_COUNT_VAL;
    g_irq_ttc_pending_bit =
        (GICD_ISPENDR(GIC_ENABLE_WORD(TTC0_CH0_INTID)) & GIC_ENABLE_BIT(TTC0_CH0_INTID)) ? 1U : 0U;
    g_irq_ttc_active_bit =
        (GICD_ISACTIVER(GIC_ENABLE_WORD(TTC0_CH0_INTID)) & GIC_ENABLE_BIT(TTC0_CH0_INTID)) ? 1U : 0U;
    g_irq_ppi_pending_word0 = GICD_ISPENDR(0);
    g_irq_ppi_active_word0  = GICD_ISACTIVER(0);
    {
        uint32_t icfgr_word = GICD_ICFGR(TTC0_CH0_INTID / 16U);
        uint32_t shift = 2U * (TTC0_CH0_INTID % 16U);
        g_irq_ttc_icfgr_bits = (icfgr_word >> shift) & 0x3U;
    }

    if (intid == TTC0_CH0_INTID) {
        _tx_timer_interrupt();      /* advance ThreadX time (TTC_ISR already read+cleared above) */
    }

    GICC_EOIR = intid;              /* signal end-of-interrupt to GIC */
}

/* ── Public API ─────────────────────────────────────────────────────── */

void timer_init(void)
{
    gic_init();
    ttc0_init();
}

void timer_start(void)
{
    (void)TTC_ISR;
    GICD_ISENABLER(GIC_ENABLE_WORD(TTC0_CH0_INTID)) = GIC_ENABLE_BIT(TTC0_CH0_INTID);
    GICD_CTLR = 1;
    GICC_PMR  = 0xFF;
    GICC_CTLR = 1;
    TTC_IER = TTC_INT_INTERVAL;
    TTC_CNT_CNTRL = CNT_INTERVAL;
    __asm__ volatile("cpsie i");
}

/*
 * ThreadX calls _tx_initialize_low_level() from tx_initialize_kernel_enter.c
 * before the scheduler starts.  We must set:
 *   _tx_thread_system_stack_ptr  — SVC stack top for the idle scheduler loop
 *   _tx_initialize_unused_memory — first free byte after BSS
 *
 * Hardware (GIC + TTC) is already running because timer_init() was called
 * from main() before tx_kernel_enter().
 */

/* ThreadX internal globals (defined in common/src/tx_thread_initialize.c) */
extern void *_tx_thread_system_stack_ptr;
extern void *_tx_initialize_unused_memory;

/* Linker symbols from memory.lds */
extern char _stack_top[];
extern char _tx_first_unused_memory[];

void _tx_initialize_low_level(void)
{
    _tx_thread_system_stack_ptr  = (void *)_stack_top;
    _tx_initialize_unused_memory = (void *)_tx_first_unused_memory;
}
