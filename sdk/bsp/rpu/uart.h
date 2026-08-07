#pragma once

#include <stdint.h>

/*
 * Minimal polled UART driver for Zynq UltraScale+ PS UART.
 *
 * Default: UART1 at 0xFF01_0000.  Switch to UART0 (0xFF00_0000) by
 * changing UART_BASE below if the board routes the console there.
 * The FSBL sets MIO pinmux; we only need to configure the UART core.
 */

void uart_init(uint32_t baud_rate);
void uart_putc(char c);
void uart_print(const char *s);
