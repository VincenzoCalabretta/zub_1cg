#pragma once

/*
 * Shared on-target test protocol for AES-ZUB firmware.
 *
 * Emits text tokens on the console UART that zub_ctl (--expect / --fail-on)
 * can match from the host without any board-specific knowledge.
 *
 * Usage — define TEST_PROTO_PRINT before including:
 *
 *     #include "uart.h"
 *     #define TEST_PROTO_PRINT(s) uart_print(s)
 *     #include "test_proto.h"
 *
 * Protocol tokens:
 *   [TEST BEGIN] <name>           — test started
 *   [TEST PASS]  <name>           — all assertions in <name> passed
 *   [TEST FAIL]  <name>: <reason> — test failed (triggers --fail-on in zub_ctl)
 *   [TEST ASSERT] <file>:<line>: <expr>  — printed before a FAIL on assert
 *   [TEST DIAG]  <msg>            — informational; not a pass/fail signal
 */

#ifndef TEST_PROTO_PRINT
#  error "Define TEST_PROTO_PRINT(str) before including test_proto.h"
#endif

#define _TP_STR(x)  #x
#define _TP_XSTR(x) _TP_STR(x)

#define TEST_BEGIN(name)           TEST_PROTO_PRINT("[TEST BEGIN] " name "\r\n")
#define TEST_PASS(name)            TEST_PROTO_PRINT("[TEST PASS] "  name "\r\n")
#define TEST_FAIL(name, reason)    TEST_PROTO_PRINT("[TEST FAIL] "  name ": " reason "\r\n")
#define TEST_DIAG(msg)             TEST_PROTO_PRINT("[TEST DIAG] "  msg  "\r\n")

/*
 * Assert expr is true; on failure emit [TEST ASSERT] + [TEST FAIL] and
 * return from the enclosing void function.
 */
#define TEST_ASSERT(name, expr)                                              \
    do {                                                                     \
        if (!(expr)) {                                                       \
            TEST_PROTO_PRINT("[TEST ASSERT] " __FILE__ ":"                  \
                             _TP_XSTR(__LINE__) ": " #expr "\r\n");         \
            TEST_FAIL(name, "assertion failed");                             \
            return;                                                          \
        }                                                                    \
    } while (0)
