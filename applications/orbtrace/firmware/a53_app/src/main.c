/* SPDX-License-Identifier: MIT
 *
 * Orbtrace A53 control service — ThreadX + NetX Duo (GEM2) entry point.
 *
 * Brings up the network stack on GEM2, then runs two TCP server threads:
 *   - port 3401 (control): device info / start / stop / status commands
 *   - port 3240 (DAP): CMSIS-DAP passthrough to the PL mailbox
 *
 * Both threads pump raw socket bytes through the Rust protocol/register
 * state machines in applications/orbtrace/firmware/a53 (control_firmware_a53)
 * via the extern "C" boundary in that crate's src/ffi.rs. This file owns
 * only the network transport and ThreadX plumbing; framing, CoreSight
 * register programming, and the CMSIS-DAP mailbox protocol live in Rust.
 *
 * Trace payload delivery on TCP 3402 (Orbflow / AXI DMA ring) is not wired
 * up yet — see applications/orbtrace/firmware/README.md.
 */

#include "encore/OS/ThreadX/ThreadXGEM2Driver.h"
#include "nx_api.h"
#include "platform.h"
#include "timer.h"
#include "tx_api.h"

extern void xil_printf(const char *fmt, ...);

/* control_firmware_a53 (Rust, applications/orbtrace/firmware/a53/src/ffi.rs).
 * `unsigned long` matches Rust `usize` on the LP64 aarch64-none-elf ABI. */
extern unsigned long orbtrace_control_feed(const unsigned char *input, unsigned long input_len,
                                            unsigned char *response, unsigned long response_cap,
                                            unsigned long *response_len_out);
extern unsigned long orbtrace_dap_feed(const unsigned char *input, unsigned long input_len,
                                        unsigned char *response, unsigned long response_cap,
                                        unsigned long *response_len_out, unsigned int poll_budget);

/* ── Network configuration ────────────────────────────────────────────── */

#define ORBTRACE_IP_ADDRESS  IP_ADDRESS(192, 168, 1, 50)
#define ORBTRACE_NETMASK     IP_ADDRESS(255, 255, 255, 0)
#define ORBTRACE_CONTROL_PORT 3401U /* orbtrace_firmware_common::CONTROL_PORT */
#define ORBTRACE_DAP_PORT     3240U /* orbtrace_firmware_common::DAP_PORT */

/* Bounds each CMSIS-DAP mailbox poll inside a single dap_exchange byte —
 * generous relative to the busy-wait budgets used in host unit tests since
 * this runs against real PL timing, not a mock. */
#define DAP_POLL_BUDGET 1000000U

/* One response frame is a 4-byte length prefix plus at most
 * MAX_CONTROL_PAYLOAD/MAX_DAP_PACKET bytes (orbtrace_firmware_common). */
#define CONTROL_RESPONSE_CAP 4100U
#define DAP_RESPONSE_CAP     1028U

/* NetX Duo does not define a default TCP receive window size; example
 * applications pick their own. 4 KiB covers one full control frame without
 * forcing a window update mid-frame. */
#define ORBTRACE_TCP_WINDOW 4096U

/* ── ThreadX / NetX Duo objects ───────────────────────────────────────── */

static NX_IP          ip;
static NX_PACKET_POOL pool;
static NX_TCP_SOCKET  control_socket;
static NX_TCP_SOCKET  dap_socket;

static TX_THREAD control_thread;
static TX_THREAD dap_thread;

static ULONG control_stack[2048]; /* 8 KiB */
static ULONG dap_stack[2048];     /* 8 KiB */

#define POOL_PACKET_PAYLOAD 1536U
#define POOL_PACKET_COUNT   16U
static UCHAR pool_memory[POOL_PACKET_PAYLOAD * POOL_PACKET_COUNT]
    __attribute__((aligned(64)));
static UCHAR ip_stack_memory[4096] __attribute__((aligned(8)));
static UCHAR arp_cache_memory[1024] __attribute__((aligned(4)));

/* ── Temporary bring-up diagnostic: raw GEM2 register poll ───────────────
 * Reads ISR/NWSR directly, bypassing the interrupt chain entirely, so a
 * "MAC never sees anything" failure and a "MAC receives fine but the ISR
 * chain never runs" failure are distinguishable on UART. */
#define GEM2_BASE     0xFF0D0000UL
#define GEM2_ISR_OFF  0x24UL
#define GEM2_NWSR_OFF 0x08UL
#define GEM2_RXQBASE_OFF 0x18UL

