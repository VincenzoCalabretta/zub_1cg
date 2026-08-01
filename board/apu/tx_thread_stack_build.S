/* Override of ports/cortex_a53/gnu/src/tx_thread_stack_build.S.
 *
 * The upstream file builds a thread initial SPSR of 0xC (EL3t = SP_EL0).
 * On bare-metal EL3, SP_EL0 is never initialized, so the first stack
 * operation in a new thread traps to the Synchronous handler.
 *
 * Fix: use 0xD (EL3h = SP_EL3).  _tx_thread_schedule already sets SP_EL3
 * to the thread stack before the ERET, so EL3h threads have a valid stack
 * immediately on entry.
 */

    .text
    .align 3

    .global _tx_thread_stack_build
    .type   _tx_thread_stack_build, @function
_tx_thread_stack_build:

    LDR     x4, [x0, #24]                   /* tx_thread_stack_end (highest addr) */
    BIC     x4, x4, #0xF                    /* 16-byte align                      */

    MOV     x2, #0
    MOV     x3, #0

    STP     x2, x3, [x4, #-16]!             /* backtrace = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x29, x30  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x0,  x1   = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x2,  x3   = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x4,  x5   = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x6,  x7   = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x8,  x9   = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x10, x11  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x12, x13  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x14, x15  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x16, x17  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x18, x19  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x20, x21  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x22, x23  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x24, x25  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x26, x27  = 0                      */
    STP     x2, x3, [x4, #-16]!             /* x28, (pad) = 0                     */

    MOV     x2, #0xD                        /* SPSR: EL3h (SP_EL3), DAIF=0        */
    MOV     x3, x1                          /* ELR  = thread entry function        */
    STP     x2, x3, [x4, #-16]!             /* push SPSR, ELR                      */

    STR     x4, [x0, #8]                    /* thread->tx_thread_stack_ptr = x4   */
    RET
    .size   _tx_thread_stack_build, . - _tx_thread_stack_build
