/* SPDX-License-Identifier: MIT
 *
 * Orbtrace A53 control service — ThreadX + NetX Duo (GEM2) entry point.
 *
 * Brings up the network stack on GEM2, then runs three TCP server threads:
 *   - port 3401 (control): device info / start / stop / status commands
 *   - port 3402 (trace): Orbflow payloads completed by AXI DMA S2MM
 *   - port 3240 (DAP): CMSIS-DAP passthrough to the PL mailbox
 *
 * Both threads pump raw socket bytes through the Rust protocol/register
 * state machines in applications/orbtrace/firmware/a53 (control_firmware_a53)
 * via the extern "C" boundary in that crate's src/ffi.rs. This file owns
 * only the network transport and ThreadX plumbing; framing, CoreSight
 * register programming, and the CMSIS-DAP mailbox protocol live in Rust.
 *
 * The trace thread owns a native AXI DMA scatter/gather ring in DDR. It
 * invalidates each completed payload, copies the Orbflow packet into a NetX
 * TCP packet, and immediately rearms the descriptor before sending.
 */

#include "encore/OS/ThreadX/ThreadXGEM2Driver.h"
#include "nx_api.h"
#include "nx_packet.h"
#include "nx_tcp.h"
#include "platform.h"
#include "timer.h"
#include "tx_api.h"
/* tx_port.h typedefs LONG/ULONG as 32-bit ThreadX ABI types. xil_types.h
 * checks for macros rather than typedefs before declaring conflicting LP64
 * versions, so suppress those duplicate declarations here. */
#define LONG LONG
#define ULONG ULONG
#include "xil_cache.h"
#include "xil_mmu.h"

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
#define ORBTRACE_TRACE_PORT   3402U /* orbtrace_firmware_common::TRACE_PORT */
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
#define ORBTRACE_TRACE_MSS  8960U
#define ORBTRACE_TRACE_TX_QUEUE 40U

/* ── ThreadX / NetX Duo objects ───────────────────────────────────────── */

static NX_IP          ip;
static NX_PACKET_POOL pool;
static NX_PACKET_POOL trace_pool;
static NX_TCP_SOCKET  control_socket;
static NX_TCP_SOCKET  trace_socket;
static NX_TCP_SOCKET  dap_socket;

static TX_THREAD control_thread;
static TX_THREAD trace_thread;
static TX_THREAD dap_thread;

static ULONG control_stack[2048]; /* 8 KiB */
static ULONG trace_stack[2048];   /* 8 KiB */
static ULONG dap_stack[2048];     /* 8 KiB */

#define POOL_PACKET_PAYLOAD 10304U
#define POOL_PACKET_COUNT   192U
static UCHAR pool_memory[POOL_PACKET_PAYLOAD * POOL_PACKET_COUNT]
    __attribute__((aligned(64)));
#define TRACE_POOL_PACKET_PAYLOAD 18688U
#define TRACE_POOL_PACKET_COUNT 96U
static UCHAR trace_pool_memory[TRACE_POOL_PACKET_PAYLOAD * TRACE_POOL_PACKET_COUNT]
    __attribute__((aligned(64), section(".trace_dma")));
static UCHAR ip_stack_memory[4096] __attribute__((aligned(8)));
static UCHAR arp_cache_memory[1024] __attribute__((aligned(4)));

/* ThreadX event trace buffer — see tx_application_define()'s
 * tx_trace_enable() call. Gives scheduler/ISR-level visibility (thread
 * switches, interrupt entry/exit, NetX/ThreadX object events) beyond what
 * this file's own diag counters can show, for chasing the 2026-08-09
 * handoff's GEM2 total-freeze bug: diag_thread's own steady 1 Hz ticking
 * during that freeze already proves ThreadX's tick-timer interrupt kept
 * firing, but this can confirm whether *any* interrupt (not just the
 * timer) is still reaching the CPU around the moment GEM2's own isr_calls
 * stops, which narrows "GEM2-specific interrupt line went silent" from
 * "something broader masked interrupts generally". 16 KiB is a rough,
 * uncalibrated size — enough for a few thousand events, generous for a
 * bring-up capture window rather than sized against a specific event rate. */
#define TRACE_EVENT_BUFFER_SIZE 16384U
static UCHAR trace_event_buffer[TRACE_EVENT_BUFFER_SIZE] __attribute__((aligned(4)));

/* ── Orbflow AXI DMA S2MM ring ──────────────────────────────────────── */

