//! Byte-exact software reference for the ZUBoard Orbtrace data plane.
//!
//! This crate deliberately has no target or async dependencies, so the same
//! model can be used by host tests and by `no_std` firmware after replacing
//! the allocation boundary.

use std::fmt::{self, Write as _};

pub const CONTROL_PORT: u16 = 3401;
pub const ORBFLOW_PORT: u16 = 3402;
pub const CMSIS_DAP_PORT: u16 = 3240;
pub const PROTOCOL_VERSION: u16 = 1;
pub const MAX_CONTROL_PAYLOAD: usize = 4096;
pub const MAX_DAP_PACKET: usize = 1024;
/// Size of the M3's own BRAM window (`sdk/bsp/m3/memory.lds`'s `RAM` region).
pub const M3_BRAM_SIZE: usize = 0x1_0000;
/// Per-`LoadM3Chunk` payload cap: comfortably under `MAX_CONTROL_PAYLOAD`
/// once the opcode/version/offset header (7 bytes) is included.
pub const M3_BRAM_CHUNK: usize = 2048;

pub mod registers {
    pub const ID: u32 = 0x0000;
    pub const VERSION: u32 = 0x0004;
    pub const CONTROL: u32 = 0x0008;
    pub const SOURCE_FORMAT: u32 = 0x000c;
    pub const SWO_BAUD: u32 = 0x0010;
    pub const DMA_BASE_LO: u32 = 0x0018;
    pub const DMA_BASE_HI: u32 = 0x001c;
    pub const DMA_RING_SIZE: u32 = 0x0020;
    pub const IRQ_STATUS: u32 = 0x0024;
    pub const IRQ_ENABLE: u32 = 0x0028;
    pub const RX_BYTES_LO: u32 = 0x0040;
    pub const RX_BYTES_HI: u32 = 0x0044;
    pub const DROP_BYTES_LO: u32 = 0x0048;
    pub const DROP_BYTES_HI: u32 = 0x004c;
    pub const SYNC_LOSS_LO: u32 = 0x0050;
    pub const SYNC_LOSS_HI: u32 = 0x0054;
    pub const FIFO_HIGH_WATER: u32 = 0x0058;
    pub const DMA_FAULTS_LO: u32 = 0x0060;
    pub const DMA_FAULTS_HI: u32 = 0x0064;
    pub const DAP_COMMAND: u32 = 0x0080;
    pub const DAP_RESPONSE: u32 = 0x0084;
    pub const DAP_STATUS: u32 = 0x0088;
    pub const DAP_CONTROL: u32 = 0x008c;
    pub const DAP_TRANSFERS_LO: u32 = 0x0090;
    pub const DAP_TRANSFERS_HI: u32 = 0x0094;
    pub const DAP_ABORTS: u32 = 0x0098;
    pub const M3_CONTROL: u32 = 0x00a0;

    pub const ID_VALUE: u32 = 0x4f52_4254; // "ORBT"
    pub const M3_CONTROL_RELEASE: u32 = 1 << 0;
    pub const M3_CONTROL_DAP_REAL: u32 = 1 << 1;
    pub const CONTROL_START: u32 = 1 << 0;
    pub const CONTROL_STOP: u32 = 1 << 1;
    pub const CONTROL_RESET: u32 = 1 << 2;
    pub const IRQ_DMA_COMPLETE: u32 = 1 << 0;
    pub const IRQ_OVERRUN: u32 = 1 << 1;
    pub const IRQ_DEBUG_COMPLETE: u32 = 1 << 2;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum Source {
    CortexM3 = 0,
    R5 = 1,
    A53 = 2,
    Test = 3,
}

impl TryFrom<u8> for Source {
    type Error = ProtocolError;
    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::CortexM3),
            1 => Ok(Self::R5),
            2 => Ok(Self::A53),
            3 => Ok(Self::Test),
            _ => Err(ProtocolError::InvalidValue("source")),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum TraceFormat {
    Tpiu1 = 0,
    Tpiu2 = 1,
    Tpiu4 = 2,
    SwoNrz = 3,
    SwoManchester = 4,
}

impl TryFrom<u8> for TraceFormat {
    type Error = ProtocolError;
    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Tpiu1),
            1 => Ok(Self::Tpiu2),
            2 => Ok(Self::Tpiu4),
            3 => Ok(Self::SwoNrz),
            4 => Ok(Self::SwoManchester),
            _ => Err(ProtocolError::InvalidValue("format")),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Stats {
    pub rx_bytes: u64,
    pub dropped_bytes: u64,
    pub sync_loss: u64,
    pub fifo_high_water: u32,
    pub dma_faults: u64,
}

#[derive(Debug, Eq, PartialEq)]
pub enum ProtocolError {
    Short,
    BadVersion(u16),
    UnknownCommand(u8),
    InvalidValue(&'static str),
    TooLarge(usize),
    BadCobs,
    BadChecksum,
}

impl fmt::Display for ProtocolError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self:?}")
    }
}