static TX_THREAD diag_thread;
static ULONG diag_stack[1024];

static void diag_thread_entry(ULONG arg)
{
    (void)arg;
    for (;;) {
        tx_thread_sleep(100); /* 1 s @ 100 Hz tick */
        ULONG isr  = *(volatile ULONG *)(GEM2_BASE + GEM2_ISR_OFF);
        ULONG nwsr = *(volatile ULONG *)(GEM2_BASE + GEM2_NWSR_OFF);
        unsigned int rx_frames, tx_frames, isr_calls, last_etype, last_len, tx_complete, last_tx_stat;
        gem2_diag_get(&rx_frames, &tx_frames, &isr_calls, &last_etype, &last_len, &tx_complete, &last_tx_stat);
        xil_printf("diag: ISR=0x%lx NWSR=0x%lx isr_calls=%u rx_frames=%u tx_frames=%u last_etype=0x%x "
                   "last_len=%u tx_complete=%u last_tx_stat=0x%x\r\n",
                   isr, nwsr, isr_calls, rx_frames, tx_frames, last_etype, last_len, tx_complete, last_tx_stat);
    }
}

/* ── One TCP server thread: accept, pump bytes through `feed`, send replies ── */

static void serve_control(NX_PACKET *packet)
{
    for (NX_PACKET *frag = packet; frag != NX_NULL; frag = frag->nx_packet_next) {
        UCHAR *data      = frag->nx_packet_prepend_ptr;
        ULONG  remaining = (ULONG)(frag->nx_packet_append_ptr - frag->nx_packet_prepend_ptr);
        ULONG  offset    = 0U;

        while (offset < remaining) {
            UCHAR response[CONTROL_RESPONSE_CAP];
            unsigned long response_len = 0U;
            unsigned long consumed = orbtrace_control_feed(
                data + offset, remaining - offset,
                response, sizeof(response), &response_len);
            offset += consumed ? (ULONG)consumed : 1U;

            if (response_len > 0U) {
                NX_PACKET *reply;
                if (nx_packet_allocate(&pool, &reply, NX_TCP_PACKET, NX_WAIT_FOREVER) == NX_SUCCESS) {
                    if (nx_packet_data_append(reply, response, (ULONG)response_len, &pool,
                                               NX_WAIT_FOREVER) == NX_SUCCESS) {
                        nx_tcp_socket_send(&control_socket, reply, NX_WAIT_FOREVER);
                    } else {
                        nx_packet_release(reply);
                    }
                }
            }
        }
    }
    nx_packet_release(packet);
}

static void control_thread_entry(ULONG arg)
{
    (void)arg;

    nx_tcp_server_socket_listen(&ip, ORBTRACE_CONTROL_PORT, &control_socket,
                                 1U, NX_NULL);

    for (;;) {
        if (nx_tcp_server_socket_accept(&control_socket, NX_WAIT_FOREVER) == NX_SUCCESS) {
            xil_printf("orbtrace: control client connected\r\n");
            for (;;) {
                NX_PACKET *packet;
                if (nx_tcp_socket_receive(&control_socket, &packet, NX_WAIT_FOREVER) != NX_SUCCESS) {
                    break;
                }
                serve_control(packet);
            }
            xil_printf("orbtrace: control client disconnected\r\n");
            nx_tcp_socket_disconnect(&control_socket, NX_NO_WAIT);
        }
        nx_tcp_server_socket_unaccept(&control_socket);
        nx_tcp_server_socket_relisten(&ip, ORBTRACE_CONTROL_PORT, &control_socket);
    }
}

static void serve_dap(NX_PACKET *packet)
{
    for (NX_PACKET *frag = packet; frag != NX_NULL; frag = frag->nx_packet_next) {
        UCHAR *data      = frag->nx_packet_prepend_ptr;
        ULONG  remaining = (ULONG)(frag->nx_packet_append_ptr - frag->nx_packet_prepend_ptr);
        ULONG  offset    = 0U;

        while (offset < remaining) {
            UCHAR response[DAP_RESPONSE_CAP];
            unsigned long response_len = 0U;
            unsigned long consumed = orbtrace_dap_feed(
                data + offset, remaining - offset,
                response, sizeof(response), &response_len, DAP_POLL_BUDGET);
            offset += consumed ? (ULONG)consumed : 1U;

            if (response_len > 0U) {
                NX_PACKET *reply;
                if (nx_packet_allocate(&pool, &reply, NX_TCP_PACKET, NX_WAIT_FOREVER) == NX_SUCCESS) {
                    if (nx_packet_data_append(reply, response, (ULONG)response_len, &pool,
                                               NX_WAIT_FOREVER) == NX_SUCCESS) {
                        nx_tcp_socket_send(&dap_socket, reply, NX_WAIT_FOREVER);
                    } else {
                        nx_packet_release(reply);
                    }
                }
            }
        }
    }
    nx_packet_release(packet);
}