#define ORBTRACE_AXI_BASE       0xA0000000UL
#define ORBTRACE_REG_DMA_LO     0x18UL
#define ORBTRACE_REG_DMA_HI     0x1CUL
#define ORBTRACE_REG_DMA_COUNT  0x20UL

#define TRACE_DMA_BASE          0xA0010000UL
#define TRACE_DMA_S2MM_DMACR    0x30UL
#define TRACE_DMA_S2MM_DMASR    0x34UL
#define TRACE_DMA_CURDESC_LO    0x38UL
#define TRACE_DMA_CURDESC_HI    0x3CUL
#define TRACE_DMA_TAILDESC_LO   0x40UL
#define TRACE_DMA_TAILDESC_HI   0x44UL
#define TRACE_DMA_CR_RUN        (1U << 0)
#define TRACE_DMA_CR_RESET      (1U << 2)
#define TRACE_DMA_SR_HALTED     (1U << 0)
#define TRACE_DMA_SR_ERROR_MASK 0x00000770U
#define TRACE_DMA_BD_COMPLETE   (1U << 31)
#define TRACE_DMA_BD_ERROR_MASK 0x70000000U
#define TRACE_DMA_LENGTH_MASK   0x03FFFFFFU

/* One 8192-byte RTL payload encodes to at most 8228 bytes, including channel,
 * checksum, COBS code bytes, and delimiter. The AXI packer combines two
 * complete Orbflow frames per DMA transfer so NetX can amortize one socket
 * send over multiple MSS-sized TCP segments. Keep modest alignment slack. */
#define TRACE_DMA_BD_COUNT      32U
#define TRACE_DMA_BUFFER_SIZE   16512U
#define TRACE_DMA_CACHE_LINE    64U
#define TRACE_DMA_RESET_BUDGET  1000000U
#define TRACE_DMA_NOT_READY     0xFEU

typedef struct __attribute__((aligned(64))) {
    unsigned int next_lo;
    unsigned int next_hi;
    unsigned int buffer_lo;
    unsigned int buffer_hi;
    unsigned int reserved[2];
    unsigned int control;
    unsigned int status;
    unsigned int application[5];
    unsigned int padding[3];
} TraceDmaDescriptor;

typedef char trace_dma_descriptor_must_be_64_bytes[
    sizeof(TraceDmaDescriptor) == 64U ? 1 : -1];

static TraceDmaDescriptor trace_descriptors[TRACE_DMA_BD_COUNT]
    __attribute__((aligned(64), section(".trace_dma")));
static NX_PACKET *trace_dma_packet_slots[TRACE_DMA_BD_COUNT];
static unsigned int trace_dma_consumer;
static volatile unsigned long trace_dma_bytes;
static volatile unsigned int trace_dma_packets;
static volatile unsigned int trace_dma_errors;

static inline unsigned int trace_mmio_read(unsigned long base, unsigned long offset)
{
    return *(volatile unsigned int *)(base + offset);
}

static inline void trace_mmio_write(unsigned long base, unsigned long offset,
                                    unsigned int value)
{
    *(volatile unsigned int *)(base + offset) = value;
}

static void trace_dma_stop(void)
{
    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_S2MM_DMACR, 0U);
    for (unsigned int i = 0U; i < TRACE_DMA_BD_COUNT; i++) {
        if (trace_dma_packet_slots[i] != NX_NULL) {
            nx_packet_release(trace_dma_packet_slots[i]);
            trace_dma_packet_slots[i] = NX_NULL;
        }
    }
}

static UINT trace_dma_allocate_packet(NX_PACKET **packet_out)
{
    NX_PACKET *packet;
    UINT result = nx_packet_allocate(&trace_pool, &packet, NX_TCP_PACKET, NX_WAIT_FOREVER);
    if (result != NX_SUCCESS) {
        return result;
    }

    /* Keep DMA payload writes on cache lines that cannot contain NetX packet
     * metadata or the headers NetX prepends later. The packet pool is much
     * larger than a trace frame, so this alignment padding is inexpensive. */
    UINTPTR data = ((UINTPTR)packet->nx_packet_prepend_ptr +
                    TRACE_DMA_CACHE_LINE - 1U) &
                   ~((UINTPTR)TRACE_DMA_CACHE_LINE - 1U);
    if (data + TRACE_DMA_BUFFER_SIZE > (UINTPTR)packet->nx_packet_data_end) {
        nx_packet_release(packet);
        return NX_SIZE_ERROR;
    }
    packet->nx_packet_prepend_ptr = (UCHAR *)data;
    packet->nx_packet_append_ptr = (UCHAR *)data;
    packet->nx_packet_length = 0U;
    *packet_out = packet;
    return NX_SUCCESS;
}

