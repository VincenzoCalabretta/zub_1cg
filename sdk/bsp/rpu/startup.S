/*
 * startup.S — Cortex-R5F reset and exception handlers for ThreadX.
 *
 * IRQ dispatch model (non-vectored):
 *
 *   IRQ vector → __tx_irq_handler
 *       B _tx_thread_context_save        ← ThreadX saves context
 *       B __tx_irq_processing_return     ← ThreadX branches here when done
 *   __tx_irq_processing_return:
 *       BL IRQHandler                    ← our C GIC dispatch
 *       B  _tx_thread_context_restore    ← ThreadX restores + maybe switches
 *
 * The double-underscore names (__tx_irq_handler, __tx_irq_processing_return)
 * must be globally visible because context_save.S references them via
 * "B __tx_irq_processing_return".
 *
 * Linker symbols consumed:
 *   _stack_top, _bss_start, _bss_end
 *   _irq_stack_size, _fiq_stack_size, _abt_stack_size
 *   _und_stack_size, _svc_stack_size, _sys_stack_size
 *
 * ThreadX port symbols provided by this file:
 *   __tx_irq_handler, __tx_irq_processing_return
 *   __tx_fiq_handler
 *   __tx_undefined, __tx_swi_interrupt
 *   __tx_prefetch_handler, __tx_abort_handler, __tx_reserved_handler
 */

    .syntax unified
    .arm

#define MODE_FIQ  0x11
#define MODE_IRQ  0x12
#define MODE_SVC  0x13
#define MODE_ABT  0x17
#define MODE_UND  0x1B
#define MODE_SYS  0x1F

/* ── Vector table ────────────────────────────────────────────────────── */

    .section .vectors, "ax"
    .globl _vectors
_vectors:
    LDR   pc, =reset_handler
    LDR   pc, =__tx_undefined
    LDR   pc, =__tx_swi_interrupt
    LDR   pc, =__tx_prefetch_handler
    LDR   pc, =__tx_abort_handler
    LDR   pc, =__tx_reserved_handler
    LDR   pc, =__tx_irq_handler          @ → ThreadX IRQ entry
    LDR   pc, =__tx_fiq_handler

/* ── Reset handler ───────────────────────────────────────────────────── */

    .text
    .globl reset_handler
    .type  reset_handler, %function
reset_handler:
#ifdef R5_STARTUP_TRACE
    /* Hardware-test checkpoint: prove that the reset vector reached here
       before any mode switch, stack access, or BSS write. */
    ldr   r0, =0xFFFFFF00
    ldr   r1, =0x52535431          @ "RST1"
    str   r1, [r0]
#endif
    cpsid if            @ disable IRQ + FIQ during init

    /* ── Per-mode stacks (carve downward from _stack_top) ────────────── */

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

    /* SVC mode — stay here for main() */
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

    /* ── Zero .bss ─────────────────────────────────────────────────────── */
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

/* ── ThreadX IRQ entry / processing return ───────────────────────────── */
/*
 * The IRQ vector above branches here.
 * We branch into _tx_thread_context_save; ThreadX saves all registers
 * and then branches back to __tx_irq_processing_return.
 */

    .globl __tx_irq_handler
    .type  __tx_irq_handler, %function
__tx_irq_handler:
    B     _tx_thread_context_save        @ hand off to ThreadX

    .globl __tx_irq_processing_return
__tx_irq_processing_return:
    BL    IRQHandler                     @ our C GIC dispatch (timer.c)
    B     _tx_thread_context_restore     @ ThreadX restores + maybe switches

/* ── FIQ handler (not used — just spin) ─────────────────────────────── */

    .globl __tx_fiq_handler
    .type  __tx_fiq_handler, %function
__tx_fiq_handler:
    B     __tx_fiq_handler

/* ── Default exception handlers ─────────────────────────────────────── */

    .globl __tx_undefined
    .type  __tx_undefined, %function
__tx_undefined:
    B     __tx_undefined

    .globl __tx_swi_interrupt
    .type  __tx_swi_interrupt, %function
__tx_swi_interrupt:
    B     __tx_swi_interrupt

    .globl __tx_prefetch_handler
    .type  __tx_prefetch_handler, %function
__tx_prefetch_handler:
    B     __tx_prefetch_handler

    .globl __tx_abort_handler
    .type  __tx_abort_handler, %function
__tx_abort_handler:
    B     __tx_abort_handler

    .globl __tx_reserved_handler
    .type  __tx_reserved_handler, %function
__tx_reserved_handler:
    B     __tx_reserved_handler