static void dap_thread_entry(ULONG arg)
{
    (void)arg;

    nx_tcp_server_socket_listen(&ip, ORBTRACE_DAP_PORT, &dap_socket, 1U, NX_NULL);

    for (;;) {
        if (nx_tcp_server_socket_accept(&dap_socket, NX_WAIT_FOREVER) == NX_SUCCESS) {
            xil_printf("orbtrace: dap client connected\r\n");
            for (;;) {
                NX_PACKET *packet;
                if (nx_tcp_socket_receive(&dap_socket, &packet, NX_WAIT_FOREVER) != NX_SUCCESS) {
                    break;
                }
                serve_dap(packet);
            }
            xil_printf("orbtrace: dap client disconnected\r\n");
            nx_tcp_socket_disconnect(&dap_socket, NX_NO_WAIT);
        }
        nx_tcp_server_socket_unaccept(&dap_socket);
        nx_tcp_server_socket_relisten(&ip, ORBTRACE_DAP_PORT, &dap_socket);
    }
}

/* ── Application definition (called by ThreadX before scheduler) ─────── */

/* Temporary bring-up instrumentation: every stage prints its NetX/ThreadX
 * status code so a silent init failure is visible on UART instead of
 * looking identical to a PHY/wiring problem (nothing printed at all). */
void tx_application_define(void *first_unused_memory)
{
    UINT status;
    (void)first_unused_memory;

    status = nx_packet_pool_create(&pool, "orbtrace_pool", POOL_PACKET_PAYLOAD,
                                    pool_memory, sizeof(pool_memory));
    xil_printf("orbtrace: nx_packet_pool_create = %d\r\n", status);

    status = nx_ip_create(&ip, "orbtrace_ip", ORBTRACE_IP_ADDRESS, ORBTRACE_NETMASK,
                           &pool, nx_driver_gem2, ip_stack_memory, sizeof(ip_stack_memory),
                           1 /* priority: highest among network-related threads */);
    xil_printf("orbtrace: nx_ip_create = %d\r\n", status);

    status = nx_arp_enable(&ip, arp_cache_memory, sizeof(arp_cache_memory));
    xil_printf("orbtrace: nx_arp_enable = %d\r\n", status);
    status = nx_tcp_enable(&ip);
    xil_printf("orbtrace: nx_tcp_enable = %d\r\n", status);

    status = nx_tcp_socket_create(&ip, &control_socket, "orbtrace_control",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                   ORBTRACE_TCP_WINDOW, NX_NULL, NX_NULL);
    xil_printf("orbtrace: control socket create = %d\r\n", status);
    status = nx_tcp_socket_create(&ip, &dap_socket, "orbtrace_dap",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                   ORBTRACE_TCP_WINDOW, NX_NULL, NX_NULL);
    xil_printf("orbtrace: dap socket create = %d\r\n", status);

    /* Same priority: the Rust FFI boundary serializes register/mailbox
     * access between these two threads with its own spinlock (see
     * applications/orbtrace/firmware/a53/src/ffi.rs), which only stays
     * livelock-free under equal ThreadX priority. */
    status = tx_thread_create(&control_thread, "orbtrace_control", control_thread_entry, 0,
                               control_stack, sizeof(control_stack), 2, 2,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    xil_printf("orbtrace: control thread create = %d\r\n", status);
    status = tx_thread_create(&dap_thread, "orbtrace_dap", dap_thread_entry, 0,
                               dap_stack, sizeof(dap_stack), 2, 2,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    xil_printf("orbtrace: dap thread create = %d\r\n", status);

    status = tx_thread_create(&diag_thread, "orbtrace_diag", diag_thread_entry, 0,
                               diag_stack, sizeof(diag_stack), 4, 4,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    xil_printf("orbtrace: diag thread create = %d\r\n", status);
}

/* ── C entry point ───────────────────────────────────────────────────── */

int main(void)
{
    init_platform();

    xil_printf("Orbtrace A53 control service\r\n");

    timer_init();
    nx_system_initialize();

    /* Hand control to ThreadX — never returns. */
    tx_kernel_enter();

    return 0;
}
