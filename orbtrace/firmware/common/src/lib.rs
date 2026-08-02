#![no_std]

use core::sync::atomic::{fence, Ordering};

pub const CONTROL_PORT: u16 = 3401;
pub const TRACE_PORT: u16 = 3402;
pub const DAP_PORT: u16 = 3240;
pub const PROTOCOL_VERSION: u16 = 1;
pub const MAX_CONTROL_PAYLOAD: usize = 4096;
pub const MAX_DAP_PACKET: usize = 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FrameError {
    TooLarge(usize),
    FramePending,
}

/// Allocation-free decoder for the little-endian u32 length framing used by
/// TCP 3401 and TCP 3240. It accepts arbitrarily fragmented network input.
pub struct FrameDecoder<const N: usize> {
    length: [u8; 4],
    length_used: usize,
    expected: usize,
    payload: [u8; N],
    payload_used: usize,
    ready: bool,
}

impl<const N: usize> FrameDecoder<N> {
    pub const fn new() -> Self {
        Self {
            length: [0; 4],
            length_used: 0,
            expected: 0,
            payload: [0; N],
            payload_used: 0,
            ready: false,
        }
    }

    /// Consume one byte. A completed payload remains borrowed from this
    /// decoder until `reset` is called after the response has been emitted.
    pub fn push(&mut self, byte: u8) -> Result<bool, FrameError> {
        if self.ready {
            return Err(FrameError::FramePending);
        }
        if self.length_used < 4 {
            self.length[self.length_used] = byte;
            self.length_used += 1;
            if self.length_used == 4 {
                self.expected = u32::from_le_bytes(self.length) as usize;
                if self.expected > N {
                    let declared = self.expected;
                    self.reset();
                    return Err(FrameError::TooLarge(declared));
                }
                self.ready = self.expected == 0;
            }
            return Ok(self.ready);
        }
        self.payload[self.payload_used] = byte;
        self.payload_used += 1;
        self.ready = self.payload_used == self.expected;
        Ok(self.ready)
    }

    pub fn frame(&self) -> Option<&[u8]> {
        self.ready.then_some(&self.payload[..self.expected])
    }

    pub fn reset(&mut self) {
        self.length = [0; 4];
        self.length_used = 0;
        self.expected = 0;
        self.payload_used = 0;
        self.ready = false;
    }
}

impl<const N: usize> Default for FrameDecoder<N> {
    fn default() -> Self {
        Self::new()
    }
}

pub const AXI_DMA_LENGTH_MASK: u32 = 0x03ff_ffff;
pub const AXI_DMA_STATUS_COMPLETE: u32 = 1 << 31;
pub const AXI_DMA_STATUS_ERROR_MASK: u32 = 0x7000_0000;

/// Native 64-byte Xilinx AXI DMA scatter/gather buffer descriptor.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C, align(64))]
pub struct AxiDmaDescriptor {
    pub next_descriptor_lo: u32,
    pub next_descriptor_hi: u32,
    pub buffer_address_lo: u32,
    pub buffer_address_hi: u32,
    pub reserved: [u32; 2],
    pub control: u32,
    pub status: u32,
    pub application: [u32; 5],
    pub padding: [u32; 3],
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DmaRingError {
    InvalidCount,
    UnalignedDescriptorBase,
    InvalidCapacity,
    Full,
    Hardware(u32),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CompletedBuffer {
    pub address: u64,
    pub capacity: u32,
    pub transferred: u32,
    pub descriptor_address: u64,
}

/// Software ownership state for an AXI DMA S2MM descriptor ring.
///
/// The caller owns cache maintenance for descriptor and payload memory. After
/// `enqueue`, program the returned descriptor address into S2MM_TAILDESC. DMA
/// completion is observed from the hardware-written status word.
pub struct AxiDmaRing<'a> {
    descriptors: &'a mut [AxiDmaDescriptor],
    descriptor_base: u64,
    producer: usize,
    consumer: usize,
}

impl<'a> AxiDmaRing<'a> {
    pub fn new(
        descriptors: &'a mut [AxiDmaDescriptor],
        descriptor_base: u64,
    ) -> Result<Self, DmaRingError> {
        if descriptors.len() < 2 || !descriptors.len().is_power_of_two() {
            return Err(DmaRingError::InvalidCount);
        }
        if descriptor_base & 63 != 0 {
            return Err(DmaRingError::UnalignedDescriptorBase);
        }
        let count = descriptors.len();
        for (index, descriptor) in descriptors.iter_mut().enumerate() {
            *descriptor = AxiDmaDescriptor::default();
            let next = descriptor_base + (((index + 1) & (count - 1)) * 64) as u64;
            descriptor.next_descriptor_lo = next as u32;
            descriptor.next_descriptor_hi = (next >> 32) as u32;
        }
        Ok(Self {
            descriptors,
            descriptor_base,
            producer: 0,
            consumer: 0,
        })
    }

    pub fn current_descriptor_address(&self) -> u64 {
        self.descriptor_base + (self.consumer * 64) as u64
    }

