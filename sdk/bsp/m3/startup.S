/*
 * startup.S — Minimal Cortex-M3 (ARMv7-M, Thumb-2) startup.
 *
 * M-profile has a single stack pointer and loads it plus the reset PC
 * straight from the vector table on reset (no mode-switching, unlike the
 * R5/A53 BSPs in this tree). Zeros .bss and jumps to main(); all other
 * exceptions spin — this BSP does not enable interrupts.
 */

    .syntax unified
    .thumb

/* ── Vector table ─────────────────────────────────────────────────────── */
/* Fixed ARMv7-M layout; VTOR must be 128-byte aligned (see memory.lds). */

    .section .vectors, "ax"
    .globl _vectors
_vectors:
    .word _stack_top              /* 0:  initial MSP */
    .word reset_handler + 1       /* 1:  Reset (bit0 set = Thumb) */
    .word default_handler + 1     /* 2:  NMI */
    .word default_handler + 1     /* 3:  HardFault */
    .word default_handler + 1     /* 4:  MemManage */
    .word default_handler + 1     /* 5:  BusFault */
    .word default_handler + 1     /* 6:  UsageFault */
    .word 0                       /* 7:  reserved */
    .word 0                       /* 8:  reserved */
    .word 0                       /* 9:  reserved */
    .word 0                       /* 10: reserved */
    .word default_handler + 1     /* 11: SVCall */
    .word default_handler + 1     /* 12: DebugMon */
    .word 0                       /* 13: reserved */
    .word default_handler + 1     /* 14: PendSV */
    .word default_handler + 1     /* 15: SysTick */

/* ── Reset handler ────────────────────────────────────────────────────── */

    .text
    .thumb_func
    .globl reset_handler
    .type  reset_handler, %function
reset_handler:
    ldr   r0, =_stack_top
    mov   sp, r0

    /* Zero .bss */
    ldr   r0, =_bss_start
    ldr   r1, =_bss_end
    movs  r2, #0
.Lbss_loop:
    cmp   r0, r1
    bge   .Lbss_done
    str   r2, [r0]
    adds  r0, r0, #4
    b     .Lbss_loop
.Lbss_done:

    bl    main
1:  b     1b
    .size reset_handler, . - reset_handler

/* ── Default handler (spins) ──────────────────────────────────────────── */

    .thumb_func
    .globl default_handler
default_handler:
    b     default_handler
