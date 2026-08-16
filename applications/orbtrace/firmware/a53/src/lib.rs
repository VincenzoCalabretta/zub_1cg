#![no_std]

use orbtrace_firmware_common::{
    FrameDecoder, FrameError, MAX_CONTROL_PAYLOAD, MAX_DAP_PACKET, PROTOCOL_VERSION,
};

mod ffi;

// The `#[panic_handler]` lang item must be resolved inside whichever crate is
// compiled as the final `staticlib` artifact (this one) rather than deferred
// to a downstream Rust binary, since no further Rust linking happens after
// that point. `-C panic=abort` on the rust_static_library target means no
// eh_personality/unwinding lang items are needed alongside it. Host unit
// tests link against std, which supplies its own handler, so this is only
// compiled into the on-target artifact.
#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

pub const ORBTRACE_AXI_BASE: usize = 0xa000_0000;
pub const AXI_DMA_BASE: usize = 0xa001_0000;
/// The M3's own BRAM window (`create_bd.tcl`'s `m3_mem_ctrl`, the PS/A53
/// preload view -- port B is the M3 core's own fetch path and is not
/// reachable from here). Size must match `sdk/bsp/m3/memory.lds`'s `RAM`.
pub const M3_BRAM_BASE: usize = 0xa002_0000;
pub const M3_BRAM_SIZE: usize = 0x1_0000;
const REG_CONTROL: usize = 0x08;
const REG_SOURCE_FORMAT: usize = 0x0c;
const REG_SWO_BAUD: usize = 0x10;
const REG_DMA_BASE_LO: usize = 0x18;
const REG_DMA_BASE_HI: usize = 0x1c;
const REG_DMA_RING_SIZE: usize = 0x20;
const REG_DAP_COMMAND: usize = 0x80;
const REG_DAP_RESPONSE: usize = 0x84;
const REG_DAP_STATUS: usize = 0x88;
const REG_M3_CONTROL: usize = 0xa0;

pub trait RegisterIo {
    fn read(&self, offset: usize) -> u32;
    fn write(&mut self, offset: usize, value: u32);

    /// Write raw bytes into the M3's BRAM window -- a separate PL AXI4 slave
    /// from the AXI-Lite register block `read`/`write` above address, and
    /// the D2 load path's whole reason to exist: unlike the register block,
    /// this BRAM does not reliably respond to JTAG-DAP-originated `mwr`/`dow`
    /// (see load_m3.tcl's history), so it is loaded from the running A53
    /// instead. Default no-op: only the real hardware `Mmio` impl needs
    /// this; existing register-level test mocks don't exercise the M3 load
    /// path. `offset + data.len()` is guaranteed `<= M3_BRAM_SIZE` by the
    /// caller (`Controller::command`).
    fn write_m3_bram(&mut self, _offset: usize, _data: &[u8]) {}

    /// Read raw bytes back from the M3's BRAM window, to verify a load
    /// before releasing the core (mirrors load_m3.tcl's own vector-word
    /// readback check). Default: all zero, matching the write default.
    fn read_m3_bram(&self, _offset: usize, out: &mut [u8]) {
        out.fill(0);
    }
}

