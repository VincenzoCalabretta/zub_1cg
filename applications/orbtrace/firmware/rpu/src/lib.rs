#![no_std]

/// Deterministic branchy control-flow pattern for R5-0 ETM tracing
/// (PS_CORESIGHT_TRACE_PLAN.md Phase 4). Unlike the M3's `Workload`
/// (`//applications/orbtrace/firmware/m3`), which emits explicit ITM
/// stimulus values the host decoder reconstructs from the wire, ETMv4
/// traces instruction/branch flow directly -- there's no "emit" call to
/// model. This instead models *which distinguishable, noinline branch
/// target* a real R5 workload visits next, so a future ETM decoder can
/// cross-check recovered branch addresses against a known-reproducible
/// sequence -- the same verify-against-a-known-sequence methodology
/// `M3_TRACE_VERIFICATION_PLAN.md`'s Phase F used for ITM content.
/// `//applications/rpu/orbtrace_workload`'s `main.c` hand-ports this exact
/// sequence -- see that app's main.c for the C equivalent.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Action {
    Marker(u32),
    Spin(u32),
    Branch(bool),
    Fault(u32),
    Call(u8),
}

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

    pub fn next(&mut self) -> Action {
        self.state ^= self.state << 13;
        self.state ^= self.state >> 17;
        self.state ^= self.state << 5;
        self.sequence = self.sequence.wrapping_add(1);
        match self.sequence & 15 {
            0 => Action::Marker(self.sequence),
            1 => Action::Spin((self.state & 0x3ff) + 1),
            2 => Action::Branch(self.state & 1 == 0),
            3 => Action::Fault(0xf001_0000 | self.sequence),
            n => Action::Call((n % 7) as u8),
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

    #[test]
    fn visits_every_call_target() {
        let mut w = Workload::new(1);
        let mut seen = [false; 7];
        for _ in 0..1000 {
            if let Action::Call(target) = w.next() {
                seen[target as usize] = true;
            }
        }
        assert!(seen.iter().all(|&s| s), "not every Call target was visited: {seen:?}");
    }
}
