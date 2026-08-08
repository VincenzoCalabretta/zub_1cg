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
        unsigned int tx_head, tx_tail, tx_count, last_isr, rxused_count;
        unsigned int driver_cmd_count, last_driver_cmd, last_driver_status;
        gem2_diag_get(&rx_frames, &tx_frames, &isr_calls, &last_etype, &last_len, &tx_complete, &last_tx_stat,
                      &tx_head, &tx_tail, &tx_count, &last_isr, &rxused_count,
                      &driver_cmd_count, &last_driver_cmd, &last_driver_status);
        unsigned int txused_count, last_txsr;
        gem2_diag_get_tx_extra(&txused_count, &last_txsr);
        xil_printf("diag: ISR=0x%lx NWSR=0x%lx isr_calls=%u rx_frames=%u tx_frames=%u last_etype=0x%x "
                   "last_len=%u tx_complete=%u last_tx_stat=0x%x tx_head=%u tx_tail=%u tx_count=%u "
                   "last_isr=0x%x rxused_count=%u txused_count=%u last_txsr=0x%x drv_cmds=%u last_cmd=%u last_status=%d\r\n",
                   isr, nwsr, isr_calls, rx_frames, tx_frames, last_etype, last_len,
                   tx_complete, last_tx_stat, tx_head, tx_tail, tx_count, last_isr, rxused_count,
                   txused_count, last_txsr, driver_cmd_count, last_driver_cmd, (int)last_driver_status);

        /* Temporary bring-up diagnostic: NetX's own drop/error counters
         * (NX_IP_INFO / NX_TCP_INFO are on by default — not disabled
         * anywhere in this build). If a SYN is received by the driver
         * (rx_frames increments, last_etype=0x800) but none of these move
         * and no further driver command follows, the packet is being
         * silently dropped somewhere between _nx_ip_packet_receive and
         * _nx_tcp_packet_process without incrementing any of NetX's own
         * accounting — narrows the search to a specific layer. */
        xil_printf("diag2: ip_invalid=%lu ip_csum_err=%lu ip_dropped=%lu tcp_invalid=%lu tcp_csum_err=%lu "
                   "tcp_dropped=%lu tcp_conns=%lu tcp_passive_conns=%lu active_listen=%p "
                   "pool_available=%lu pool_total=%lu\r\n",
                   ip.nx_ip_invalid_packets, ip.nx_ip_receive_checksum_errors,
                   ip.nx_ip_receive_packets_dropped, ip.nx_ip_tcp_invalid_packets,
                   ip.nx_ip_tcp_checksum_errors, ip.nx_ip_tcp_receive_packets_dropped,
                   ip.nx_ip_tcp_connections, ip.nx_ip_tcp_passive_connections,
                   (void *)ip.nx_ip_tcp_active_listen_requests,
                   pool.nx_packet_pool_available, pool.nx_packet_pool_total);
        if (last_etype == 0x0800) {
            unsigned char ip_dump[40];
            gem2_diag_get_ip_dump(ip_dump);
            xil_printf("diag: ip_dump=");
            for (unsigned i = 0; i < sizeof(ip_dump); i++) {
                xil_printf("%02x", ip_dump[i]);
            }
            xil_printf("\r\n");
        }

        /* Temporary bring-up diagnostic: raw RX BD ring state, printed every
         * cycle (not just when RXUSED fires) — a new stall signature shows
         * isr_calls advancing by exactly one extra FRAMERX interrupt with
         * rx_frames never following (rxused_count staying 0), which the old
         * rxused_count-gated print never captures. This shows whether
         * software (rx_tail) and hardware (RXQBASE, and each BD's own
         * NEW/WRAP bits) actually agree on what's next, or have desynced. */
        {
            unsigned int bd_rx_tail, rxqbase, rx_bd_base, addr_words[4], stat_words[4];
            gem2_diag_get_rx_bd_dump(&bd_rx_tail, &rxqbase, &rx_bd_base, addr_words, stat_words);
            xil_printf("diag3: rx_tail=%u rxqbase=0x%x rx_bd_base=0x%x rxqbase_slot=%d "
                       "bd0=%x/%x bd1=%x/%x bd2=%x/%x bd3=%x/%x\r\n",
                       bd_rx_tail, rxqbase, rx_bd_base, (int)((rxqbase - rx_bd_base) / 16 /* GEM2_BD_STRIDE */),
                       addr_words[0], stat_words[0], addr_words[1], stat_words[1],
                       addr_words[2], stat_words[2], addr_words[3], stat_words[3]);
        }

        /* Temporary bring-up diagnostic: raw TX BD ring state, once tx_count
         * has stopped draining (more submitted than completed). */
        if (tx_count > 0) {
            unsigned int t_head, t_tail, t_count, txqbase, tx_bd_base, taddr_words[4], tstat_words[4];
            gem2_diag_get_tx_bd_dump(&t_head, &t_tail, &t_count, &txqbase, &tx_bd_base, taddr_words, tstat_words);
            xil_printf("diag4: tx_head=%u tx_tail=%u tx_count=%u txqbase=0x%x tx_bd_base=0x%x txqbase_slot=%d "
                       "bd0=%x/%x bd1=%x/%x bd2=%x/%x bd3=%x/%x\r\n",
                       t_head, t_tail, t_count, txqbase, tx_bd_base, (int)((txqbase - tx_bd_base) / 16 /* GEM2_BD_STRIDE */),
                       taddr_words[0], tstat_words[0], taddr_words[1], tstat_words[1],
                       taddr_words[2], tstat_words[2], taddr_words[3], tstat_words[3]);
        }
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

    UINT listen_status = nx_tcp_server_socket_listen(&ip, ORBTRACE_CONTROL_PORT, &control_socket,
                                                      1U, NX_NULL);
    xil_printf("orbtrace: control socket listen = %d\r\n", listen_status);

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

    UINT listen_status = nx_tcp_server_socket_listen(&ip, ORBTRACE_DAP_PORT, &dap_socket, 1U, NX_NULL);
    xil_printf("orbtrace: dap socket listen = %d\r\n", listen_status);

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