impl std::error::Error for ProtocolError {}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Command {
    GetInfo,
    Configure {
        source: Source,
        format: TraceFormat,
        swo_baud: u32,
    },
    Start,
    Stop,
    Reset,
    GetStats,
    /// Write `data` (at most `M3_BRAM_CHUNK` bytes) into the M3's BRAM at
    /// byte `offset`. The D2 load path (see M3_TRACE_VERIFICATION_PLAN.md):
    /// streams the image over TCP instead of the JTAG-DAP `mwr`/`dow` path,
    /// which does not reliably reach this BRAM (see load_m3.tcl's history).
    LoadM3Chunk {
        offset: u32,
        data: Vec<u8>,
    },
    /// Read back `length` bytes from the M3's BRAM at byte `offset`, to
    /// verify a load before releasing the core -- mirrors load_m3.tcl's own
    /// vector-word readback check.
    ReadM3Bram {
        offset: u32,
        length: u16,
    },
    /// Raw write to `registers::M3_CONTROL` (hold/release reset, and later
    /// the Phase G real-DAP-route bit).
    M3Control {
        bits: u8,
    },
}

impl Command {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::from(PROTOCOL_VERSION.to_le_bytes());
        match self {
            Self::GetInfo => out.push(1),
            Self::Configure {
                source,
                format,
                swo_baud,
            } => {
                out.extend([2, *source as u8, *format as u8]);
                out.extend(swo_baud.to_le_bytes());
            }
            Self::Start => out.push(3),
            Self::Stop => out.push(4),
            Self::Reset => out.push(5),
            Self::GetStats => out.push(6),
            Self::LoadM3Chunk { offset, data } => {
                out.push(7);
                out.extend(offset.to_le_bytes());
                out.extend(data);
            }
            Self::ReadM3Bram { offset, length } => {
                out.push(8);
                out.extend(offset.to_le_bytes());
                out.extend(length.to_le_bytes());
            }
            Self::M3Control { bits } => out.extend([9, *bits]),
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, ProtocolError> {
        if bytes.len() < 3 {
            return Err(ProtocolError::Short);
        }
        let version = u16::from_le_bytes([bytes[0], bytes[1]]);
        if version != PROTOCOL_VERSION {
            return Err(ProtocolError::BadVersion(version));
        }
        match bytes[2] {
            1 => Ok(Self::GetInfo),
            2 if bytes.len() == 9 => Ok(Self::Configure {
                source: bytes[3].try_into()?,
                format: bytes[4].try_into()?,
                swo_baud: u32::from_le_bytes(bytes[5..9].try_into().unwrap()),
            }),
            2 => Err(ProtocolError::Short),
            3 => Ok(Self::Start),
            4 => Ok(Self::Stop),
            5 => Ok(Self::Reset),
            6 => Ok(Self::GetStats),
            7 if bytes.len() >= 7 => Ok(Self::LoadM3Chunk {
                offset: u32::from_le_bytes(bytes[3..7].try_into().unwrap()),
                data: bytes[7..].to_vec(),
            }),
            7 => Err(ProtocolError::Short),
            8 if bytes.len() == 9 => Ok(Self::ReadM3Bram {
                offset: u32::from_le_bytes(bytes[3..7].try_into().unwrap()),
                length: u16::from_le_bytes(bytes[7..9].try_into().unwrap()),
            }),
            8 => Err(ProtocolError::Short),
            9 if bytes.len() == 4 => Ok(Self::M3Control { bits: bytes[3] }),
            9 => Err(ProtocolError::Short),
            other => Err(ProtocolError::UnknownCommand(other)),
        }
    }
}

/// Prefix a control or CMSIS-DAP payload with its little-endian length.
pub fn length_frame(payload: &[u8], maximum: usize) -> Result<Vec<u8>, ProtocolError> {
    if payload.len() > maximum {
        return Err(ProtocolError::TooLarge(payload.len()));
    }
    let mut framed = Vec::with_capacity(payload.len() + 4);
    framed.extend((payload.len() as u32).to_le_bytes());
    framed.extend(payload);
    Ok(framed)
}

/// Decode one length-prefixed item, returning it and the number of bytes used.
pub fn length_unframe(bytes: &[u8], maximum: usize) -> Result<(&[u8], usize), ProtocolError> {
    if bytes.len() < 4 {
        return Err(ProtocolError::Short);
    }
    let len = u32::from_le_bytes(bytes[..4].try_into().unwrap()) as usize;
    if len > maximum {
        return Err(ProtocolError::TooLarge(len));
    }
    if bytes.len() < len + 4 {
        return Err(ProtocolError::Short);
    }
    Ok((&bytes[4..4 + len], len + 4))
}

/// Standard Consistent Overhead Byte Stuffing (COBS), without delimiter.
pub fn cobs_encode(input: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(input.len() + input.len() / 254 + 1);
    let mut code_index = 0;
    out.push(0);
    let mut code = 1u8;
    for &byte in input {
        if byte == 0 {
            out[code_index] = code;
            code_index = out.len();
            out.push(0);
            code = 1;
        } else {
            out.push(byte);
            code = code.wrapping_add(1);
            if code == 0xff {
                out[code_index] = code;
                code_index = out.len();
                out.push(0);
                code = 1;
            }
        }
    }
    out[code_index] = code;
    out
}

pub fn cobs_decode(input: &[u8]) -> Result<Vec<u8>, ProtocolError> {
    let mut out = Vec::with_capacity(input.len());
    let mut pos = 0;
    while pos < input.len() {
        let code = input[pos] as usize;
        if code == 0 || pos + code > input.len() + 1 {
            return Err(ProtocolError::BadCobs);
        }
        pos += 1;
        for _ in 1..code {
            if pos >= input.len() {
                return Err(ProtocolError::BadCobs);
            }
            out.push(input[pos]);
            pos += 1;
        }
        if code != 0xff && pos < input.len() {
            out.push(0);
        }
    }
    Ok(out)
}

/// Orbtrace checksum: append the wrapping two's complement of all bytes.
pub fn append_checksum(packet: &mut Vec<u8>) {
    let sum = packet.iter().fold(0u8, |sum, byte| sum.wrapping_add(*byte));
    packet.push(0u8.wrapping_sub(sum));
}

pub fn verify_checksum(packet: &[u8]) -> bool {
    !packet.is_empty() && packet.iter().fold(0u8, |sum, b| sum.wrapping_add(*b)) == 0
}

/// Channel byte + payload + checksum, COBS encoded, then zero delimited.
pub fn orbflow_frame(channel: u8, payload: &[u8]) -> Result<Vec<u8>, ProtocolError> {
    if channel > 0x7f {
        return Err(ProtocolError::InvalidValue("channel"));
    }
    let mut raw = Vec::with_capacity(payload.len() + 2);
    raw.push(channel);
    raw.extend(payload);
    append_checksum(&mut raw);
    let mut encoded = cobs_encode(&raw);
    encoded.push(0);
    Ok(encoded)
}

pub fn orbflow_unframe(frame: &[u8]) -> Result<(u8, Vec<u8>), ProtocolError> {
    let encoded = frame.strip_suffix(&[0]).unwrap_or(frame);
    let raw = cobs_decode(encoded)?;
    if raw.len() < 2 || !verify_checksum(&raw) {
        return Err(ProtocolError::BadChecksum);
    }
    Ok((raw[0], raw[1..raw.len() - 1].to_vec()))
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MuxedByte {
    pub channel: u8,
    pub data: u8,
}

/// Recover 16-byte TPIU formatter frames and apply the formatter-ID rules.
#[derive(Default)]
pub struct TpiuDecoder {
    sync_window: u32,
    synced: bool,
    frame: Vec<u8>,
    channel: u8,
    delayed_channel: Option<u8>,
    pub sync_losses: u64,
}

impl TpiuDecoder {
    pub fn reset_sync(&mut self) {
        if self.synced {
            self.sync_losses += 1;
        }
        self.synced = false;
        self.frame.clear();
    }

    pub fn push(&mut self, byte: u8) -> Vec<MuxedByte> {
        self.sync_window = (self.sync_window >> 8) | ((byte as u32) << 24);
        if !self.synced {
            if self.sync_window == 0xffff_ff7f {
                self.synced = true;
                self.frame.clear();
            }
            return Vec::new();
        }
        self.frame.push(byte);
        if self.frame.len() != 16 {
            return Vec::new();
        }
        let raw = std::mem::take(&mut self.frame);
        let mut out = Vec::new();
        for index in 0..15 {
            let is_id = index % 2 == 0 && raw[index] & 1 != 0;
            let data = if index % 2 == 0 {
                (raw[index] & 0xfe) | ((raw[15] >> (index / 2)) & 1)
            } else {
                raw[index]
            };
            if is_id {
                let id = data >> 1;
                if data & 1 != 0 {
                    self.delayed_channel = Some(id);
                } else {
                    self.channel = id;
                }
            } else {
                if self.channel != 0 {
                    out.push(MuxedByte {
                        channel: self.channel,
                        data,
                    });
                }
                if let Some(next) = self.delayed_channel.take() {
                    self.channel = next;
                }
            }
        }
        out
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SwoError {
    BadStart,
    BadStop,
    BadManchester,
}

/// Pack chronological DDR edge samples from 1, 2, or 4 trace lanes. Lane zero
/// is the least-significant bit and the first edge occupies the low bits.
pub fn decode_parallel_ddr(samples: &[u8], width: u8) -> Result<Vec<u8>, ProtocolError> {
    if !matches!(width, 1 | 2 | 4) {
        return Err(ProtocolError::InvalidValue("trace width"));
    }
    let edges_per_byte = 8 / width as usize;
    let mask = (1u8 << width) - 1;
    let mut output = Vec::with_capacity(samples.len() / edges_per_byte);
    for edges in samples.chunks_exact(edges_per_byte) {
        let mut byte = 0u8;
        for (index, sample) in edges.iter().enumerate() {
            byte |= (sample & mask) << (index * width as usize);
        }
        output.push(byte);
    }
    Ok(output)
}

/// Decode one oversampled 8-N-1 NRZ UART word. Samples are centered by the caller.
pub fn decode_swo_nrz(samples: &[bool], samples_per_bit: usize) -> Result<u8, SwoError> {
    if samples_per_bit == 0 || samples.len() < samples_per_bit * 10 {
        return Err(SwoError::BadStop);
    }
    let sample = |bit: usize| samples[bit * samples_per_bit + samples_per_bit / 2];
    if sample(0) {
        return Err(SwoError::BadStart);
    }
    if !sample(9) {
        return Err(SwoError::BadStop);
    }
    let mut value = 0;
    for bit in 0..8 {
        value |= (sample(bit + 1) as u8) << bit;
    }
    Ok(value)
}

/// Decode a centered Manchester 8-N-1 word. Each logical bit is represented
/// by two half-bit samples: low/high is zero and high/low is one.
pub fn decode_swo_manchester(half_bits: &[bool]) -> Result<u8, SwoError> {
    if half_bits.len() < 20 {
        return Err(SwoError::BadStop);
    }
    let bit = |index: usize| -> Result<bool, SwoError> {
        let first = half_bits[index * 2];
        let second = half_bits[index * 2 + 1];
        if first == second {
            Err(SwoError::BadManchester)
        } else {
            Ok(first)
        }
    };
    if bit(0)? {
        return Err(SwoError::BadStart);
    }
    if !bit(9)? {
        return Err(SwoError::BadStop);
    }
    let mut value = 0u8;
    for index in 0..8 {
        value |= (bit(index + 1)? as u8) << index;
    }
    Ok(value)
}

/// Deterministic synthetic SW-DP/AP sufficient to exercise Arm DAP semantics.
#[derive(Default)]
pub struct SyntheticDap {
    pub aborts: u32,
    pub transfers: u64,
    wait_once: bool,
    fault_once: bool,
    parity_error_once: bool,
}

impl SyntheticDap {
    pub fn set_wait_once(&mut self) {
        self.wait_once = true;
    }

    pub fn set_fault_once(&mut self) {
        self.fault_once = true;
    }

    pub fn set_parity_error_once(&mut self) {
        self.parity_error_once = true;
    }

    pub fn execute(&mut self, request: &[u8]) -> Result<Vec<u8>, ProtocolError> {
        if request.is_empty() || request.len() > MAX_DAP_PACKET {
            return Err(ProtocolError::Short);
        }
        match request[0] {
            0x00 => {
                // DAP_Info
                let id = request.get(1).copied().unwrap_or(0);
                let value: &[u8] = match id {
                    0x01 => b"Orbcode",
                    0x02 => b"ZUBoard Orbtrace",
                    _ => b"",
                };
                let mut out = vec![0x00, value.len() as u8];
                out.extend(value);
                Ok(out)
            }
            0x02 => Ok(vec![0x02, request.get(1).copied().unwrap_or(0) & 0x03]),
            0x05 => {
                // DAP_Transfer: deterministic read data = request word xor address tag
                if request.len() < 3 {
                    return Err(ProtocolError::Short);
                }
                let count = request[2] as usize;
                if self.wait_once {
                    self.wait_once = false;
                    return Ok(vec![0x05, 0, 0x02]);
                }
                if self.fault_once {
                    self.fault_once = false;
                    return Ok(vec![0x05, 0, 0x04]);
                }
                let status = if self.parity_error_once {
                    self.parity_error_once = false;
                    0x09
                } else {
                    0x01
                };
                let mut out = vec![0x05, 0, status];
                let mut request_pos = 3usize;
                let mut completed = 0u8;
                for _ in 0..count {
                    let Some(&transfer) = request.get(request_pos) else {
                        out[1] = completed;
                        out[2] = 0x04;
                        return Ok(out);
                    };
                    request_pos += 1;
                    self.transfers += 1;
                    if transfer & 0x02 != 0 {
                        out.extend((0xa5a5_0000u32 | transfer as u32).to_le_bytes());
                    } else if request_pos + 4 <= request.len() {
                        request_pos += 4;
                    } else {
                        out[1] = completed;
                        out[2] = 0x04;
                        return Ok(out);
                    }
                    completed += 1;
                }
                out[1] = completed;
                Ok(out)
            }
            0x07 | 0x08 => {
                self.aborts += 1;
                Ok(vec![request[0], 0])
            }
            0x10 => Ok(vec![0x10, request.get(1).copied().unwrap_or(0)]),
            0x14 => Ok(vec![0x14, 0, request.get(3).copied().unwrap_or(0) ^ 1]),
            command => Ok(vec![command, 0xff]),
        }
    }
}

pub fn stats_from_le(bytes: &[u8]) -> Result<Stats, ProtocolError> {
    if bytes.len() < 36 {
        return Err(ProtocolError::Short);
    }
    Ok(Stats {
        rx_bytes: u64::from_le_bytes(bytes[0..8].try_into().unwrap()),
        dropped_bytes: u64::from_le_bytes(bytes[8..16].try_into().unwrap()),
        sync_loss: u64::from_le_bytes(bytes[16..24].try_into().unwrap()),
        fifo_high_water: u32::from_le_bytes(bytes[24..28].try_into().unwrap()),
        dma_faults: u64::from_le_bytes(bytes[28..36].try_into().unwrap()),
    })
}

// -- M3 trace -> Perfetto pipeline (see M3_PERFETTO_VISUALIZATION_PLAN.md) --

/// Phase 1: recover one orbflow channel's raw byte stream from a capture
/// file. Frame boundaries are `0x00` delimiters (COBS encoding never emits a
/// literal `0x00` inside a frame, so splitting on it is exact); frames that
/// fail `orbflow_unframe`'s checksum are counted-not-panicked-on, since a
/// real capture's sync-loss rate makes partial/corrupt frames routine.
pub fn reconstruct_channel_stream(capture: &[u8], channel: u8) -> Vec<u8> {
    let mut stream = Vec::new();
    for frame in capture.split(|&byte| byte == 0) {
        if frame.is_empty() {
            continue;
        }
        if let Ok((frame_channel, payload)) = orbflow_unframe(frame) {
            if frame_channel == channel {
                stream.extend(payload);
            }
        }
    }
    stream
}

/// How many valid orbflow frames were seen per channel, most-frequent
/// first. Used to pick the real M3 channel empirically instead of assuming
/// it (see the plan's Phase 1, item 3) rather than hardcoding a value.
pub fn channel_histogram(capture: &[u8]) -> Vec<(u8, usize)> {
    let mut counts = std::collections::BTreeMap::new();
    for frame in capture.split(|&byte| byte == 0) {
        if frame.is_empty() {
            continue;
        }
        if let Ok((channel, _)) = orbflow_unframe(frame) {
            *counts.entry(channel).or_insert(0usize) += 1;
        }
    }
    let mut entries: Vec<(u8, usize)> = counts.into_iter().collect();
    entries.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    entries
}

/// Phase 2: one decoded ARMv7-M ITM packet.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ItmPacket {
    /// Software Instrumentation Trace write: `header = (port << 3) |
    /// size_code`, `size_code` 1/2/3 for a 1/2/4-byte little-endian payload.
    Swit { port: u8, value: u32, width: u8 },
    /// A header byte whose `size_code` isn't 1/2/3 (some other ITM packet
    /// type, or resync noise) -- not a SWIT packet.
    Unrecognized(u8),
}

/// Decode a raw ITM byte stream into packets. Tolerant of garbage: an
/// unrecognized or truncated header advances exactly one byte and resumes,
/// the same posture `orbtrace_tpiu_demux.sv`'s channel-plausibility gate
/// takes at the framing layer, since real captures interleave resync noise
/// with genuine SWIT packets.
pub fn decode_itm_stream(bytes: &[u8]) -> Vec<ItmPacket> {
    let mut packets = Vec::new();
    let mut pos = 0;
    while pos < bytes.len() {
        let header = bytes[pos];
        let width = match header & 0x7 {
            1 => 1,
            2 => 2,
            3 => 4,
            _ => {
                packets.push(ItmPacket::Unrecognized(header));
                pos += 1;
                continue;
            }
        };
        if pos + 1 + width > bytes.len() {
            packets.push(ItmPacket::Unrecognized(header));
            pos += 1;
            continue;
        }
        let mut value = 0u32;
        for (index, byte) in bytes[pos + 1..pos + 1 + width].iter().enumerate() {
            value |= (*byte as u32) << (8 * index);
        }
        packets.push(ItmPacket::Swit {
            port: header >> 3,
            value,
            width: width as u8,
        });
        pos += 1 + width;
    }
    packets
}

/// Phase 3: the M3 firmware's `Workload` event kind recovered from a port-0
/// SWIT's width/value shape (see M3_PERFETTO_VISUALIZATION_PLAN.md section
/// 3). `Timestamp` values are `sequence`, a multiple of 16 by construction
/// (it's only emitted when `sequence & 15 == 0`); `Fault` values always have
/// the fixed `0xf001_0000` high half; `Malformed` is the only 1-byte-wide
/// port-0 write; `Idle` is whatever 4-byte, non-fault, non-multiple-of-16
/// value is left. This is a best-effort classification (a 1-in-16 `Idle`
/// value can coincidentally be a multiple of 16 and get misread as a
/// `Timestamp`) -- good enough to label a visualization, not a claim of
/// exact semantic recovery.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Port0Event {
    Timestamp(u32),
    Idle(u32),
    Malformed(u8),
    Fault(u32),
    Unknown(u32),
}

pub fn classify_port0(value: u32, width: u8) -> Port0Event {
    match width {
        1 => Port0Event::Malformed(value as u8),
        4 if value & 0xffff_0000 == 0xf001_0000 => Port0Event::Fault(value),
        4 if value != 0 && value % 16 == 0 => Port0Event::Timestamp(value),
        4 if (1..=1024).contains(&value) => Port0Event::Idle(value),
        _ => Port0Event::Unknown(value),
    }
}

fn event_name(port: u8, value: u32, width: u8) -> String {
    if port == 0 {
        match classify_port0(value, width) {
            Port0Event::Timestamp(v) => format!("Timestamp({v})"),
            Port0Event::Idle(v) => format!("Idle({v})"),
            Port0Event::Malformed(v) => format!("Malformed({v:#04x})"),
            Port0Event::Fault(v) => format!("Fault({v:#010x})"),
            Port0Event::Unknown(v) => format!("Unknown({v:#x})"),
        }
    } else {
        format!("port {port} = {value:#x}")
    }
}

/// One instant event on a per-ITM-port Perfetto track. `ts` is a synthetic,
/// monotonically increasing index over decoded SWIT packets (this pipeline
/// has no real wall-clock time base), which is enough to preserve relative
/// ordering in the viewer.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PerfettoEvent {
    pub port: u8,
    pub ts: u64,
    pub name: String,
    pub value: u32,
    pub width: u8,
}

#[derive(Clone, Debug, Eq, PartialEq, Default)]
pub struct PerfettoTrace {
    pub events: Vec<PerfettoEvent>,
    /// `(ts, cumulative_count)` samples for a `dropped_sequence_gap` counter
    /// track, stepped once each time consecutive recovered port-0
    /// `Timestamp` values don't differ by exactly 16 -- direct, visible
    /// evidence of the sync-loss `M3_TRACE_VERIFICATION_PLAN.md` Phase E/F
    /// already measured, rather than an external stat.
    pub sequence_gaps: Vec<(u64, u64)>,
}

/// Phase 3: map decoded ITM packets to Perfetto track events.
/// `Unrecognized` packets don't produce an event (see `decode_itm_stream`'s
/// doc comment) and don't advance `ts`.
pub fn build_perfetto_trace(packets: &[ItmPacket]) -> PerfettoTrace {
    let mut trace = PerfettoTrace::default();
    let mut last_timestamp: Option<u32> = None;
    let mut gap_count = 0u64;
    let mut ts = 0u64;
    for packet in packets {
        let ItmPacket::Swit { port, value, width } = *packet else {
            continue;
        };
        if port == 0 {
            if let Port0Event::Timestamp(v) = classify_port0(value, width) {
                if let Some(previous) = last_timestamp {
                    if v != previous.wrapping_add(16) {
                        gap_count += 1;
                        trace.sequence_gaps.push((ts, gap_count));
                    }
                }
                last_timestamp = Some(v);
            }
        }
        trace.events.push(PerfettoEvent {
            port,
            ts,
            name: event_name(port, value, width),
            value,
            width,
        });
        ts += 1;
    }
    trace
}

fn json_escape(out: &mut String, value: &str) {
    out.push('"');
    for c in value.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            c if (c as u32) < 0x20 => {
                write!(out, "\\u{:04x}", c as u32).unwrap();
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// Phase 4: render a `PerfettoTrace` as a Chrome/Perfetto JSON trace
/// (`{"traceEvents":[...]}`), viewable by dragging into ui.perfetto.dev.
/// One thread (`tid`) per ITM port under `pid` 1, named via a `"ph":"M"`
/// metadata event; each decoded SWIT is a `"ph":"I"` instant event on its
/// port's thread. `sequence_gaps`, if non-empty, becomes a `"ph":"C"`
/// counter track under a separate `pid` so it renders on its own row.
pub fn perfetto_json(trace: &PerfettoTrace) -> String {
    let mut out = String::from("{\"traceEvents\":[\n");
    let mut first = true;
    let mut item = |out: &mut String, s: &str| {
        if !first {
            out.push_str(",\n");
        }
        first = false;
        out.push_str(s);
    };
    for port in 0u8..=7 {
        let mut entry = format!("{{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":{port},\"args\":{{\"name\":");
        json_escape(&mut entry, &format!("ITM port {port}"));
        entry.push_str("}}");
        item(&mut out, &entry);
    }
    for event in &trace.events {
        let mut entry = String::new();
        write!(
            entry,
            "{{\"ph\":\"I\",\"ts\":{},\"pid\":1,\"tid\":{},\"s\":\"t\",\"name\":",
            event.ts, event.port
        )
        .unwrap();
        json_escape(&mut entry, &event.name);
        write!(
            entry,
            ",\"args\":{{\"value\":{},\"width\":{}}}}}",
            event.value, event.width
        )
        .unwrap();
        item(&mut out, &entry);
    }
    if !trace.sequence_gaps.is_empty() {
        item(
            &mut out,
            "{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":2,\"tid\":0,\"args\":{\"name\":\"dropped_sequence_gap\"}}",
        );
        for (ts, value) in &trace.sequence_gaps {
            let entry = format!(
                "{{\"ph\":\"C\",\"name\":\"dropped_sequence_gap\",\"ts\":{ts},\"pid\":2,\"tid\":0,\"args\":{{\"dropped_sequence_gap\":{value}}}}}"
            );
            item(&mut out, &entry);
        }
    }
    out.push_str("\n]}\n");
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cobs_fixed_and_boundaries() {
        let cases = [
            vec![],
            vec![0],
            vec![0, 0],
            vec![1, 0, 2],
            (1..=254).collect(),
        ];
        for input in cases {
            assert_eq!(cobs_decode(&cobs_encode(&input)).unwrap(), input);
        }
        assert_eq!(cobs_encode(&[0]), vec![1, 1]);
        assert_eq!(cobs_encode(&[1, 2, 3]), vec![4, 1, 2, 3]);
    }

    #[test]
    fn cobs_randomized_streams_are_reversible() {
        let mut state = 0x6d2b_79f5u32;
        for length in 0..=1024 {
            let mut input = Vec::with_capacity(length);
            for _ in 0..length {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                input.push(state as u8);
            }
            assert_eq!(cobs_decode(&cobs_encode(&input)).unwrap(), input);
        }
    }

    #[test]
    fn orbflow_checksum_wraps() {
        let framed = orbflow_frame(7, &[0, 0xff, 1, 2]).unwrap();
        assert!(!framed[..framed.len() - 1].contains(&0));
        assert_eq!(orbflow_unframe(&framed).unwrap(), (7, vec![0, 0xff, 1, 2]));
        let mut bad = framed;
        bad[1] ^= 1;
        assert!(orbflow_unframe(&bad).is_err());
    }

    #[test]
    fn control_round_trip_and_limits() {
        let command = Command::Configure {
            source: Source::R5,
            format: TraceFormat::Tpiu4,
            swo_baud: 2_000_000,
        };
        assert_eq!(Command::decode(&command.encode()).unwrap(), command);
        assert!(length_frame(&vec![0; MAX_DAP_PACKET + 1], MAX_DAP_PACKET).is_err());
    }

    #[test]
    fn m3_commands_round_trip() {
        let load = Command::LoadM3Chunk {
            offset: 0x1234,
            data: vec![1, 2, 3, 4, 5],
        };
        assert_eq!(Command::decode(&load.encode()).unwrap(), load);

        let read = Command::ReadM3Bram {
            offset: 0x100,
            length: 16,
        };
        assert_eq!(Command::decode(&read.encode()).unwrap(), read);

        let control = Command::M3Control {
            bits: registers::M3_CONTROL_RELEASE as u8,
        };
        assert_eq!(Command::decode(&control.encode()).unwrap(), control);

        // An empty chunk is a degenerate but valid write of zero bytes.
        let empty = Command::LoadM3Chunk {
            offset: 0,
            data: vec![],
        };
        assert_eq!(Command::decode(&empty.encode()).unwrap(), empty);
    }

    #[test]
    fn swo_nrz_decodes_and_rejects_stop_error() {
        let value = 0xa6u8;
        let mut samples = Vec::new();
        for level in std::iter::once(false)
            .chain((0..8).map(|b| value & (1 << b) != 0))
            .chain(std::iter::once(true))
        {
            samples.extend([level; 4]);
        }
        assert_eq!(decode_swo_nrz(&samples, 4), Ok(value));
        samples[38] = false;
        assert_eq!(decode_swo_nrz(&samples, 4), Err(SwoError::BadStop));
    }

    #[test]
    fn parallel_ddr_widths_have_identical_wire_order() {
        assert_eq!(decode_parallel_ddr(&[0x0b, 0x0a], 4).unwrap(), [0xab]);
        assert_eq!(decode_parallel_ddr(&[3, 2, 2, 2], 2).unwrap(), [0xab]);
        assert_eq!(
            decode_parallel_ddr(
                &[true, true, false, true, false, true, false, true].map(u8::from),
                1
            )
            .unwrap(),
            [0xab]
        );
        assert!(decode_parallel_ddr(&[], 3).is_err());
    }

    #[test]
    fn swo_manchester_decodes_and_rejects_missing_transition() {
        let value = 0xa6u8;
        let bits = std::iter::once(false)
            .chain((0..8).map(|bit| value & (1 << bit) != 0))
            .chain(std::iter::once(true));
        let mut halves = Vec::new();
        for bit in bits {
            halves.extend([bit, !bit]);
        }
        assert_eq!(decode_swo_manchester(&halves), Ok(value));
        halves[6] = halves[7];
        assert_eq!(decode_swo_manchester(&halves), Err(SwoError::BadManchester));
    }

    #[test]
    fn synthetic_dap_wait_read_and_abort() {
        let mut dap = SyntheticDap::default();
        dap.set_wait_once();
        assert_eq!(dap.execute(&[5, 0, 1, 2]).unwrap(), vec![5, 0, 2]);
        assert_eq!(
            dap.execute(&[5, 0, 1, 2]).unwrap(),
            vec![5, 1, 1, 2, 0, 0xa5, 0xa5]
        );
        assert_eq!(dap.execute(&[8]).unwrap(), vec![8, 0]);
        assert_eq!(dap.aborts, 1);
    }

    #[test]
    fn synthetic_dap_fault_parity_and_jtag_paths() {
        let mut dap = SyntheticDap::default();
        dap.set_fault_once();
        assert_eq!(dap.execute(&[5, 0, 1, 2]).unwrap(), vec![5, 0, 4]);
        dap.set_parity_error_once();
        assert_eq!(dap.execute(&[5, 0, 1, 2]).unwrap()[..3], [5, 1, 9]);
        assert_eq!(dap.execute(&[0x14, 1, 1, 0xaa]).unwrap(), [0x14, 0, 0xab]);
        assert_eq!(dap.execute(&[7]).unwrap(), [7, 0]);
        assert_eq!(dap.aborts, 1);
    }

    #[test]
    fn synthetic_dap_parses_mixed_read_write_transfers() {
        let mut dap = SyntheticDap::default();
        let response = dap.execute(&[5, 0, 2, 0, 1, 2, 3, 4, 2]).unwrap();
        assert_eq!(response, [5, 2, 1, 2, 0, 0xa5, 0xa5]);
        assert_eq!(dap.transfers, 2);
        assert_eq!(dap.execute(&[5, 0, 1, 0]).unwrap(), [5, 0, 4]);
    }

    #[test]
    fn tpiu_sync_and_unmangle() {
        let mut decoder = TpiuDecoder::default();
        for b in [0x7f, 0xff, 0xff, 0xff] {
            assert!(decoder.push(b).is_empty());
        }
        // ID=2 in byte 0, followed by fourteen data bytes; aux bit restores bit zero.
        let mut raw = [0x22u8; 16];
        raw[0] = 0x05;
        raw[15] = 0;
        let mut output = Vec::new();
        for b in raw {
            output.extend(decoder.push(b));
        }
        assert_eq!(output.len(), 14);
        assert!(output.iter().all(|b| b.channel == 2));
        decoder.reset_sync();
        assert_eq!(decoder.sync_losses, 1);
    }

    fn encode_swit(port: u8, value: u32, width: u8) -> Vec<u8> {
        let size_code = match width {
            1 => 1,
            2 => 2,
            4 => 3,
            _ => panic!("bad width"),
        };
        let mut out = vec![(port << 3) | size_code];
        out.extend(&value.to_le_bytes()[..width as usize]);
        out
    }

    #[test]
    fn reconstruct_channel_stream_filters_and_skips_corrupt_frames() {
        let mut capture = Vec::new();
        capture.extend(orbflow_frame(1, &[1, 2, 3]).unwrap());
        capture.extend(orbflow_frame(2, &[9, 9]).unwrap());
        let mut corrupt = orbflow_frame(3, &[7, 7, 7]).unwrap();
        corrupt[1] ^= 1; // flip a payload byte inside the COBS-encoded frame
        capture.extend(corrupt);
        capture.extend(orbflow_frame(1, &[4, 5]).unwrap());

        assert_eq!(
            reconstruct_channel_stream(&capture, 1),
            vec![1, 2, 3, 4, 5]
        );
        assert_eq!(reconstruct_channel_stream(&capture, 2), vec![9, 9]);
        assert_eq!(reconstruct_channel_stream(&capture, 3), Vec::<u8>::new());

        let histogram = channel_histogram(&capture);
        assert_eq!(histogram, vec![(1, 2), (2, 1)]);
    }

    #[test]
    fn decode_itm_stream_round_trips_and_resyncs_past_garbage() {
        let events = [
            (0u8, 32u32, 4u8),         // Timestamp
            (0, 500, 4),               // Idle
            (0, 0xab, 1),              // Malformed
            (0, 0xf001_0007, 4),       // Fault
            (5, 0x1234, 2),            // generic port
        ];
        let mut bytes = Vec::new();
        for (port, value, width) in events {
            bytes.push(0x00); // reserved size_code=0 header: garbage to resync past
            bytes.extend(encode_swit(port, value, width));
        }
        bytes.push(0x06); // size_code=6, reserved: trailing garbage

        let decoded: Vec<ItmPacket> = decode_itm_stream(&bytes)
            .into_iter()
            .filter(|packet| matches!(packet, ItmPacket::Swit { .. }))
            .collect();
        let expected: Vec<ItmPacket> = events
            .iter()
            .map(|&(port, value, width)| ItmPacket::Swit { port, value, width })
            .collect();
        assert_eq!(decoded, expected);
    }

    #[test]
    fn classify_port0_matches_value_shape() {
        assert_eq!(classify_port0(32, 4), Port0Event::Timestamp(32));
        assert_eq!(classify_port0(500, 4), Port0Event::Idle(500));
        assert_eq!(classify_port0(0xab, 1), Port0Event::Malformed(0xab));
        assert_eq!(
            classify_port0(0xf001_0007, 4),
            Port0Event::Fault(0xf001_0007)
        );
        assert_eq!(classify_port0(2001, 4), Port0Event::Unknown(2001));
    }

    #[test]
    fn build_perfetto_trace_detects_sequence_gaps() {
        let packets = vec![
            ItmPacket::Swit {
                port: 0,
                value: 16,
                width: 4,
            },
            ItmPacket::Swit {
                port: 0,
                value: 32,
                width: 4,
            },
            ItmPacket::Swit {
                port: 0,
                value: 64, // skipped 48: a dropped Timestamp cycle
                width: 4,
            },
        ];
        let trace = build_perfetto_trace(&packets);
        assert_eq!(trace.events.len(), 3);
        assert_eq!(trace.sequence_gaps, vec![(2, 1)]);
    }

    #[test]
    fn perfetto_json_has_one_instant_per_event_and_a_gap_counter() {
        let packets = vec![
            ItmPacket::Swit {
                port: 0,
                value: 16,
                width: 4,
            },
            ItmPacket::Swit {
                port: 0,
                value: 64,
                width: 4,
            },
            ItmPacket::Swit {
                port: 3,
                value: 0xdead,
                width: 2,
            },
        ];
        let trace = build_perfetto_trace(&packets);
        let json = perfetto_json(&trace);
        assert!(json.starts_with("{\"traceEvents\":["));
        assert_eq!(json.matches("\"ph\":\"I\"").count(), trace.events.len());
        assert_eq!(
            json.matches("\"ph\":\"C\"").count(),
            trace.sequence_gaps.len()
        );
        assert!(json.contains("Timestamp(16)"));
        assert!(json.contains("port 3 = 0xdead"));
        assert!(json.contains("dropped_sequence_gap"));
    }
}