static void trace_dma_rearm(unsigned int slot, NX_PACKET *packet)
{
    TraceDmaDescriptor *bd = &trace_descriptors[slot];
    UINTPTR buffer = (UINTPTR)packet->nx_packet_prepend_ptr;

    /* trace_pool's complete packet storage is in the same non-cacheable
     * translation block as the descriptor rings, so ownership transfers do
     * not need payload cache maintenance. */
    bd->buffer_lo = (unsigned int)buffer;
    bd->buffer_hi = (unsigned int)((unsigned long)buffer >> 32);
    bd->control = TRACE_DMA_BUFFER_SIZE;
    bd->status = 0U;
    trace_dma_packet_slots[slot] = packet;
}

static UINT trace_dma_initialize(void)
{
    unsigned int remaining = TRACE_DMA_RESET_BUDGET;

    /* The AXI DMA reference applications require descriptor memory to be
     * non-cacheable on AArch64; their XAxiDma cache-maintenance macros are
     * intentionally no-ops on this architecture. The linker isolates only
     * descriptor rings and the dedicated trace packet pool in their own
     * 2 MiB translation block. DMA therefore writes directly into complete
     * non-cacheable NetX packet buffers without any per-byte cache scan. */
    Xil_SetTlbAttributes((UINTPTR)trace_descriptors, NORM_NONCACHE);

    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_S2MM_DMACR, TRACE_DMA_CR_RESET);
    while ((trace_mmio_read(TRACE_DMA_BASE, TRACE_DMA_S2MM_DMACR) & TRACE_DMA_CR_RESET) != 0U) {
        if (remaining-- == 0U) {
            trace_dma_errors++;
            return NX_NOT_SUCCESSFUL;
        }
    }

    for (unsigned int i = 0U; i < TRACE_DMA_BD_COUNT; i++) {
        unsigned long next = (unsigned long)&trace_descriptors[(i + 1U) % TRACE_DMA_BD_COUNT];
        TraceDmaDescriptor *bd = &trace_descriptors[i];
        memset(bd, 0, sizeof(*bd));
        bd->next_lo = (unsigned int)next;
        bd->next_hi = (unsigned int)(next >> 32);
        NX_PACKET *packet;
        UINT result = trace_dma_allocate_packet(&packet);
        if (result != NX_SUCCESS) {
            trace_dma_errors++;
            return result;
        }
        trace_dma_rearm(i, packet);
    }
    Xil_DCacheFlushRange((INTPTR)trace_descriptors, sizeof(trace_descriptors));

    unsigned long first = (unsigned long)&trace_descriptors[0];
    unsigned long last = (unsigned long)&trace_descriptors[TRACE_DMA_BD_COUNT - 1U];
    trace_mmio_write(ORBTRACE_AXI_BASE, ORBTRACE_REG_DMA_LO, (unsigned int)first);
    trace_mmio_write(ORBTRACE_AXI_BASE, ORBTRACE_REG_DMA_HI, (unsigned int)(first >> 32));
    trace_mmio_write(ORBTRACE_AXI_BASE, ORBTRACE_REG_DMA_COUNT, TRACE_DMA_BD_COUNT);
    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_CURDESC_LO, (unsigned int)first);
    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_CURDESC_HI, (unsigned int)(first >> 32));
    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_S2MM_DMACR, TRACE_DMA_CR_RUN);
    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_TAILDESC_HI, (unsigned int)(last >> 32));
    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_TAILDESC_LO, (unsigned int)last);

    trace_dma_consumer = 0U;
    return NX_SUCCESS;
}

/* Hand one completed DMA frame to NetX without copying it. Each descriptor
 * owns a preallocated packet while hardware writes; completion swaps in a
 * fresh packet and rearms the descriptor before sending the old one. Each
 * transfer contains two independently delimited Orbflow frames; NetX splits
 * it into MSS-sized TCP segments without changing the application stream. */
