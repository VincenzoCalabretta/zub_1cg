#include "postmortem.h"
#include <stddef.h>
#include <stdint.h>

/* Provided by the BSP (uart.c) — forward-declared to avoid a circular dep
 * between the postmortem library and the BSP that embeds it. */
extern void uart_print(const char *s);

/*
 * The record is placed by the linker at 0xFFFFD800 via the .noinit section
 * in memory.lds.  NOLOAD + no BSS membership means startup.S never zeros it,
 * so it survives a warm reset and the host decoder can read it after power-on.
 */
__attribute__((section(".noinit"), used))
volatile pm_record_t pm_record;

/* ISO 3309 / Ethernet CRC32. */
static uint32_t crc32_compute(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        uint8_t b = *p++;
        for (int i = 0; i < 8; i++) {
            if ((crc ^ (uint32_t)b) & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
            b >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* LR offset from fault PC (ARM instruction set). */
static uint32_t lr_to_pc_offset(uint32_t exc_type) {
    return (exc_type == PM_EXC_DABT) ? 8u : 4u;
}

void pm_save_from_exc(uint32_t exc_type, uint32_t spsr,
                      uint32_t lr_exc, const uint32_t *frame) {
    uint32_t dfsr, dfar, ifsr;
    /* CP15 fault-status and fault-address registers (ARMv7-R). */
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(ifsr));

    pm_record.magic    = PM_MAGIC;
    pm_record.version  = PM_VERSION;
    pm_record.exc_type = exc_type;
    pm_record.cpsr     = spsr;
    pm_record.lr_raw   = lr_exc;
    pm_record.pc       = lr_exc - lr_to_pc_offset(exc_type);
    for (int i = 0; i < 13; i++)
        pm_record.r[i] = frame[i];
    pm_record.dfsr = dfsr;
    pm_record.dfar = dfar;
    pm_record.ifsr = ifsr;
    pm_record.crc32 = crc32_compute(
        (const void *)&pm_record, offsetof(pm_record_t, crc32));
}

/* ── UART output helpers ─────────────────────────────────────────────── */

static void print_hex32(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 9; i >= 2; i--) { buf[i] = hex[v & 0xFu]; v >>= 4; }
    buf[10] = '\0';
    uart_print(buf);
}

static const char *exc_name(uint32_t t) {
    if (t == PM_EXC_UNDEF)    return "undef";
    if (t == PM_EXC_PREFETCH) return "prefetch";
    if (t == PM_EXC_DABT)     return "data-abort";
    return "reserved";
}

void pm_print(void) {
    if (pm_record.magic != PM_MAGIC) return;

    uint32_t crc = crc32_compute((const void *)&pm_record,
                                  offsetof(pm_record_t, crc32));
    const char *crc_tag = (crc == pm_record.crc32) ? "OK" : "BAD";

    uart_print("[POSTMORTEM] exc=");
    uart_print(exc_name(pm_record.exc_type));
    uart_print(" pc=");       print_hex32(pm_record.pc);
    uart_print(" lr=");       print_hex32(pm_record.lr_raw);
    uart_print(" cpsr=");     print_hex32(pm_record.cpsr);
    uart_print(" crc=");      uart_print(crc_tag);
    uart_print("\r\n");

    uart_print("[POSTMORTEM] r0=");  print_hex32(pm_record.r[0]);
    uart_print(" r1=");              print_hex32(pm_record.r[1]);
    uart_print(" r2=");              print_hex32(pm_record.r[2]);
    uart_print(" r3=");              print_hex32(pm_record.r[3]);
    uart_print("\r\n");
    uart_print("[POSTMORTEM] r4=");  print_hex32(pm_record.r[4]);
    uart_print(" r5=");              print_hex32(pm_record.r[5]);
    uart_print(" r6=");              print_hex32(pm_record.r[6]);
    uart_print(" r7=");              print_hex32(pm_record.r[7]);
    uart_print("\r\n");
    uart_print("[POSTMORTEM] r8=");  print_hex32(pm_record.r[8]);
    uart_print(" r9=");              print_hex32(pm_record.r[9]);
    uart_print(" r10=");             print_hex32(pm_record.r[10]);
    uart_print(" r11=");             print_hex32(pm_record.r[11]);
    uart_print(" r12=");             print_hex32(pm_record.r[12]);
    uart_print("\r\n");
    uart_print("[POSTMORTEM] dfsr="); print_hex32(pm_record.dfsr);
    uart_print(" dfar=");             print_hex32(pm_record.dfar);
    uart_print(" ifsr=");             print_hex32(pm_record.ifsr);
    uart_print("\r\n");

    /* Invalidate so a subsequent cold-boot doesn't replay stale data. */
    pm_record.magic = 0u;
}

/* Default hook — print and halt.  Override in the application binary to
 * emit test-protocol tokens or take other post-fault actions. */
__attribute__((weak)) void pm_on_exception(void) {
    pm_print();
    for (;;) {}
}
