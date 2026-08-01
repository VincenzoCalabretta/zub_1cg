#include "tx_api.h"
#include "uart.h"
#include "timer.h"

/* ── ThreadX objects ─────────────────────────────────────────────────── */

static TX_THREAD hello_thread;

/* Stack for the hello thread (words, aligned to 8 bytes by default) */
static ULONG hello_stack[1024];

/* ── Thread entry ────────────────────────────────────────────────────── */

static void hello_entry(ULONG arg)
{
    (void)arg;
    uart_print("Hello, World!\r\n");
    timer_start();
    for (;;) {
        uart_print("Hello, World!\r\n");
        tx_thread_sleep(100);   /* 100 ticks × 10 ms/tick = 1 second */
    }
}

/* ── Application definition (called by ThreadX before scheduler starts) */

void tx_application_define(void *first_unused_memory)
{
    (void)first_unused_memory;   /* stacks are statically allocated */

    tx_thread_create(
        &hello_thread,           /* control block */
        "hello",                 /* name */
        hello_entry,             /* entry function */
        0,                       /* entry arg */
        hello_stack,             /* stack start */
        sizeof(hello_stack),     /* stack size in bytes */
        1,                       /* priority (1 = highest) */
        1,                       /* preemption threshold */
        TX_NO_TIME_SLICE,        /* time-slice ticks (0 = none) */
        TX_AUTO_START            /* start immediately */
    );
}

/* ── C entry point ───────────────────────────────────────────────────── */

int main(void)
{
    uart_init(115200);

    uart_print("\r\n--- ThreadX Hello World (AES-ZUB R5F) ---\r\n");

    /* Configure the GIC + TTC before handing off to ThreadX. IRQs remain
     * masked until ThreadX restores the initial thread context. */
    timer_init();

    /* Hand off to ThreadX — never returns. */
    tx_kernel_enter();

    return 0;
}
