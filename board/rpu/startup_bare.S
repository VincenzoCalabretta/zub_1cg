/*
 * startup_bare.S — Minimal Cortex-R5F startup for bare-metal tests.
 *
 * Sets up all exception-mode stacks (matching the sizes in memory.lds),
 * zeros .bss, and jumps to main().  IRQ and FIQ handlers spin; they must
 * not fire during a bare-metal test run.
 *
 * No ThreadX dependency — safe to link without @threadx//:threadx_r5.
 */

    .syntax unified
    .arm

#define MODE_FIQ  0x11
#define MODE_IRQ  0x12
#define MODE_SVC  0x13
#define MODE_ABT  0x17
#define MODE_UND  0x1B
#define MODE_SYS  0x1F

/* ── Vector table ─────────────────────────────────────────────────────── */

    .section .vectors, "ax"
    .globl _vectors
_vectors:
    LDR   pc, =reset_handler
    B     __bare_undef
    B     __bare_swi
    B     __bare_prefetch
    B     __bare_abort
    B     __bare_reserved
    B     __bare_irq      /* IRQ — must not fire in bare-metal tests */
    B     __bare_fiq

/* ── Reset handler ────────────────────────────────────────────────────── */

    .text
    .globl reset_handler
    .type  reset_handler, %function
reset_handler:
    cpsid if

    /* Carve mode stacks downward from _stack_top (matches memory.lds). */
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

    /* Zero .bss */
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

/* ── Exception handlers (spin) ────────────────────────────────────────── */

    .globl __bare_undef
__bare_undef:
    B     __bare_undef

    .globl __bare_swi
__bare_swi:
    B     __bare_swi

    .globl __bare_prefetch
__bare_prefetch:
    B     __bare_prefetch

    .globl __bare_abort
__bare_abort:
    B     __bare_abort

    .globl __bare_reserved
__bare_reserved:
    B     __bare_reserved

    .globl __bare_irq
__bare_irq:
    B     __bare_irq

    .globl __bare_fiq
__bare_fiq:
    B     __bare_fiq