static UINT trace_dma_send_completed(void)
{
    unsigned int slot = trace_dma_consumer;
    TraceDmaDescriptor *bd = &trace_descriptors[slot];
    Xil_DCacheInvalidateRange((INTPTR)bd, sizeof(*bd));

    if ((bd->status & TRACE_DMA_BD_COMPLETE) == 0U) {
        return TRACE_DMA_NOT_READY;
    }

    unsigned int status = bd->status;
    if ((status & TRACE_DMA_BD_ERROR_MASK) != 0U) {
        trace_dma_errors++;
        return NX_NOT_SUCCESSFUL;
    }
    unsigned int length = status & TRACE_DMA_LENGTH_MASK;
    if (length == 0U || length > TRACE_DMA_BUFFER_SIZE) {
        trace_dma_errors++;
        return NX_NOT_SUCCESSFUL;
    }

    NX_PACKET *packet = trace_dma_packet_slots[slot];
    if (packet == NX_NULL) {
        trace_dma_errors++;
        return NX_NOT_SUCCESSFUL;
    }
    packet->nx_packet_append_ptr = packet->nx_packet_prepend_ptr + length;
    packet->nx_packet_length = length;
    packet->nx_packet_interface_capability_flag |= GEM2_PACKET_NONCACHE;

    NX_PACKET *replacement;
    UINT result = trace_dma_allocate_packet(&replacement);
    if (result != NX_SUCCESS) {
        return result;
    }
    trace_dma_rearm(slot, replacement);
    unsigned long descriptor = (unsigned long)bd;
    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_TAILDESC_HI,
                     (unsigned int)(descriptor >> 32));
    trace_mmio_write(TRACE_DMA_BASE, TRACE_DMA_TAILDESC_LO,
                     (unsigned int)descriptor);
    trace_dma_consumer = (slot + 1U) % TRACE_DMA_BD_COUNT;

    result = nx_tcp_socket_send(&trace_socket, packet, NX_WAIT_FOREVER);
    if (result != NX_SUCCESS) {
        nx_packet_release(packet);
        return result;
    }
    trace_dma_bytes += length;
    trace_dma_packets++;
    return NX_SUCCESS;
}

/* ── Temporary bring-up diagnostic: raw GEM2 register poll ───────────────
 * Reads ISR/NWSR directly, bypassing the interrupt chain entirely, so a
 * "MAC never sees anything" failure and a "MAC receives fine but the ISR
 * chain never runs" failure are distinguishable on UART. */
#define GEM2_BASE     0xFF0D0000UL
#define GEM2_ISR_OFF  0x24UL
#define GEM2_NWSR_OFF 0x08UL
#define GEM2_RXQBASE_OFF 0x18UL

static TX_THREAD diag_thread;
/* 4x'd from the original 1024 ULONGs (4KB). Root-caused on real hardware
 * (see ORBTRACE_TEST_REPORT session 6): this thread's body makes several
 * nested calls (gem2_diag_get_*() plus multiple 15-20-argument xil_printf()
 * calls with local hex-dump buffers and loops) that plausibly exceeded 4KB
 * of stack at its deepest point, silently overflowing into adjacent .bss
 * statics (this array sits directly next to several TX_THREAD control
 * blocks and, a bit further down, arp_cache_memory — see the linker
 * symbol map). That is the leading suspect for garbage later observed in
 * NetX's ARP table (a bit pattern proven impossible from any legitimate
 * ARP-parsing code path) despite the ARP request delivered to NetX by this
 * driver being independently confirmed byte-perfect. Not yet proven by a
 * stack high-water-mark measurement — increased as a direct, cheap test of
 * the hypothesis. */
static ULONG diag_stack[4096];

