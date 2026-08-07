/*
 * startup_pm.S — Cortex-R5F startup with postmortem exception capture.
 *
 * Reset handler and stack setup are identical to startup_bare.S.
 * Exception handlers save {r0..r12} on the exception mode stack and
 * forward to pm_save_from_exc() / pm_on_exception() so faults produce
 * a versioned .noinit postmortem record readable after warm reset.
 *
 * No ThreadX dependency.  Link with postmortem.c and uart.c (bsp_pm).
 */

    .syntax unified
    .arm

#define MODE_FIQ  0x11
#define MODE_IRQ  0x12
#define MODE_SVC  0x13
#define MODE_ABT  0x17
#define MODE_UND  0x1B
#define MODE_SYS  0x1F

/* Exception type codes — must match postmortem.h PM_EXC_* values. */
#define PM_EXC_UNDEF      1
#define PM_EXC_PREFETCH   2
#define PM_EXC_DABT       3
#define PM_EXC_RESERVED   4

/* ── Vector table ─────────────────────────────────────────────────────── */

    .section .vectors, "ax"
    .globl _vectors
_vectors:
    LDR   pc, =reset_handler
    LDR   pc, =__pm_undef_handler
    B     __pm_spin               /* SWI — not used; spin */
    LDR   pc, =__pm_prefetch_handler
    LDR   pc, =__pm_dabt_handler
    B     __pm_spin               /* reserved — spin */
    B     __pm_spin               /* IRQ — must not fire in bare-metal tests */
    B     __pm_spin               /* FIQ — must not fire in bare-metal tests */

/* ── Reset handler ────────────────────────────────────────────────────── */

    .text
    .globl reset_handler
    .type  reset_handler, %function
reset_handler:
    cpsid if

    cps   #MODE_IRQ
    ldr   sp, =_stack_top
    ldr   r1, =_irq_stack_size
    sub   sp, sp, r1

    cps   #MODE_FIQ
    ldr   sp, =_stack_top
    ldr   r1, =_irq_stack_size
    ldr   r2, =_fiq_stack_size
    sub   sp, sp, r1
    sub   sp, sp, r2

    cps   #MODE_ABT
    ldr   sp, =_stack_top
    ldr   r1, =_irq_stack_size
    ldr   r2, =_fiq_stack_size
    ldr   r3, =_abt_stack_size
    sub   sp, sp, r1
    sub   sp, sp, r2
    sub   sp, sp, r3

    cps   #MODE_UND
    ldr   sp, =_stack_top
    ldr   r1, =_irq_stack_size
    ldr   r2, =_fiq_stack_size
    ldr   r3, =_abt_stack_size
    ldr   r4, =_und_stack_size
    sub   sp, sp, r1
    sub   sp, sp, r2
    sub   sp, sp, r3
    sub   sp, sp, r4

    cps   #MODE_SYS
    ldr   sp, =_stack_top
    ldr   r1, =_irq_stack_size
    ldr   r2, =_fiq_stack_size
    ldr   r3, =_abt_stack_size
    ldr   r4, =_und_stack_size
    ldr   r5, =_sys_stack_size
    sub   sp, sp, r1
    sub   sp, sp, r2
    sub   sp, sp, r3
    sub   sp, sp, r4
    sub   sp, sp, r5

    cps   #MODE_SVC
    ldr   sp, =_stack_top
    ldr   r1, =_irq_stack_size
    ldr   r2, =_fiq_stack_size
    ldr   r3, =_abt_stack_size
    ldr   r4, =_und_stack_size
    ldr   r5, =_sys_stack_size
    ldr   r6, =_svc_stack_size
    sub   sp, sp, r1
    sub   sp, sp, r2
    sub   sp, sp, r3
    sub   sp, sp, r4
    sub   sp, sp, r5
    sub   sp, sp, r6

    /* Zero .bss — does NOT touch the .noinit region at PM_RECORD_ADDR. */
    ldr   r0, =_bss_start
    ldr   r1, =_bss_end
    mov   r2, #0
.Lbss_loop:
    cmp   r0, r1
    bge   .Lbss_done
    str   r2, [r0], #4
    b     .Lbss_loop
.Lbss_done:

    bl    main
1:  b     1b
    .size reset_handler, . - reset_handler

/* ── Exception capture macro ─────────────────────────────────────────── */
/*
 * On ARMv7-R, when an exception fires:
 *   • r0–r12 are NOT banked — they hold the faulted context's values.
 *   • lr      is banked and holds the exception-mode return address.
 *   • spsr    is banked and holds the faulted CPSR.
 *
 * Strategy:
 *   1. Allocate 13 words on the exception mode stack.
 *   2. STM {r0..r12} to preserve the faulted context.
 *   3. Call pm_save_from_exc(exc_type, spsr, lr_exc, frame).
 *   4. Call pm_on_exception() — app overrides this weak symbol.
 */

.macro pm_exc_handler label, exc_const
    .globl \label
    .type  \label, %function
\label:
    sub   sp, sp, #(13 * 4)     /* carve frame on exception mode stack */
    stm   sp, {r0-r12}          /* save faulted r0..r12 */
    mrs   r1, spsr               /* r1 = faulted CPSR (spsr argument) */
    mov   r2, lr                 /* r2 = exception-mode LR (lr_exc argument) */
    mov   r3, sp                 /* r3 = frame pointer (pointer to saved r0..r12) */
    mov   r0, #\exc_const        /* r0 = exc_type */
    bl    pm_save_from_exc
    bl    pm_on_exception
1:  b     1b                     /* pm_on_exception must not return; spin if it does */
    .size \label, . - \label
.endm

/* ── Concrete exception handlers ─────────────────────────────────────── */

pm_exc_handler __pm_undef_handler,    PM_EXC_UNDEF
pm_exc_handler __pm_prefetch_handler, PM_EXC_PREFETCH
pm_exc_handler __pm_dabt_handler,     PM_EXC_DABT

/* ── Unrecoverable spin (SWI, IRQ, FIQ, reserved) ──────────────────── */

    .globl __pm_spin
__pm_spin:
    B     __pm_spin
