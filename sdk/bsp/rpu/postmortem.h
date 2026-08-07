#pragma once
#include <stdint.h>

/*
 * On-target postmortem record for Cortex-R5F (ARMv7-R).
 *
 * Saved to a fixed .noinit region in OCM (PM_RECORD_ADDR) so it survives
 * the warm reset that follows an unrecoverable exception.  The host decoder
 * (tools/pm_decode) reads the raw OCM dump and prints a human-readable
 * report, optionally decorating PC/LR with source locations via addr2line.
 *
 * Exception type codes — exc_type field.
 */
#define PM_EXC_UNDEF      1u   /* undefined instruction */
#define PM_EXC_PREFETCH   2u   /* prefetch abort (IFAR/IFSR valid) */
#define PM_EXC_DABT       3u   /* data abort (DFAR/DFSR valid) */
#define PM_EXC_RESERVED   4u   /* reserved / unhandled vector */

#define PM_MAGIC    0x504D0001u /* "PM" + version 1 sentinel */
#define PM_VERSION  1u

/* Fixed OCM address for the postmortem record — between .bss and the stacks.
 * Must not overlap with any .bss symbol or any mode stack's deepest frame. */
#define PM_RECORD_ADDR  0xFFFFD800u

/* Record format — all fields little-endian, packed to 4-byte boundary.
 * CRC32 (ISO 3309 / Ethernet polynomial) covers every field before crc32. */
typedef struct {
    uint32_t magic;      /* PM_MAGIC when valid */
    uint32_t version;    /* PM_VERSION */
    uint32_t exc_type;   /* PM_EXC_* */
    uint32_t cpsr;       /* SPSR at time of fault (= CPSR of faulted mode) */
    uint32_t pc;         /* fault PC (LR adjusted by exc-type-specific offset) */
    uint32_t lr_raw;     /* raw LR from the exception mode's banked register */
    uint32_t r[13];      /* r0..r12 as they were when the exception fired */
    uint32_t dfsr;       /* CP15 c5/c0/0 — Data Fault Status (data abort) */
    uint32_t dfar;       /* CP15 c6/c0/0 — Data Fault Address (data abort) */
    uint32_t ifsr;       /* CP15 c5/c0/1 — Instruction Fault Status (prefetch) */
    uint32_t crc32;      /* CRC of all preceding fields */
} pm_record_t;

/*
 * Called from exception-mode assembly stubs after saving r0–r12 on the
 * exception mode stack.  frame points to the saved {r0..r12} array.
 * Fills and CRC-seals the record at PM_RECORD_ADDR.
 */
void pm_save_from_exc(uint32_t exc_type, uint32_t spsr,
                      uint32_t lr_exc, const uint32_t *frame);

/*
 * Weak hook called after pm_save_from_exc completes.  The default
 * implementation (in postmortem.c) calls pm_print() then spins.
 * Override in the application to emit test-protocol tokens or take
 * other post-fault actions before halting.
 */
void pm_on_exception(void);

/* Print a human-readable postmortem dump over UART.
 * No-op if PM_RECORD_ADDR does not hold a valid, CRC-correct record. */
void pm_print(void);