pub struct Controller<IO> {
    io: IO,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DapError {
    EmptyRequest,
    BadFrame,
    PacketTooLarge,
    ResponseTooLarge,
    Timeout,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ServiceError {
    Frame(FrameError),
    OutputTooSmall,
    BadControlRequest,
    Dap(DapError),
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ServiceReply {
    pub consumed: usize,
    pub response_length: Option<usize>,
}

/// Per-connection state for TCP 3401. One call emits at most one response so
/// the caller can transmit it before continuing with pipelined input.
pub struct ControlConnection {
    decoder: FrameDecoder<MAX_CONTROL_PAYLOAD>,
}

impl ControlConnection {
    pub const fn new() -> Self {
        Self {
            decoder: FrameDecoder::new(),
        }
    }

    pub fn feed<IO: RegisterIo>(
        &mut self,
        input: &[u8],
        controller: &mut Controller<IO>,
        response: &mut [u8],
    ) -> Result<ServiceReply, ServiceError> {
        for (index, byte) in input.iter().enumerate() {
            let complete = self.decoder.push(*byte).map_err(ServiceError::Frame)?;
            if complete {
                if response.len() < 4 {
                    self.decoder.reset();
                    return Err(ServiceError::OutputTooSmall);
                }
                let length = controller
                    .command(self.decoder.frame().unwrap_or(&[]), &mut response[4..])
                    .map_err(|_| ServiceError::BadControlRequest);
                self.decoder.reset();
                let length = length?;
                response[..4].copy_from_slice(&(length as u32).to_le_bytes());
                return Ok(ServiceReply {
                    consumed: index + 1,
                    response_length: Some(length + 4),
                });
            }
        }
        Ok(ServiceReply {
            consumed: input.len(),
            response_length: None,
        })
    }
}

impl Default for ControlConnection {
    fn default() -> Self {
        Self::new()
    }
}

/// Per-connection state for the unchanged CMSIS-DAP payloads on TCP 3240.
pub struct DapConnection {
    decoder: FrameDecoder<MAX_DAP_PACKET>,
}

impl DapConnection {
    pub const fn new() -> Self {
        Self {
            decoder: FrameDecoder::new(),
        }
    }

    pub fn feed<IO: RegisterIo>(
        &mut self,
        input: &[u8],
        controller: &mut Controller<IO>,
        response: &mut [u8],
        poll_budget: u32,
    ) -> Result<ServiceReply, ServiceError> {
        for (index, byte) in input.iter().enumerate() {
            let complete = self.decoder.push(*byte).map_err(ServiceError::Frame)?;
            if complete {
                if response.len() < 4 {
                    self.decoder.reset();
                    return Err(ServiceError::OutputTooSmall);
                }
                let length = controller
                    .dap_exchange(
                        self.decoder.frame().unwrap_or(&[]),
                        &mut response[4..],
                        poll_budget,
                    )
                    .map_err(ServiceError::Dap);
                self.decoder.reset();
                let length = length?;
                response[..4].copy_from_slice(&(length as u32).to_le_bytes());
                return Ok(ServiceReply {
                    consumed: index + 1,
                    response_length: Some(length + 4),
                });
            }
        }
        Ok(ServiceReply {
            consumed: input.len(),
            response_length: None,
        })
    }
}

impl Default for DapConnection {
    fn default() -> Self {
        Self::new()
    }
}

impl<IO: RegisterIo> Controller<IO> {
    pub const fn new(io: IO) -> Self {
        Self { io }
    }

    pub fn configure_dma_ring(&mut self, descriptor_base: u64, descriptor_count: u32) {
        self.io.write(REG_DMA_BASE_LO, descriptor_base as u32);
        self.io
            .write(REG_DMA_BASE_HI, (descriptor_base >> 32) as u32);
        self.io.write(REG_DMA_RING_SIZE, descriptor_count);
    }
    /// Handles the payload after the u32 transport length. The response is
    /// written into caller-owned storage to keep the board path allocation-free.
    pub fn command(&mut self, request: &[u8], response: &mut [u8]) -> Result<usize, ()> {
        if request.len() < 3
            || response.is_empty()
            || u16::from_le_bytes([request[0], request[1]]) != PROTOCOL_VERSION
        {
            return Err(());
        }
        match request[2] {
            1 => {
                let info = b"ZUBoard-Orbtrace/1";
                if response.len() < info.len() {
                    return Err(());
                }
                response[..info.len()].copy_from_slice(info);
                Ok(info.len())
            }
            2 if request.len() == 9 => {
                let baud = u32::from_le_bytes([request[5], request[6], request[7], request[8]]);
                self.io.write(
                    REG_SOURCE_FORMAT,
                    (request[3] as u32) | ((request[4] as u32) << 8),
                );
                self.io.write(REG_SWO_BAUD, baud);
                response[0] = 0;
                Ok(1)
            }
            3 => {
                self.io.write(REG_CONTROL, 1);
                response[0] = 0;
                Ok(1)
            }
            4 => {
                self.io.write(REG_CONTROL, 2);
                response[0] = 0;
                Ok(1)
            }
            5 => {
                self.io.write(REG_CONTROL, 4);
                response[0] = 0;
                Ok(1)
            }
            6 if response.len() >= 36 => {
                for (index, offset) in [0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x60, 0x64]
                    .iter()
                    .enumerate()
                {
                    response[index * 4..index * 4 + 4]
                        .copy_from_slice(&self.io.read(*offset).to_le_bytes());
                }
                Ok(36)
            }
            7 if request.len() >= 7 => {
                let offset =
                    u32::from_le_bytes([request[3], request[4], request[5], request[6]]) as usize;
                let data = &request[7..];
                let end = offset.checked_add(data.len()).ok_or(())?;
                if end > M3_BRAM_SIZE {
                    return Err(());
                }
                self.io.write_m3_bram(offset, data);
                response[0] = 0;
                Ok(1)
            }
            8 if request.len() == 9 => {
                let offset =
                    u32::from_le_bytes([request[3], request[4], request[5], request[6]]) as usize;
                let length = u16::from_le_bytes([request[7], request[8]]) as usize;
                let end = offset.checked_add(length).ok_or(())?;
                if end > M3_BRAM_SIZE || response.len() < length {
                    return Err(());
                }
                self.io.read_m3_bram(offset, &mut response[..length]);
                Ok(length)
            }
            9 if request.len() == 4 => {
                self.io.write(REG_M3_CONTROL, request[3] as u32);
                response[0] = 0;
                Ok(1)
            }
            _ => Err(()),
        }
    }

    /// Move one unchanged CMSIS-DAP payload through the PL command engine.
    /// `poll_budget` bounds each mailbox wait so a wedged PL cannot stall the
    /// TCP 3240 service forever.
    pub fn dap_exchange(
        &mut self,
        request: &[u8],
        response: &mut [u8],
        poll_budget: u32,
    ) -> Result<usize, DapError> {
        if request.is_empty() {
            return Err(DapError::EmptyRequest);
        }
        for (index, byte) in request.iter().enumerate() {
            let mut remaining = poll_budget;
            while self.io.read(REG_DAP_STATUS) & 1 == 0 {
                if remaining == 0 {
                    return Err(DapError::Timeout);
                }
                remaining -= 1;
            }
            self.io.write(
                REG_DAP_COMMAND,
                *byte as u32 | (((index + 1 == request.len()) as u32) << 8),
            );
        }
        let mut length = 0usize;
        loop {
            let mut remaining = poll_budget;
            while self.io.read(REG_DAP_STATUS) & 2 == 0 {
                if remaining == 0 {
                    return Err(DapError::Timeout);
                }
                remaining -= 1;
            }
            let word = self.io.read(REG_DAP_RESPONSE);
            if length == response.len() {
                return Err(DapError::ResponseTooLarge);
            }
            response[length] = word as u8;
            length += 1;
            if word & (1 << 9) != 0 {
                return Ok(length);
            }
        }
    }

    /// Process exactly one TCP 3240 frame, preserving the CMSIS-DAP payload
    /// and adding only the board-specific little-endian response length.
    pub fn dap_frame(
        &mut self,
        framed_request: &[u8],
        framed_response: &mut [u8],
        poll_budget: u32,
    ) -> Result<usize, DapError> {
        if framed_request.len() < 4 || framed_response.len() < 4 {
            return Err(DapError::BadFrame);
        }
        let payload_length = u32::from_le_bytes([
            framed_request[0],
            framed_request[1],
            framed_request[2],
            framed_request[3],
        ]) as usize;
        if payload_length > 1024 {
            return Err(DapError::PacketTooLarge);
        }
        if payload_length + 4 != framed_request.len() {
            return Err(DapError::BadFrame);
        }
        let response_length =
            self.dap_exchange(&framed_request[4..], &mut framed_response[4..], poll_budget)?;
        framed_response[..4].copy_from_slice(&(response_length as u32).to_le_bytes());
        Ok(response_length + 4)
    }
}

pub const S2MM_DMACR: usize = 0x30;
pub const S2MM_DMASR: usize = 0x34;
pub const S2MM_CURDESC_LO: usize = 0x38;
pub const S2MM_CURDESC_HI: usize = 0x3c;
pub const S2MM_TAILDESC_LO: usize = 0x40;
pub const S2MM_TAILDESC_HI: usize = 0x44;
pub const DMA_CR_RUN_STOP: u32 = 1 << 0;
pub const DMA_CR_RESET: u32 = 1 << 2;
pub const DMA_CR_IOC_IRQ_ENABLE: u32 = 1 << 12;
pub const DMA_CR_ERROR_IRQ_ENABLE: u32 = 1 << 14;
pub const DMA_SR_IRQ_MASK: u32 = 0x7000;

pub trait DmaRegisterIo {
    fn read_dma(&self, offset: usize) -> u32;
    fn write_dma(&mut self, offset: usize, value: u32);
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DmaError {
    ResetTimeout,
}

/// AXI DMA S2MM scatter/gather register programming. Descriptor ownership and
/// completion parsing live in `orbtrace_firmware_common::AxiDmaRing`.
pub struct S2mmDma<IO> {
    io: IO,
}

impl<IO: DmaRegisterIo> S2mmDma<IO> {
    pub const fn new(io: IO) -> Self {
        Self { io }
    }

    pub fn initialize(
        &mut self,
        current_descriptor: u64,
        poll_budget: u32,
    ) -> Result<(), DmaError> {
        self.io.write_dma(S2MM_DMACR, DMA_CR_RESET);
        let mut remaining = poll_budget;
        while self.io.read_dma(S2MM_DMACR) & DMA_CR_RESET != 0 {
            if remaining == 0 {
                return Err(DmaError::ResetTimeout);
            }
            remaining -= 1;
        }
        self.io
            .write_dma(S2MM_CURDESC_LO, current_descriptor as u32);
        self.io
            .write_dma(S2MM_CURDESC_HI, (current_descriptor >> 32) as u32);
        self.io.write_dma(
            S2MM_DMACR,
            DMA_CR_RUN_STOP | DMA_CR_IOC_IRQ_ENABLE | DMA_CR_ERROR_IRQ_ENABLE,
        );
        Ok(())
    }

    pub fn submit_tail(&mut self, descriptor: u64) {
        self.io
            .write_dma(S2MM_TAILDESC_HI, (descriptor >> 32) as u32);
        self.io.write_dma(S2MM_TAILDESC_LO, descriptor as u32);
    }

    pub fn acknowledge_interrupts(&mut self) -> u32 {
        let status = self.io.read_dma(S2MM_DMASR);
        self.io.write_dma(S2MM_DMASR, status & DMA_SR_IRQ_MASK);
        status
    }
}

// Narrow ABI owned by the existing ThreadX/NetX Duo GEM integration. Rust
// owns framing and ring state; C owns only packet allocation and transmission.
unsafe extern "C" {
    pub fn orb_net_listen(port: u16) -> i32;
    pub fn orb_net_recv(socket: i32, data: *mut u8, capacity: usize) -> isize;
    pub fn orb_net_send(socket: i32, data: *const u8, length: usize) -> isize;
    pub fn orb_cache_flush(address: *const u8, length: usize);
    pub fn orb_cache_invalidate(address: *mut u8, length: usize);
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use core::cell::Cell;
    use std::vec::Vec;

    struct Mailbox {
        writes: Vec<u32>,
        response: [u32; 3],
        response_index: Cell<usize>,
    }
    impl RegisterIo for Mailbox {
        fn read(&self, offset: usize) -> u32 {
            match offset {
                REG_DAP_STATUS => {
                    if self.writes.last().map(|w| w & 0x100 != 0).unwrap_or(false) {
                        3
                    } else {
                        1
                    }
                }
                REG_DAP_RESPONSE => {
                    let index = self.response_index.get();
                    self.response_index.set(index + 1);
                    self.response[index]
                }
                _ => 0,
            }
        }
        fn write(&mut self, offset: usize, value: u32) {
            if offset == REG_DAP_COMMAND {
                self.writes.push(value);
            }
        }
    }

    #[test]
    fn dap_payload_uses_mailbox_without_reframing() {
        let io = Mailbox {
            writes: Vec::new(),
            response: [5 | (1 << 8), 1 | (1 << 8), 1 | (1 << 8) | (1 << 9)],
            response_index: Cell::new(0),
        };
        let mut controller = Controller::new(io);
        let mut response = [0; 8];
        assert_eq!(
            controller.dap_exchange(&[5, 0, 0], &mut response, 10),
            Ok(3)
        );
        assert_eq!(&response[..3], &[5, 1, 1]);
        assert_eq!(controller.io.writes, [5, 0, 0x100]);
    }

    #[test]
    fn dap_tcp_frame_has_only_little_endian_length_wrapper() {
        let io = Mailbox {
            writes: Vec::new(),
            response: [5 | (1 << 8), 1 | (1 << 8), 1 | (1 << 8) | (1 << 9)],
            response_index: Cell::new(0),
        };
        let mut controller = Controller::new(io);
        let mut output = [0; 16];
        assert_eq!(
            controller.dap_frame(&[3, 0, 0, 0, 5, 0, 0], &mut output, 10),
            Ok(7)
        );
        assert_eq!(&output[..7], &[3, 0, 0, 0, 5, 1, 1]);
        assert_eq!(
            controller.dap_frame(&[2, 0, 0, 0, 5], &mut output, 10),
            Err(DapError::BadFrame)
        );
    }

    #[derive(Default)]
    struct ServiceIo(Vec<(usize, u32)>);
    impl RegisterIo for ServiceIo {
        fn read(&self, _offset: usize) -> u32 {
            0
        }
        fn write(&mut self, offset: usize, value: u32) {
            self.0.push((offset, value));
        }
    }

    #[test]
    fn control_connection_handles_fragmentation_and_pipelining() {
        let mut controller = Controller::new(ServiceIo::default());
        let mut connection = ControlConnection::new();
        let mut response = [0; 64];
        let requests = [
            3, 0, 0, 0, 1, 0, 1, // GetInfo
            3, 0, 0, 0, 1, 0, 3, // Start
        ];
        assert_eq!(
            connection.feed(&requests[..2], &mut controller, &mut response),
            Ok(ServiceReply {
                consumed: 2,
                response_length: None
            })
        );
        let first = connection
            .feed(&requests[2..], &mut controller, &mut response)
            .unwrap();
        assert_eq!(first.consumed, 5);
        assert_eq!(
            &response[4..first.response_length.unwrap()],
            b"ZUBoard-Orbtrace/1"
        );
        let second_start = 2 + first.consumed;
        let second = connection
            .feed(&requests[second_start..], &mut controller, &mut response)
            .unwrap();
        assert_eq!(second.response_length, Some(5));
        assert_eq!(&response[..5], &[1, 0, 0, 0, 0]);
        assert_eq!(controller.io.0, [(REG_CONTROL, 1)]);
    }

    #[test]
    fn dap_connection_preserves_fragmented_payload() {
        let io = Mailbox {
            writes: Vec::new(),
            response: [5 | (1 << 8), 1 | (1 << 8), 1 | (1 << 8) | (1 << 9)],
            response_index: Cell::new(0),
        };
        let mut controller = Controller::new(io);
        let mut connection = DapConnection::new();
        let mut output = [0; 16];
        assert_eq!(
            connection.feed(&[3, 0, 0], &mut controller, &mut output, 10),
            Ok(ServiceReply {
                consumed: 3,
                response_length: None
            })
        );
        assert_eq!(
            connection.feed(&[0, 5, 0, 0], &mut controller, &mut output, 10),
            Ok(ServiceReply {
                consumed: 4,
                response_length: Some(7)
            })
        );
        assert_eq!(&output[..7], &[3, 0, 0, 0, 5, 1, 1]);
    }

    #[derive(Default)]
    struct M3Io {
        bram: Vec<u8>,
        reg_writes: Vec<(usize, u32)>,
    }
    impl RegisterIo for M3Io {
        fn read(&self, _offset: usize) -> u32 {
            0
        }
        fn write(&mut self, offset: usize, value: u32) {
            self.reg_writes.push((offset, value));
        }
        fn write_m3_bram(&mut self, offset: usize, data: &[u8]) {
            if self.bram.len() < offset + data.len() {
                self.bram.resize(offset + data.len(), 0);
            }
            self.bram[offset..offset + data.len()].copy_from_slice(data);
        }
        fn read_m3_bram(&self, offset: usize, out: &mut [u8]) {
            for (index, byte) in out.iter_mut().enumerate() {
                *byte = self.bram.get(offset + index).copied().unwrap_or(0);
            }
        }
    }

    #[test]
    fn m3_load_chunk_writes_and_reads_back() {
        let mut controller = Controller::new(M3Io::default());
        let mut response = [0; 64];
        // [version_lo, version_hi, opcode=7, offset:u32 LE, data...]
        let load = [1, 0, 7, 4, 0, 0, 0, 0xaa, 0xbb, 0xcc, 0xdd];
        assert_eq!(controller.command(&load, &mut response), Ok(1));
        assert_eq!(&controller.io.bram[4..8], [0xaa, 0xbb, 0xcc, 0xdd]);

        // [version_lo, version_hi, opcode=8, offset:u32 LE, length:u16 LE]
        // Read back the same offset (4) written above.
        let read = [1, 0, 8, 4, 0, 0, 0, 4, 0];
        assert_eq!(controller.command(&read, &mut response), Ok(4));
        assert_eq!(&response[..4], [0xaa, 0xbb, 0xcc, 0xdd]);
    }

    #[test]
    fn m3_load_chunk_rejects_out_of_range_offset() {
        let mut controller = Controller::new(M3Io::default());
        let mut response = [0; 8];
        let offset = (M3_BRAM_SIZE as u32).to_le_bytes();
        let request = [1, 0, 7, offset[0], offset[1], offset[2], offset[3], 0xaa];
        assert_eq!(controller.command(&request, &mut response), Err(()));
    }

    #[test]
    fn m3_control_writes_the_control_register() {
        let mut controller = Controller::new(M3Io::default());
        let mut response = [0; 8];
        // [version_lo, version_hi, opcode=9, bits]
        assert_eq!(controller.command(&[1, 0, 9, 1], &mut response), Ok(1));
        assert_eq!(controller.io.reg_writes, [(REG_M3_CONTROL, 1)]);
    }

    struct DmaIo {
        reset_stuck: bool,
        writes: Vec<(usize, u32)>,
        status: u32,
    }
    impl DmaRegisterIo for DmaIo {
        fn read_dma(&self, offset: usize) -> u32 {
            match offset {
                S2MM_DMACR if self.reset_stuck => DMA_CR_RESET,
                S2MM_DMASR => self.status,
                _ => 0,
            }
        }
        fn write_dma(&mut self, offset: usize, value: u32) {
            self.writes.push((offset, value));
        }
    }

    #[test]
    fn s2mm_programs_64_bit_descriptor_addresses_and_irqs() {
        let io = DmaIo {
            reset_stuck: false,
            writes: Vec::new(),
            status: 0x5000,
        };
        let mut dma = S2mmDma::new(io);
        assert_eq!(dma.initialize(0x1_2345_6000, 4), Ok(()));
        dma.submit_tail(0x2_3456_7000);
        assert_eq!(dma.acknowledge_interrupts(), 0x5000);
        assert_eq!(
            dma.io.writes,
            [
                (S2MM_DMACR, DMA_CR_RESET),
                (S2MM_CURDESC_LO, 0x2345_6000),
                (S2MM_CURDESC_HI, 1),
                (
                    S2MM_DMACR,
                    DMA_CR_RUN_STOP | DMA_CR_IOC_IRQ_ENABLE | DMA_CR_ERROR_IRQ_ENABLE
                ),
                (S2MM_TAILDESC_HI, 2),
                (S2MM_TAILDESC_LO, 0x3456_7000),
                (S2MM_DMASR, 0x5000),
            ]
        );
    }

    #[test]
    fn s2mm_reset_is_bounded() {
        let io = DmaIo {
            reset_stuck: true,
            writes: Vec::new(),
            status: 0,
        };
        let mut dma = S2mmDma::new(io);
        assert_eq!(dma.initialize(0x1000, 2), Err(DmaError::ResetTimeout));
    }

    #[test]
    fn pl_dma_ring_configuration_is_64_bit() {
        let mut controller = Controller::new(ServiceIo::default());
        controller.configure_dma_ring(0x1_8000_0000, 256);
        assert_eq!(
            controller.io.0,
            [
                (REG_DMA_BASE_LO, 0x8000_0000),
                (REG_DMA_BASE_HI, 1),
                (REG_DMA_RING_SIZE, 256),
            ]
        );
    }

    struct CoreSightWrites(Vec<(usize, u32)>);
    impl coresight::Mmio for CoreSightWrites {
        unsafe fn write32(&mut self, address: usize, value: u32) {
            self.0.push((address, value));
        }
    }

    #[test]
    fn coresight_routes_r5_and_a53_1_through_distinct_funnels() {
        let mut r5 = CoreSightWrites(Vec::new());
        // SAFETY: this test records accesses without touching MMIO.
        unsafe {
            coresight::select(&mut r5, false);
        }
        assert_eq!(
            r5.0[0],
            (
                coresight::R5_0_ETM + coresight::LAR,
                coresight::CORESIGHT_UNLOCK
            )
        );
        assert_eq!(
            r5.0[5],
            (coresight::FUNNEL_RPU + coresight::FUNNEL_CONTROL, 1)
        );
        assert_eq!(
            r5.0[7],
            (coresight::FUNNEL_SYSTEM + coresight::FUNNEL_CONTROL, 1)
        );

        let mut a53 = CoreSightWrites(Vec::new());
        // SAFETY: this test records accesses without touching MMIO.
        unsafe {
            coresight::select(&mut a53, true);
        }
        assert_eq!(
            a53.0[0],
            (
                coresight::A53_1_ETM + coresight::LAR,
                coresight::CORESIGHT_UNLOCK
            )
        );
        assert_eq!(
            a53.0[6],
            (coresight::FUNNEL_APU + coresight::FUNNEL_CONTROL, 2)
        );
        assert_eq!(
            a53.0[7],
            (coresight::FUNNEL_SYSTEM + coresight::FUNNEL_CONTROL, 2)
        );
    }
}

pub mod coresight {
    // ZynqMP CoreSight addresses are intentionally centralized. Firmware must
    // unlock components and stop an ETM before changing its configuration.
    pub const CORESIGHT_UNLOCK: u32 = 0xc5ac_ce55;
    // ZynqMP internal CoreSight system map (UG1085, Figure 39-8).
    pub const R5_0_ETM: usize = 0xfe3f_c000;
    pub const A53_1_ETM: usize = 0xfe54_0000;
    pub const FUNNEL_RPU: usize = 0xfe11_0000;
    pub const FUNNEL_APU: usize = 0xfe12_0000;
    pub const FUNNEL_SYSTEM: usize = 0xfe13_0000;
    pub const TPIU: usize = 0xfe18_0000;
    pub const LAR: usize = 0xfb0;
    pub const FUNNEL_CONTROL: usize = 0x000;

    pub trait Mmio {
        unsafe fn write32(&mut self, address: usize, value: u32);
    }
    pub unsafe fn select<M: Mmio>(mmio: &mut M, a53_1: bool) {
        let etm = if a53_1 { A53_1_ETM } else { R5_0_ETM };
        // SAFETY: caller guarantees the fixed CoreSight range is mapped device-nGnRE.
        unsafe {
            mmio.write32(etm + LAR, CORESIGHT_UNLOCK);
            mmio.write32(FUNNEL_RPU + LAR, CORESIGHT_UNLOCK);
            mmio.write32(FUNNEL_APU + LAR, CORESIGHT_UNLOCK);
            mmio.write32(FUNNEL_SYSTEM + LAR, CORESIGHT_UNLOCK);
            mmio.write32(TPIU + LAR, CORESIGHT_UNLOCK);
            // Input zero is R5-0 on the RPU funnel; input one is A53-1 on the
            // APU funnel. Funnel 2 receives those funnels on inputs 0 and 1.
            mmio.write32(FUNNEL_RPU + FUNNEL_CONTROL, if a53_1 { 0 } else { 1 });
            mmio.write32(FUNNEL_APU + FUNNEL_CONTROL, if a53_1 { 2 } else { 0 });
            mmio.write32(FUNNEL_SYSTEM + FUNNEL_CONTROL, if a53_1 { 2 } else { 1 });
        }
    }
}
