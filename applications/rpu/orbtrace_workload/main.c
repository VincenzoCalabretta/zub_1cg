#include "tx_api.h"
#include "uart.h"
#include "timer.h"
#define TEST_PROTO_PRINT(s) uart_print(s)
#include "test_proto.h"

/*
 * Deterministic branchy control-flow workload for R5-0 ETM tracing
 * (PS_CORESIGHT_TRACE_PLAN.md Phase 4). Hand-ported from the Rust
 * reference model in //applications/orbtrace/firmware/rpu (kept as a
 * separate host-testable crate; no cross-language golden-vector test ties
 * the two together -- same convention as firmware/m3 <-> firmware/m3_app).
 *
 * Unlike the M3's ITM Workload (which emits explicit stimulus values onto
 * a wire a host decoder reconstructs), ETMv4 traces instruction/branch
 * flow directly -- there's no "emit" call. This instead visits a
 * deterministic, reproducible sequence of distinguishable, noinline branch
 * targets, so a future ETM decoder can cross-check recovered branch
 * addresses against a known sequence -- the same verify-against-a-known-
 * sequence methodology M3_TRACE_VERIFICATION_PLAN.md's Phase F used.
 */

static TX_THREAD workload_thread;
static ULONG workload_stack[1024];

static uint32_t state = 7;
static uint32_t sequence = 0;

/* Cheap, real side effect every action performs, so the compiler can't
 * fold a noinline function's body away as dead code -- also gives a JTAG
 * halt-and-read a coarse "is this still running" signal, same rationale
 * as m3_app's g_heartbeat. */
volatile uint32_t g_heartbeat;

__attribute__((noinline)) static void action_call0(uint32_t v) { g_heartbeat += (v & 0xffu) + 1u; }
__attribute__((noinline)) static void action_call1(uint32_t v) { g_heartbeat += (v & 0xffu) + 2u; }
__attribute__((noinline)) static void action_call2(uint32_t v) { g_heartbeat += (v & 0xffu) + 3u; }
__attribute__((noinline)) static void action_call3(uint32_t v) { g_heartbeat += (v & 0xffu) + 4u; }
__attribute__((noinline)) static void action_call4(uint32_t v) { g_heartbeat += (v & 0xffu) + 5u; }
__attribute__((noinline)) static void action_call5(uint32_t v) { g_heartbeat += (v & 0xffu) + 6u; }
__attribute__((noinline)) static void action_call6(uint32_t v) { g_heartbeat += (v & 0xffu) + 7u; }
__attribute__((noinline)) static void action_branch_true(void) { g_heartbeat += 101u; }
__attribute__((noinline)) static void action_branch_false(void) { g_heartbeat += 103u; }
__attribute__((noinline)) static void action_fault(uint32_t code) { g_heartbeat += code & 0xffu; }
__attribute__((noinline)) static void action_marker(uint32_t seq) { g_heartbeat += seq & 0xffu; }

static void emit_next(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    sequence++;

    uint32_t n = sequence & 15u;
    switch (n) {
    case 0:
        action_marker(sequence);
        break;
    case 1: {
        uint32_t spins = (state & 0x3ffu) + 1u;
        for (volatile uint32_t i = 0; i < spins; i++) {
        }
        break;
    }
    case 2:
        if ((state & 1u) == 0u) {
            action_branch_true();
        } else {
            action_branch_false();
        }
        break;
    case 3:
        action_fault(0xf0010000u | sequence);
        break;
    default: {
        uint32_t target = n % 7u;
        switch (target) {
        case 0: action_call0(state); break;
        case 1: action_call1(state); break;
        case 2: action_call2(state); break;
        case 3: action_call3(state); break;
        case 4: action_call4(state); break;
        case 5: action_call5(state); break;
        default: action_call6(state); break;
        }
        break;
    }
    }
}

static void workload_entry(ULONG arg)
{
    (void)arg;
    TEST_BEGIN("orbtrace_workload");
    TEST_DIAG("R5-0 ETM trace workload running");
    TEST_PASS("orbtrace_workload");
    timer_start();
    for (;;) {
        emit_next();
        tx_thread_sleep(1); /* 1 tick x 10 ms/tick */
    }
}

/* ── Application definition (called by ThreadX before scheduler starts) */

void tx_application_define(void *first_unused_memory)
{
    (void)first_unused_memory; /* stacks are statically allocated */

    tx_thread_create(
        &workload_thread,
        "workload",
        workload_entry,
        0,
        workload_stack,
        sizeof(workload_stack),
        1, /* priority */
        1, /* preemption threshold */
        TX_NO_TIME_SLICE,
        TX_AUTO_START
    );
}

/* ── C entry point ───────────────────────────────────────────────────── */

int main(void)
{
    uart_init(115200);

    uart_print("\r\n--- ThreadX ETM Workload (AES-ZUB R5F) ---\r\n");

    /* Configure the GIC + TTC before handing off to ThreadX. IRQs remain
     * masked until ThreadX restores the initial thread context. */
    timer_init();

    /* Hand off to ThreadX — never returns. */
    tx_kernel_enter();

    return 0;
}
