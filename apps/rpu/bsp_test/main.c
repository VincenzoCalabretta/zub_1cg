#include "uart.h"
#define TEST_PROTO_PRINT(s) uart_print(s)
#include "test_proto.h"

/*
 * Bare-metal BSP smoke test for AES-ZUB R5F.
 *
 * Tests UART initialization and polled output without ThreadX.
 * The board test script matches [TEST PASS] bsp_uart and fails on [TEST FAIL].
 */

static void test_uart(void)
{
    TEST_BEGIN("bsp_uart");

    /* uart_init programs the UART0 registers; polled uart_print must work
     * for this test to emit anything at all.  Reaching this point confirms
     * the UART is alive.  */
    TEST_DIAG("UART polled write functional");

    /* Round-trip sanity: uart_putc emits a single character; if we reach
     * the next line the FIFO accepted it without stalling. */
    uart_putc('\r');
    uart_putc('\n');

    TEST_PASS("bsp_uart");
}

int main(void)
{
    uart_init(115200);

    test_uart();

    uart_print("[TEST ALL DONE]\r\n");

    for (;;) {}
}
