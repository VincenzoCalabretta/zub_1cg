/*
 * AArch64 EL3 exception vector table + ThreadX low-level init for ZynqMP A53.
 *
 * Provides:
 *   _tx_a53_vectors          – 2048-byte EL3 vector table; IRQ entries route to
 *                              _tx_a53_irq_handler which calls _tx_thread_context_save,
 *                              IRQHandler (C), then _tx_thread_context_restore.
 *   _tx_a53_irq_handler      – IRQ stub: save x29/x30, call context_save, dispatch C
 *                              handler, tail-call context_restore.
 *   _tx_initialize_low_level – Called by ThreadX before the scheduler starts:
 *                              installs VBAR_EL3, saves boot SP as system stack,
 *                              records first unused memory.
 *
 * EL context: bare-metal running entirely in EL3.  The Xilinx standalone BSP
 * boot code sets VBAR_EL3 to its own table; we replace it here so ThreadX owns
 * interrupt dispatch.
 */

/* ── Exception Vector Table ─────────────────────────────────────────────── */

    .section .vectors, "ax"
    .align   11         /* 2^11 = 2048 bytes: AArch64 VBAR alignment */

    .global  _tx_a53_vectors
_tx_a53_vectors:

    /* Current EL with SP0 */
    B       .                       /* 0x000 Synchronous */
    .balign 0x80
    B       _tx_a53_irq_handler     /* 0x080 IRQ         */
    .balign 0x80
    B       .                       /* 0x100 FIQ         */
    .balign 0x80
    B       .                       /* 0x180 SError      */
    .balign 0x80

    /* Current EL with SPx (active while any thread or scheduler runs in EL3) */
    B       .                       /* 0x200 Synchronous */
    .balign 0x80
    B       _tx_a53_irq_handler     /* 0x280 IRQ         */
    .balign 0x80
    B       .                       /* 0x300 FIQ         */
    .balign 0x80
    B       .                       /* 0x380 SError      */
    .balign 0x80

    /* Lower EL AArch64 – unused bare-metal, hang */
    B       .
    .balign 0x80
    B       .
    .balign 0x80
    B       .
    .balign 0x80
    B       .
    .balign 0x80

    /* Lower EL AArch32 – unused bare-metal, hang */
    B       .
    .balign 0x80
    B       .
    .balign 0x80
    B       .
    .balign 0x80
    B       .


/* ── IRQ Handler Stub ────────────────────────────────────────────────────── */

    .text
    .align  3

    .global _tx_a53_irq_handler
    .type   _tx_a53_irq_handler, %function
_tx_a53_irq_handler:
    /*
     * On entry: processor has saved ELR_EL3 + SPSR_EL3; interrupts are masked.
     * x29 (FP) and x30 (LR) are live; push them first because
     * _tx_thread_context_restore expects them at the bottom of the saved frame.
     */
    STP     x29, x30, [sp, #-16]!       /* save FP + LR                         */
    BL      _tx_thread_context_save     /* save thread regs; may switch to sys SP */
    BL      IRQHandler                  /* C-level interrupt dispatch             */
    B       _tx_thread_context_restore  /* restore regs / reschedule; never rets  */
    .size   _tx_a53_irq_handler, . - _tx_a53_irq_handler


/* ── _tx_initialize_low_level ───────────────────────────────────────────── */

    .global _tx_initialize_low_level
    .type   _tx_initialize_low_level, %function
_tx_initialize_low_level:
    MSR     DAIFSet, 0x3                    /* mask IRQ + FIQ during setup          */

    /* Install ThreadX vector table into VBAR_EL3 */
    ADRP    x0, _tx_a53_vectors
    ADD     x0, x0, :lo12:_tx_a53_vectors
    MSR     VBAR_EL3, x0
    ISB

    /*
     * _tx_thread_system_stack_ptr = current SP aligned to 16 bytes.
     * The boot/main stack becomes the ThreadX system (scheduler) stack.
     */
    LDR     x0, =_tx_thread_system_stack_ptr
    MOV     x1, sp
    SUB     x1, x1, #15
    BIC     x1, x1, #0xF
    STR     x1, [x0]

    /*
     * _tx_initialize_unused_memory = _end (first byte past loaded BSS).
     * Passed to tx_application_define(); we use static stacks so the value
     * itself is not used, but ThreadX requires it to be non-NULL.
     */
    LDR     x0, =_tx_initialize_unused_memory
    LDR     x1, =_end
    STR     x1, [x0]

    RET
    .size   _tx_initialize_low_level, . - _tx_initialize_low_level