    pub fn enqueue(&mut self, address: u64, capacity: u32) -> Result<u64, DmaRingError> {
        if capacity == 0 || capacity > AXI_DMA_LENGTH_MASK {
            return Err(DmaRingError::InvalidCapacity);
        }
        let next = (self.producer + 1) & (self.descriptors.len() - 1);
        if next == self.consumer {
            return Err(DmaRingError::Full);
        }
        let index = self.producer;
        let descriptor = &mut self.descriptors[index];
        descriptor.buffer_address_lo = address as u32;
        descriptor.buffer_address_hi = (address >> 32) as u32;
        descriptor.control = capacity;
        descriptor.status = 0;
        fence(Ordering::Release);
        self.producer = next;
        Ok(self.descriptor_base + (index * 64) as u64)
    }

    pub fn poll_completed(&mut self) -> Result<Option<CompletedBuffer>, DmaRingError> {
        if self.consumer == self.producer {
            return Ok(None);
        }
        fence(Ordering::Acquire);
        let index = self.consumer;
        let descriptor = &mut self.descriptors[index];
        if descriptor.status & AXI_DMA_STATUS_COMPLETE == 0 {
            return Ok(None);
        }
        if descriptor.status & AXI_DMA_STATUS_ERROR_MASK != 0 {
            return Err(DmaRingError::Hardware(descriptor.status));
        }
        let completed = CompletedBuffer {
            address: (descriptor.buffer_address_lo as u64)
                | ((descriptor.buffer_address_hi as u64) << 32),
            capacity: descriptor.control & AXI_DMA_LENGTH_MASK,
            transferred: descriptor.status & AXI_DMA_LENGTH_MASK,
            descriptor_address: self.descriptor_base + (index * 64) as u64,
        };
        descriptor.control = 0;
        descriptor.status = 0;
        self.consumer = (self.consumer + 1) & (self.descriptors.len() - 1);
        Ok(Some(completed))
    }

    #[cfg(test)]
    fn set_status(&mut self, index: usize, status: u32) {
        self.descriptors[index].status = status;
    }
}

#[cfg(test)]
extern crate std;
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn framing_survives_fragmentation_and_empty_frames() {
        let mut decoder = FrameDecoder::<8>::new();
        for byte in [3, 0, 0, 0, 0xaa, 0xbb] {
            assert_eq!(decoder.push(byte), Ok(false));
        }
        assert_eq!(decoder.push(0xcc), Ok(true));
        assert_eq!(decoder.frame(), Some(&[0xaa, 0xbb, 0xcc][..]));
        assert_eq!(decoder.push(0), Err(FrameError::FramePending));
        decoder.reset();
        for byte in [0, 0, 0, 0] {
            let _ = decoder.push(byte);
        }
        assert_eq!(decoder.frame(), Some(&[][..]));
    }

    #[test]
    fn framing_rejects_oversize_and_recovers() {
        let mut decoder = FrameDecoder::<2>::new();
        assert_eq!(decoder.push(3), Ok(false));
        assert_eq!(decoder.push(0), Ok(false));
        assert_eq!(decoder.push(0), Ok(false));
        assert_eq!(decoder.push(0), Err(FrameError::TooLarge(3)));
        for byte in [1, 0, 0, 0, 0x55] {
            let _ = decoder.push(byte);
        }
        assert_eq!(decoder.frame(), Some(&[0x55][..]));
    }

    #[test]
    fn descriptor_ring_links_wraps_and_never_overwrites() {
        assert_eq!(core::mem::size_of::<AxiDmaDescriptor>(), 64);
        assert_eq!(core::mem::align_of::<AxiDmaDescriptor>(), 64);
        let mut storage = [AxiDmaDescriptor::default(); 4];
        let mut ring = AxiDmaRing::new(&mut storage, 0x8000_0000).unwrap();
        assert_eq!(ring.current_descriptor_address(), 0x8000_0000);
        assert_eq!(ring.enqueue(0x1_0000_1000, 4096), Ok(0x8000_0000));
        assert_eq!(ring.enqueue(0x2000, 2048), Ok(0x8000_0040));
        assert_eq!(ring.enqueue(0x3000, 1024), Ok(0x8000_0080));
        assert_eq!(ring.enqueue(0x4000, 512), Err(DmaRingError::Full));
        ring.set_status(0, AXI_DMA_STATUS_COMPLETE | 128);
        assert_eq!(
            ring.poll_completed(),
            Ok(Some(CompletedBuffer {
                address: 0x1_0000_1000,
                capacity: 4096,
                transferred: 128,
                descriptor_address: 0x8000_0000,
            }))
        );
        assert_eq!(ring.enqueue(0x4000, 512), Ok(0x8000_00c0));
    }

    #[test]
    fn descriptor_ring_reports_dma_errors_without_consuming() {
        let mut storage = [AxiDmaDescriptor::default(); 2];
        let mut ring = AxiDmaRing::new(&mut storage, 0x4000).unwrap();
        ring.enqueue(0x8000, 64).unwrap();
        ring.set_status(0, AXI_DMA_STATUS_COMPLETE | (1 << 29));
        assert_eq!(
            ring.poll_completed(),
            Err(DmaRingError::Hardware(AXI_DMA_STATUS_COMPLETE | (1 << 29)))
        );
        assert_eq!(ring.enqueue(0x9000, 64), Err(DmaRingError::Full));
    }
}
