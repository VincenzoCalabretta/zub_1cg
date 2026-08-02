//! Byte-exact software reference for the ZUBoard Orbtrace data plane.
//!
//! This crate deliberately has no target or async dependencies, so the same
//! model can be used by host tests and by `no_std` firmware after replacing
//! the allocation boundary.

use std::fmt;

pub const CONTROL_PORT: u16 = 3401;
pub const ORBFLOW_PORT: u16 = 3402;
pub const CMSIS_DAP_PORT: u16 = 3240;
pub const PROTOCOL_VERSION: u16 = 1;
pub const MAX_CONTROL_PAYLOAD: usize = 4096;
pub const MAX_DAP_PACKET: usize = 1024;

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

    pub const ID_VALUE: u32 = 0x4f52_4254; // "ORBT"
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
    VexRiscv = 0,
    R5 = 1,
    A53 = 2,
    Test = 3,
}

impl TryFrom<u8> for Source {
    type Error = ProtocolError;
    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::VexRiscv),
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
}
