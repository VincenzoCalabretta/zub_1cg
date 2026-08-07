#![no_std]

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Event {
    Itm { channel: u8, value: u32, width: u8 },
    Timestamp(u32),
    Idle(u32),
    Malformed(u8),
    Fault(u32),
}

/// Allocation-free deterministic stimulus. Reinitializing with the same seed
/// reproduces command/trace correlation exactly on RV32IMAC and in host tests.
pub struct Workload {
    state: u32,
    sequence: u32,
}
impl Workload {
    pub const fn new(seed: u32) -> Self {
        Self {
            state: seed,
            sequence: 0,
        }
    }
    pub fn next(&mut self) -> Event {
        self.state ^= self.state << 13;
        self.state ^= self.state >> 17;
        self.state ^= self.state << 5;
        self.sequence = self.sequence.wrapping_add(1);
        match self.sequence & 15 {
            0 => Event::Timestamp(self.sequence),
            1 => Event::Idle((self.state & 0x3ff) + 1),
            2 => Event::Malformed(self.state as u8),
            3 => Event::Fault(0xf001_0000 | self.sequence),
            n => Event::Itm {
                channel: (n % 7) as u8 + 1,
                value: self.state,
                width: [1, 2, 4][n as usize % 3],
            },
        }
    }
}

#[cfg(test)]
extern crate std;
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reproducible() {
        let mut a = Workload::new(7);
        let mut b = Workload::new(7);
        for _ in 0..100 {
            assert_eq!(a.next(), b.next());
        }
    }
}
