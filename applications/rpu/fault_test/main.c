#include "uart.h"
#define TEST_PROTO_PRINT(s) uart_print(s)
#include "test_proto.h"
#include "postmortem.h"

/*
 * Override the weak pm_on_exception() from postmortem.c.
 * After printing the postmortem record the test emits the pass token so
 * the serial-watch harness can verify the fault was captured correctly.
 */
void pm_on_exception(void) {
    pm_print();
    TEST_PASS("fault_capture");
    uart_print("[TEST ALL DONE]\r\n");
    for (;;) {}
}

/*
 * Inject an undefined instruction at runtime.  The .word encodes an
 * instruction that is permanently undefined on ARMv7-R (UDF encoding).
 * The linker sees only the .word directive, so this does not require a
 * special intrinsic; the noinline attribute prevents the compiler from
 * optimising the call away.
 */
__attribute__((noinline))
static void inject_undef(void) {
    __asm__ volatile(".word 0xE7F000F0");  /* ARM UDF #0 — always undefined */
}

int main(void) {
    uart_init(115200);
    TEST_BEGIN("fault_capture");
    uart_print("[TEST DIAG] injecting undefined instruction\r\n");
    inject_undef();
    /* Should not reach here — the UND handler captures and calls pm_on_exception. */
    TEST_FAIL("fault_capture", "exception handler did not fire");
    for (;;) {}
}
