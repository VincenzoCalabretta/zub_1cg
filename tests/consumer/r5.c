#include "tx_api.h"
#include "uart.h"
#include "timer.h"

static TX_THREAD thread;
static ULONG stack[256];

static void entry(ULONG arg)
{
    (void)arg;
    uart_print("zub consumer R5\r\n");
    for (;;) { tx_thread_sleep(100); }
}

void tx_application_define(void *unused)
{
    (void)unused;
    tx_thread_create(&thread, "consumer", entry, 0, stack, sizeof(stack),
                     1, 1, TX_NO_TIME_SLICE, TX_AUTO_START);
}

int main(void)
{
    uart_init(115200);
    timer_init();
    tx_kernel_enter();
    return 0;
}