static void diag_thread_entry(ULONG arg)
{
    (void)arg;
    for (;;) {
        tx_thread_sleep(100); /* 1 s @ 100 Hz tick */
        gem2_tx_poll_recover();
        gem2_rx_poll_recover();
        gem2_link_poll_recover();
        ULONG isr  = *(volatile ULONG *)(GEM2_BASE + GEM2_ISR_OFF);
        ULONG nwsr = *(volatile ULONG *)(GEM2_BASE + GEM2_NWSR_OFF);
        unsigned int rx_frames, tx_frames, isr_calls, last_etype, last_len, tx_complete, last_tx_stat;
        unsigned int tx_head, tx_tail, tx_count, last_isr, rxused_count;
        unsigned int driver_cmd_count, last_driver_cmd, last_driver_status;
        gem2_diag_get(&rx_frames, &tx_frames, &isr_calls, &last_etype, &last_len, &tx_complete, &last_tx_stat,
                      &tx_head, &tx_tail, &tx_count, &last_isr, &rxused_count,
                      &driver_cmd_count, &last_driver_cmd, &last_driver_status);
        unsigned int txused_count, last_txsr, tx_deferred_requests, tx_deferred_runs;
        gem2_diag_get_tx_extra(&txused_count, &last_txsr,
                               &tx_deferred_requests, &tx_deferred_runs);
        unsigned int tx_recover_attempts, tx_recover_txqbase_before, tx_recover_txqbase_after;
        gem2_diag_get_tx_recover(&tx_recover_attempts, &tx_recover_txqbase_before, &tx_recover_txqbase_after);
        unsigned int tx_dst_msw, tx_dst_lsw, tx_dst_cmd;
        gem2_diag_get_tx_dst(&tx_dst_msw, &tx_dst_lsw, &tx_dst_cmd);
        unsigned int tx_dropped_bad_dst = gem2_diag_get_tx_dropped_bad_dst();
        unsigned int tx_retransmit_count, tx_prepend_before, tx_prepend_after, tx_append;
        unsigned int tx_length_before, tx_length_after;
        gem2_diag_get_tx_pkt_state(&tx_retransmit_count, &tx_prepend_before, &tx_prepend_after,
                                    &tx_append, &tx_length_before, &tx_length_after);
        xil_printf("diag: ISR=0x%lx NWSR=0x%lx isr_calls=%u rx_frames=%u tx_frames=%u last_etype=0x%x "
                   "last_len=%u tx_complete=%u last_tx_stat=0x%x tx_head=%u tx_tail=%u tx_count=%u "
                   "last_isr=0x%x rxused_count=%u txused_count=%u last_txsr=0x%x tx_deferred=%u/%u "
                   "drv_cmds=%u last_cmd=%u last_status=%d "
                   "tx_recover_attempts=%u tx_recover_txqbase=0x%x->0x%x tx_dst=%x:%08x tx_dst_cmd=%u "
                   "tx_dropped_bad_dst=%u\r\n",
                   isr, nwsr, isr_calls, rx_frames, tx_frames, last_etype, last_len,
                   tx_complete, last_tx_stat, tx_head, tx_tail, tx_count, last_isr, rxused_count,
                   txused_count, last_txsr, tx_deferred_requests, tx_deferred_runs,
                   driver_cmd_count, last_driver_cmd, (int)last_driver_status,
                   tx_recover_attempts, tx_recover_txqbase_before, tx_recover_txqbase_after,
                   tx_dst_msw, tx_dst_lsw, tx_dst_cmd, tx_dropped_bad_dst);
        /* Temporary bring-up diagnostic: whether gem2_packet_send()'s
         * NX_ETHERNET_SIZE prepend/length mutation is actually being
         * undone before the next call sees the same NX_PACKET pointer
         * again (a NetX TCP retransmission of a still-queued segment). If
         * tx_retransmit_count ever advances while prepend_before !=
         * prepend_after from the *previous* line's own print, the restore
         * added below "Set up TX BD" in gem2_packet_send() isn't taking
         * effect and the header-mutation hypothesis in the 2026-08-09
         * handoff needs another look. */
        xil_printf("diag5: tx_retransmit_count=%u tx_prepend=0x%x->0x%x tx_append=0x%x "
                   "tx_length=%u->%u\r\n",
                   tx_retransmit_count, tx_prepend_before, tx_prepend_after, tx_append,
                   tx_length_before, tx_length_after);
        {
            /* Total-freeze diagnostics: live PHY link status (independent
             * of any GEM2/NetX state — see gem2_link_poll_recover()) plus
             * how many times that detector has escalated to a full
             * MAC/PHY reinit. If link_up ever reads 0 during a freeze, the
             * PHY itself dropped the link and the fault is upstream of the
             * MAC (RGMII/PHY/cable); if it stays 1 throughout, the MAC or
             * its DMA is wedged with the physical link still up. */
            unsigned int phy_addr, phy_found, bmsr, link_up, link_recover_attempts;
            gem2_diag_get_phy_link(&phy_addr, &phy_found, &bmsr, &link_up);
            gem2_diag_get_link_recover(&link_recover_attempts);
            xil_printf("diag6: phy_addr=%u phy_found=%u bmsr=0x%x link_up=%u link_recover_attempts=%u\r\n",
                       phy_addr, phy_found, bmsr, link_up, link_recover_attempts);

            /* Settles whether a sustained isr_calls freeze with
             * link_recover_attempts stuck at 0 means gem2_link_poll_recover()
             * saw trace_active=0 the whole time (trace send loop already
             * exited — most likely via trace_dma_send_completed() detecting
             * a real AXI DMA S2MM error/halt, see dmasr below) or whether
             * trace_active stayed 1 but stall_ticks itself never reached the
             * 3-tick threshold. Also reads TRACE_DMA_S2MM_DMASR directly and
             * unconditionally (unlike trace_dma_send_completed(), which only
             * ever reads it after an already-fatal per-descriptor error) so
             * a halted-but-not-yet-detected AXI DMA S2MM engine is visible
             * on every tick, not just the one that breaks the send loop. */
            unsigned int trace_active, stall_ticks;
            gem2_diag_get_link_poll_state(&trace_active, &stall_ticks);
            unsigned int rx_poll_recover_attempts = gem2_diag_get_rx_poll_recover();
            xil_printf("diag7: trace_active=%u stall_ticks=%u rx_poll_recover_attempts=%u dmasr=0x%x\r\n",
                       trace_active, stall_ticks,
                       rx_poll_recover_attempts,
                       trace_mmio_read(TRACE_DMA_BASE, TRACE_DMA_S2MM_DMASR));
        }
        {
            /* Snapshot the trace socket's TCP sent-queue head under NetX's
             * own IP mutex. Wire captures show this head being retransmitted
             * after a later cumulative ACK has covered it; these fields make
             * the exact sequence/length value seen by NetX's ACK walker
             * observable instead of inferring it from the wire. */
            UINT lock_status = tx_mutex_get(&ip.nx_ip_protection, NX_NO_WAIT);
            if (lock_status == TX_SUCCESS) {
                NX_PACKET *head = trace_socket.nx_tcp_socket_transmit_sent_head;
                unsigned int head_addr = (unsigned int)(ALIGN_TYPE)head;
                unsigned int queue_next = 0U;
                unsigned int tcp_next = 0U;
                unsigned int prepend = 0U;
                unsigned int ip_header = 0U;
                unsigned int ip_header_length = 0U;
                unsigned int packet_length = 0U;
                unsigned int driver_done = 0U;
                unsigned int head_sequence = 0U;
                unsigned int head_end_sequence = 0U;

                if (head != NX_NULL) {
                    queue_next = (unsigned int)(ALIGN_TYPE)head->nx_packet_queue_next;
                    tcp_next = (unsigned int)(ALIGN_TYPE)head->nx_packet_union_next.nx_packet_tcp_queue_next;
                    prepend = (unsigned int)(ALIGN_TYPE)head->nx_packet_prepend_ptr;
                    ip_header = (unsigned int)(ALIGN_TYPE)head->nx_packet_ip_header;
                    ip_header_length = head->nx_packet_ip_header_length;
                    packet_length = head->nx_packet_length;
                    driver_done = (head->nx_packet_queue_next == (NX_PACKET *)NX_DRIVER_TX_DONE);

                    NX_TCP_HEADER *head_tcp = driver_done
                        ? (NX_TCP_HEADER *)head->nx_packet_prepend_ptr
                        : (NX_TCP_HEADER *)(head->nx_packet_ip_header + head->nx_packet_ip_header_length);
                    ULONG sequence = head_tcp->nx_tcp_sequence_number;
                    ULONG word3 = head_tcp->nx_tcp_header_word_3;
                    NX_CHANGE_ULONG_ENDIAN(sequence);
                    NX_CHANGE_ULONG_ENDIAN(word3);
                    ULONG header_length = (word3 >> NX_TCP_HEADER_SHIFT) * (ULONG)sizeof(ULONG);
                    ULONG prefix_length = (ULONG)((ALIGN_TYPE)head_tcp -
                                                  (ALIGN_TYPE)head->nx_packet_prepend_ptr);
                    ULONG payload_length = 0U;
                    if (head->nx_packet_length >= header_length + prefix_length) {
                        payload_length = head->nx_packet_length - header_length - prefix_length;
                    }
                    head_sequence = sequence;
                    head_end_sequence = sequence + payload_length;
                }

                xil_printf("diag8: state=%u mss=%u peer_mss=%u ifcap=0x%x sent_count=%u tx_seq=%u outstanding=%u timeout=%u/%u retries=%u "
                           "head=0x%x done=%u qnext=0x%x tcp_next=0x%x prepend=0x%x ip_header=0x%x "
                           "ip_hlen=%u length=%u head_seq=%u head_end=%u\r\n",
                           trace_socket.nx_tcp_socket_state,
                           trace_socket.nx_tcp_socket_connect_mss,
                           trace_socket.nx_tcp_socket_peer_mss,
                           ip.nx_ip_interface[0].nx_interface_capability_flag,
                           trace_socket.nx_tcp_socket_transmit_sent_count,
                           trace_socket.nx_tcp_socket_tx_sequence,
                           trace_socket.nx_tcp_socket_tx_outstanding_bytes,
                           trace_socket.nx_tcp_socket_timeout,
                           trace_socket.nx_tcp_socket_timeout_rate,
                           trace_socket.nx_tcp_socket_timeout_retries,
                           head_addr, driver_done, queue_next, tcp_next, prepend, ip_header,
                           ip_header_length, packet_length, head_sequence, head_end_sequence);
                tx_mutex_put(&ip.nx_ip_protection);
            } else {
                xil_printf("diag8: ip_mutex_busy=%u\r\n", lock_status);
            }
        }
        {
            unsigned int arp_cache_count, arp_cache_ip[4], arp_cache_msw[4], arp_cache_lsw[4];
            gem2_diag_get_arp_cache(&arp_cache_count, arp_cache_ip, arp_cache_msw, arp_cache_lsw);
            xil_printf("diag: arp_cache_count=%u", arp_cache_count);
            for (unsigned i = 0; i < arp_cache_count; i++) {
                xil_printf(" [%u]=%08x->%04x:%08x", i, arp_cache_ip[i], arp_cache_msw[i], arp_cache_lsw[i]);
            }
            xil_printf("\r\n");
        }

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
        {
            unsigned char arp_dump[28];
            unsigned int arp_dump_valid;
            gem2_diag_get_arp_dump(arp_dump, &arp_dump_valid);
            if (arp_dump_valid) {
                xil_printf("diag: arp_dump=");
                for (unsigned i = 0; i < sizeof(arp_dump); i++) {
                    xil_printf("%02x", arp_dump[i]);
                }
                xil_printf("\r\n");
            }
        }
        {
            unsigned int req_addr_lo, req_addr_hi, req_sizeof;
            unsigned char req_bytes[48];
            gem2_diag_get_req_dump(&req_addr_lo, &req_addr_hi, &req_sizeof, req_bytes);
            xil_printf("diag: req_addr=%08x%08x sizeof_req=%u req_bytes=",
                       req_addr_hi, req_addr_lo, req_sizeof);
            for (unsigned i = 0; i < sizeof(req_bytes); i++) {
                xil_printf("%02x", req_bytes[i]);
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

    /* backlog 4U, not 1U: confirmed via JTAG (M3_TRACE_VERIFICATION_PLAN.md's
     * 2026-08-17 D2 investigation) that a single TCP handshake can get stuck
     * at NX_TCP_SYN_RECEIVED and never complete -- with backlog 1U that one
     * stuck half-open connection permanently starves every later connection
     * attempt on this port, since nx_tcp_server_socket_accept only ever
     * waits on the single queued slot. A backlog >1 lets NetX's listen queue
     * hold several pending handshakes independently, so a later one reaching
     * NX_TCP_ESTABLISHED can still be accepted even while an earlier one
     * never resolves. Doesn't address why that ACK is lost in the first
     * place, only stops one bad connection from wedging the whole service. */
    UINT listen_status = nx_tcp_server_socket_listen(&ip, ORBTRACE_CONTROL_PORT, &control_socket,
                                                      4U, NX_NULL);
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

static void trace_thread_entry(ULONG arg)
{
    (void)arg;

    UINT listen_status = nx_tcp_server_socket_listen(&ip, ORBTRACE_TRACE_PORT, &trace_socket,
                                                      1U, NX_NULL);
    xil_printf("orbtrace: trace socket listen = %d\r\n", listen_status);

    for (;;) {
        if (nx_tcp_server_socket_accept(&trace_socket, NX_WAIT_FOREVER) == NX_SUCCESS) {
            xil_printf("orbtrace: trace client connected\r\n");
            if (trace_dma_initialize() == NX_SUCCESS) {
                /* Tell gem2_link_poll_recover() (ThreadXGEM2Driver.c) that
                 * GEM2 interrupt activity is now expected, so its
                 * total-freeze detector is armed only while a capture is
                 * actually running — see gem2_set_trace_active()'s doc
                 * comment for why this must not fire during ordinary idle
                 * periods. */
                gem2_set_trace_active(1U);
                unsigned int completed_since_sleep = 0U;
                for (;;) {
                    UINT result = trace_dma_send_completed();
                    if (result == TRACE_DMA_NOT_READY) {
                        /* Yield only to threads of the same priority. This
                         * remains a sub-tick poll and does not insert the
                         * 10 ms gap that tx_thread_sleep(1) would. */
                        tx_thread_relinquish();
                    } else if (result != NX_SUCCESS) {
                        xil_printf("orbtrace: trace stream stopped status=%d dmasr=0x%x\r\n",
                                   result,
                                   trace_mmio_read(TRACE_DMA_BASE, TRACE_DMA_S2MM_DMASR));
                        break;
                    } else if (++completed_since_sleep >= 4096U) {
                        /* This thread runs above diagnostics and a plain
                         * relinquish cannot yield to a lower priority. Give
                         * lower-priority recovery/telemetry work one real
                         * tick periodically, but count completed frames—not
                         * empty busy-poll iterations. Counting polls made the
                         * sleep cadence CPU-speed-dependent and repeatedly
                         * left the 32-entry DMA ring exhausted for most of a
                         * 10 ms tick under sustained traffic. */
                        completed_since_sleep = 0U;
                        tx_thread_sleep(1);
                    }
                }
                gem2_set_trace_active(0U);
            } else {
                xil_printf("orbtrace: trace DMA initialization failed\r\n");
            }
            trace_dma_stop();
            xil_printf("orbtrace: trace client disconnected bytes=%lu packets=%u errors=%u\r\n",
                       trace_dma_bytes, trace_dma_packets, trace_dma_errors);
            nx_tcp_socket_disconnect(&trace_socket, NX_NO_WAIT);
        }
        nx_tcp_server_socket_unaccept(&trace_socket);
        nx_tcp_server_socket_relisten(&ip, ORBTRACE_TRACE_PORT, &trace_socket);
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

    /* Registry entries: enough to name every ThreadX/NetX object this
     * application creates below (1 pool, 1 IP, 3 TCP sockets, 4 threads) —
     * see trace_event_buffer's comment.
     *
     * TEMPORARILY DISABLED for a controlled A/B test: a hardware run
     * immediately after first enabling this call froze the entire system
     * (not just GEM2 — diag_thread's own 1 Hz ticking also stopped) within
     * seconds of the first real client TCP connection, which is a new and
     * more severe failure than anything seen without tracing enabled. Not
     * yet proven that this call is the cause rather than coincidence — see
     * the 2026-08-09 handoff. Re-enable only after confirming the freeze
     * still reproduces identically with this line removed. */
    (void)trace_event_buffer;
#if 0
    status = tx_trace_enable(trace_event_buffer, sizeof(trace_event_buffer), 16U);
    xil_printf("orbtrace: tx_trace_enable = %d\r\n", status);
#endif

    status = nx_packet_pool_create(&pool, "orbtrace_pool", POOL_PACKET_PAYLOAD,
                                    pool_memory, sizeof(pool_memory));
    xil_printf("orbtrace: nx_packet_pool_create = %d\r\n", status);
    Xil_SetTlbAttributes((UINTPTR)trace_pool_memory, NORM_NONCACHE);
    status = nx_packet_pool_create(&trace_pool, "orbtrace_trace_pool",
                                    TRACE_POOL_PACKET_PAYLOAD,
                                    trace_pool_memory, sizeof(trace_pool_memory));
    xil_printf("orbtrace: trace nx_packet_pool_create = %d\r\n", status);

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
    status = nx_tcp_socket_create(&ip, &trace_socket, "orbtrace_trace",
                                   NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                   ORBTRACE_TCP_WINDOW, NX_NULL, NX_NULL);
    xil_printf("orbtrace: trace socket create = %d\r\n", status);
    status = nx_tcp_socket_mss_set(&trace_socket, ORBTRACE_TRACE_MSS);
    xil_printf("orbtrace: trace socket MSS set = %d\r\n", status);
    /* The NetX default is 20 packets. With one 1459-byte Orbflow frame per
     * TCP segment that capped outstanding data at exactly 29,180 bytes on
     * hardware and forced the producer to wait for ACK processing. Forty
     * doubles the trace flight allowance while retaining control-path slack
     * after the fixed 64-entry RX ring is armed. */
    status = nx_tcp_socket_transmit_configure(&trace_socket,
                                               ORBTRACE_TRACE_TX_QUEUE,
                                               NX_IP_PERIODIC_RATE,
                                               NX_TCP_MAXIMUM_RETRIES,
                                               NX_TCP_RETRY_SHIFT);
    xil_printf("orbtrace: trace socket TX queue configure = %d\r\n", status);
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
    status = tx_thread_create(&trace_thread, "orbtrace_trace", trace_thread_entry, 0,
                               trace_stack, sizeof(trace_stack), 3, 3,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    xil_printf("orbtrace: trace thread create = %d\r\n", status);
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
