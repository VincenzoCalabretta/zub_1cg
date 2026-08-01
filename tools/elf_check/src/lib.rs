//! Minimal ELF program-header validation for bare-metal firmware.
//!
//! This intentionally parses only the little-endian ELF fields needed to
//! enforce load-segment invariants, avoiding a large host-side dependency.

use std::fmt;

const ELF_MAGIC: &[u8; 4] = b"\x7fELF";
const ELFCLASS32: u8 = 1;
const ELFCLASS64: u8 = 2;
const ELFDATA2LSB: u8 = 1;
const PT_LOAD: u32 = 1;
const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Class {
    Elf32,
    Elf64,
}

impl fmt::Display for Class {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Elf32 => write!(formatter, "ELF32"),
            Self::Elf64 => write!(formatter, "ELF64"),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LoadSegment {
    pub flags: u32,
}

impl LoadSegment {
    pub fn is_read_execute(self) -> bool {
        self.flags & (PF_R | PF_X | PF_W) == (PF_R | PF_X)
    }

    pub fn is_read_write(self) -> bool {
        self.flags & (PF_R | PF_X | PF_W) == (PF_R | PF_W)
    }

    pub fn is_writable_and_executable(self) -> bool {
        self.flags & (PF_W | PF_X) == (PF_W | PF_X)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Elf {
    pub class: Class,
    pub machine: u16,
    pub entry: u64,
    pub load_segments: Vec<LoadSegment>,
}

fn byte_range(bytes: &[u8], offset: usize, length: usize) -> Result<&[u8], String> {
    bytes
        .get(offset..offset.saturating_add(length))
        .ok_or_else(|| format!("truncated ELF at offset 0x{offset:x}"))
}

fn u16_at(bytes: &[u8], offset: usize) -> Result<u16, String> {
    let value = byte_range(bytes, offset, 2)?;
    Ok(u16::from_le_bytes([value[0], value[1]]))
}

fn u32_at(bytes: &[u8], offset: usize) -> Result<u32, String> {
    let value = byte_range(bytes, offset, 4)?;
    Ok(u32::from_le_bytes([value[0], value[1], value[2], value[3]]))
}

fn u64_at(bytes: &[u8], offset: usize) -> Result<u64, String> {
    let value = byte_range(bytes, offset, 8)?;
    Ok(u64::from_le_bytes([
        value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7],
    ]))
}

/// Parses the ELF header and PT_LOAD program headers relevant to firmware.
pub fn parse(bytes: &[u8]) -> Result<Elf, String> {
    if byte_range(bytes, 0, 16)?[..4] != *ELF_MAGIC {
        return Err("not an ELF file".to_owned());
    }
    if bytes[5] != ELFDATA2LSB {
        return Err("only little-endian ELF is supported".to_owned());
    }

    let class = match bytes[4] {
        ELFCLASS32 => Class::Elf32,
        ELFCLASS64 => Class::Elf64,
        other => return Err(format!("unsupported ELF class {other}")),
    };
    let machine = u16_at(bytes, 18)?;
    let (entry, program_header_offset, program_header_size, program_header_count, flags_offset) =
        match class {
            Class::Elf32 => (
                u32_at(bytes, 24)? as u64,
                u32_at(bytes, 28)? as u64,
                u16_at(bytes, 42)? as usize,
                u16_at(bytes, 44)? as usize,
                24,
            ),
            Class::Elf64 => (
                u64_at(bytes, 24)?,
                u64_at(bytes, 32)?,
                u16_at(bytes, 54)? as usize,
                u16_at(bytes, 56)? as usize,
                4,
            ),
        };
    let minimum_header_size = match class {
        Class::Elf32 => 32,
        Class::Elf64 => 56,
    };
    if program_header_size < minimum_header_size {
        return Err(format!(
            "program header entry is too small: {program_header_size} bytes"
        ));
    }

    let mut load_segments = Vec::new();
    for index in 0..program_header_count {
        let offset = program_header_offset
            .checked_add((index * program_header_size) as u64)
            .ok_or_else(|| "program header offset overflow".to_owned())?;
        let offset = usize::try_from(offset).map_err(|_| "program header offset is too large")?;
        byte_range(bytes, offset, program_header_size)?;
        if u32_at(bytes, offset)? == PT_LOAD {
            load_segments.push(LoadSegment {
                flags: u32_at(bytes, offset + flags_offset)?,
            });
        }
    }
    Ok(Elf {
        class,
        machine,
        entry,
        load_segments,
    })
}

/// Validates the hardened firmware segment layout.
pub fn validate(elf: &Elf) -> Result<(), String> {
    if elf.load_segments.is_empty() {
        return Err("ELF has no PT_LOAD segments".to_owned());
    }
    if elf
        .load_segments
        .iter()
        .copied()
        .any(LoadSegment::is_writable_and_executable)
    {
        return Err("ELF has a writable-and-executable PT_LOAD segment".to_owned());
    }
    if !elf
        .load_segments
        .iter()
        .copied()
        .any(LoadSegment::is_read_execute)
    {
        return Err("ELF has no read-execute PT_LOAD segment".to_owned());
    }
    if !elf
        .load_segments
        .iter()
        .copied()
        .any(LoadSegment::is_read_write)
    {
        return Err("ELF has no read-write PT_LOAD segment".to_owned());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn elf64_with_segments(flags: &[u32]) -> Vec<u8> {
        let mut bytes = vec![0_u8; 64 + 56 * flags.len()];
        bytes[..4].copy_from_slice(ELF_MAGIC);
        bytes[4] = ELFCLASS64;
        bytes[5] = ELFDATA2LSB;
        bytes[18..20].copy_from_slice(&183_u16.to_le_bytes());
        bytes[24..32].copy_from_slice(&0x800_u64.to_le_bytes());
        bytes[32..40].copy_from_slice(&64_u64.to_le_bytes());
        bytes[54..56].copy_from_slice(&56_u16.to_le_bytes());
        bytes[56..58].copy_from_slice(&(flags.len() as u16).to_le_bytes());
        for (index, flags) in flags.iter().enumerate() {
            let offset = 64 + index * 56;
            bytes[offset..offset + 4].copy_from_slice(&PT_LOAD.to_le_bytes());
            bytes[offset + 4..offset + 8].copy_from_slice(&flags.to_le_bytes());
        }
        bytes
    }

    #[test]
    fn accepts_separate_executable_and_writable_segments() {
        let elf = parse(&elf64_with_segments(&[PF_R | PF_X, PF_R | PF_W])).unwrap();
        assert_eq!(elf.class, Class::Elf64);
        assert_eq!(elf.entry, 0x800);
        validate(&elf).unwrap();
    }

    #[test]
    fn rejects_writable_and_executable_segment() {
        let elf = parse(&elf64_with_segments(&[PF_R | PF_W | PF_X])).unwrap();
        assert_eq!(
            validate(&elf).unwrap_err(),
            "ELF has a writable-and-executable PT_LOAD segment"
        );
    }

    #[test]
    fn rejects_non_elf_and_missing_segment_classes() {
        assert_eq!(parse(b"not an elf").unwrap_err(), "truncated ELF at offset 0x0");
        let elf = parse(&elf64_with_segments(&[PF_R | PF_X])).unwrap();
        assert_eq!(
            validate(&elf).unwrap_err(),
            "ELF has no read-write PT_LOAD segment"
        );
    }
}
