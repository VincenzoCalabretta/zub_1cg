//! C ABI boundary consumed by the ThreadX/NetX Duo A53 service
//! (`applications/orbtrace/firmware/a53_app`). C owns the TCP 3401/3240
//! accept-and-pump loops and network buffers; these functions own the wire
//! protocol state machines and the orbtrace AXI-Lite register block.

use core::sync::atomic::{AtomicBool, Ordering};

use crate::{
    ControlConnection, Controller, DapConnection, DmaRegisterIo, RegisterIo, AXI_DMA_BASE,
    M3_BRAM_BASE, ORBTRACE_AXI_BASE,
};

struct Mmio;

impl RegisterIo for Mmio {
    fn read(&self, offset: usize) -> u32 {
        // SAFETY: ORBTRACE_AXI_BASE is the fixed Vivado AXI-Lite block address;
        // the A53 MMU maps it as device memory via the vendor BSP page tables.
        unsafe { core::ptr::read_volatile((ORBTRACE_AXI_BASE + offset) as *const u32) }
    }

    fn write(&mut self, offset: usize, value: u32) {
        // SAFETY: see `read`.
        unsafe { core::ptr::write_volatile((ORBTRACE_AXI_BASE + offset) as *mut u32, value) }
    }

    fn write_m3_bram(&mut self, offset: usize, data: &[u8]) {
        // Word-wise stores: the BRAM's native width is 32 bits
        // (create_bd.tcl's m3_mem, Write_Width_A=32), and JTAG-DAP writes to
        // this same window (byte- and word-granular alike, per load_m3.tcl's
        // history) do not reliably land -- narrow AXI writes are the prime
        // suspect. Bias every full word toward a single 32-bit AXI write
        // rather than the four byte-writes a naive byte-copy would emit.
        let mut chunks = data.chunks_exact(4);
        let mut address = M3_BRAM_BASE + offset;
        for chunk in &mut chunks {
            let word = u32::from_le_bytes(chunk.try_into().unwrap());
            // SAFETY: M3_BRAM_BASE is the fixed Vivado PL AXI4 BRAM window
            // for the M3's own code/data; the caller (Controller::command)
            // has already bounds-checked offset+data.len() <= M3_BRAM_SIZE,
            // and the A53 MMU maps the whole PL AXI GP aperture (which this
            // falls within) as device memory via the vendor BSP page tables.
            unsafe { core::ptr::write_volatile(address as *mut u32, word) };
            address += 4;
        }
        for (index, &byte) in chunks.remainder().iter().enumerate() {
            // SAFETY: see above; a trailing partial word is rare (only the
            // image's final chunk can be non-multiple-of-4) and still
            // within the same bounds-checked window.
            unsafe { core::ptr::write_volatile((address + index) as *mut u8, byte) };
        }
    }

    fn read_m3_bram(&self, offset: usize, out: &mut [u8]) {
        let mut chunks = out.chunks_exact_mut(4);
        let mut address = M3_BRAM_BASE + offset;
        for chunk in &mut chunks {
            // SAFETY: see `write_m3_bram`.
            let word = unsafe { core::ptr::read_volatile(address as *const u32) };
            chunk.copy_from_slice(&word.to_le_bytes());
            address += 4;
        }
        for (index, byte) in chunks.into_remainder().iter_mut().enumerate() {
            // SAFETY: see `write_m3_bram`.
            *byte = unsafe { core::ptr::read_volatile((address + index) as *const u8) };
        }
    }
}

impl DmaRegisterIo for Mmio {
    fn read_dma(&self, offset: usize) -> u32 {
        // SAFETY: AXI_DMA_BASE is the fixed Vivado AXI DMA register block.
        unsafe { core::ptr::read_volatile((AXI_DMA_BASE + offset) as *const u32) }
    }

    fn write_dma(&mut self, offset: usize, value: u32) {
        // SAFETY: see `read_dma`.
        unsafe { core::ptr::write_volatile((AXI_DMA_BASE + offset) as *mut u32, value) }
    }
}

/// Both service threads (TCP 3401 and TCP 3240) touch the same physical
/// register block through the same `Controller`. A ThreadX-preemptible
/// single-core A53 does not need cross-core atomicity here, only mutual
/// exclusion between those two threads, so a bare spinlock is sufficient —
/// no dynamic allocation and no dependency beyond `core`.
static LOCK: AtomicBool = AtomicBool::new(false);

struct LockGuard;

impl Drop for LockGuard {
    fn drop(&mut self) {
        LOCK.store(false, Ordering::Release);
    }
}

fn lock() -> LockGuard {
    while LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
    LockGuard
}

static mut CONTROLLER: Controller<Mmio> = Controller::new(Mmio);
static mut CONTROL_CONN: ControlConnection = ControlConnection::new();
static mut DAP_CONN: DapConnection = DapConnection::new();

/// Feed bytes received on TCP 3401 into the control-service framer.
///
/// Returns the number of leading bytes of `input` consumed. When a response
/// was produced, `*response_len_out` is set to its length (already framed
/// with its length prefix — send `response[..*response_len_out]` verbatim)
/// before the caller feeds any further input; otherwise it is set to 0.
///
/// # Safety
/// `input` must be valid for `input_len` bytes and `response` for
/// `response_cap` bytes. Must not be called from more than one thread
/// concurrently with itself or with [`orbtrace_dap_feed`].
#[no_mangle]
pub unsafe extern "C" fn orbtrace_control_feed(
    input: *const u8,
    input_len: usize,
    response: *mut u8,
    response_cap: usize,
    response_len_out: *mut usize,
) -> usize {
    let _guard = lock();
    let input = core::slice::from_raw_parts(input, input_len);
    let response = core::slice::from_raw_parts_mut(response, response_cap);
    let outcome = (*core::ptr::addr_of_mut!(CONTROL_CONN)).feed(
        input,
        &mut *core::ptr::addr_of_mut!(CONTROLLER),
        response,
    );
    match outcome {
        Ok(reply) => {
            *response_len_out = reply.response_length.unwrap_or(0);
            reply.consumed
        }
        // The decoder already reset itself on error; drop the byte that
        // triggered it so the caller's loop makes forward progress.
        Err(_) => {
            *response_len_out = 0;
            1
        }
    }
}

/// Feed bytes received on TCP 3240 into the CMSIS-DAP passthrough framer.
///
/// Same contract as [`orbtrace_control_feed`]; `poll_budget` bounds each
/// DAP mailbox wait so a wedged PL cannot stall the service thread forever.
///
/// # Safety
/// Same as [`orbtrace_control_feed`].
#[no_mangle]
pub unsafe extern "C" fn orbtrace_dap_feed(
    input: *const u8,
    input_len: usize,
    response: *mut u8,
    response_cap: usize,
    response_len_out: *mut usize,
    poll_budget: u32,
) -> usize {
    let _guard = lock();
    let input = core::slice::from_raw_parts(input, input_len);
    let response = core::slice::from_raw_parts_mut(response, response_cap);
    let outcome = (*core::ptr::addr_of_mut!(DAP_CONN)).feed(
        input,
        &mut *core::ptr::addr_of_mut!(CONTROLLER),
        response,
        poll_budget,
    );
    match outcome {
        Ok(reply) => {
            *response_len_out = reply.response_length.unwrap_or(0);
            reply.consumed
        }
        Err(_) => {
            *response_len_out = 0;
            1
        }
    }
}
