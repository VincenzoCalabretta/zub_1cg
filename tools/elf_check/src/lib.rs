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
const SHT_SYMTAB: u32 = 2;
const SHT_DYNSYM: u32 = 11;
const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

pub const EM_ARM: u16 = 40;
pub const EM_AARCH64: u16 = 183;

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
    pub virtual_address: u64,
    pub memory_size: u64,
    pub alignment: u64,
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
    pub symbols: Vec<Symbol>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Symbol {
    pub name: String,
    pub value: u64,
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
                virtual_address: match class {
                    Class::Elf32 => u32_at(bytes, offset + 8)? as u64,
                    Class::Elf64 => u64_at(bytes, offset + 16)?,
                },
                memory_size: match class {
                    Class::Elf32 => u32_at(bytes, offset + 20)? as u64,
                    Class::Elf64 => u64_at(bytes, offset + 40)?,
                },
                alignment: match class {
                    Class::Elf32 => u32_at(bytes, offset + 28)? as u64,
                    Class::Elf64 => u64_at(bytes, offset + 48)?,
                },
            });
        }
    }
    let symbols = parse_symbols(bytes, class)?;
    Ok(Elf {
        class,
        machine,
        entry,
        load_segments,
        symbols,
    })
}

fn parse_symbols(bytes: &[u8], class: Class) -> Result<Vec<Symbol>, String> {
    let (section_header_offset, section_header_size, section_header_count) = match class {
        Class::Elf32 => (
            u32_at(bytes, 32)? as u64,
            u16_at(bytes, 46)? as usize,
            u16_at(bytes, 48)? as usize,
        ),
        Class::Elf64 => (
            u64_at(bytes, 40)?,
            u16_at(bytes, 58)? as usize,
            u16_at(bytes, 60)? as usize,
        ),
    };
    let minimum_section_header_size = match class {
        Class::Elf32 => 40,
        Class::Elf64 => 64,
    };
    if section_header_count == 0 {
        return Ok(Vec::new());
    }
    if section_header_size < minimum_section_header_size {
        return Err(format!(
            "section header entry is too small: {section_header_size} bytes"
        ));
    }
    let section_header_offset =
        usize::try_from(section_header_offset).map_err(|_| "section header offset is too large")?;
    let mut symbols = Vec::new();
    for index in 0..section_header_count {
        let offset = section_header_offset
            .checked_add(
                index
                    .checked_mul(section_header_size)
                    .ok_or_else(|| "section header offset overflow")?,
            )
            .ok_or_else(|| "section header offset overflow")?;
        byte_range(bytes, offset, section_header_size)?;
        let section_type = u32_at(bytes, offset + 4)?;
        if section_type != SHT_SYMTAB && section_type != SHT_DYNSYM {
            continue;
        }
        let (section_offset, section_size, string_table_index, entry_size) = match class {
            Class::Elf32 => (
                u32_at(bytes, offset + 16)? as u64,
                u32_at(bytes, offset + 20)? as u64,
                u32_at(bytes, offset + 24)? as usize,
                u32_at(bytes, offset + 36)? as u64,
            ),
            Class::Elf64 => (
                u64_at(bytes, offset + 24)?,
                u64_at(bytes, offset + 32)?,
                u32_at(bytes, offset + 40)? as usize,
                u64_at(bytes, offset + 56)?,
            ),
        };
        let minimum_symbol_size = match class {
            Class::Elf32 => 16,
            Class::Elf64 => 24,
        };
        let entry_size =
            usize::try_from(entry_size).map_err(|_| "symbol entry size is too large")?;
        if entry_size < minimum_symbol_size {
            return Err(format!("symbol entry is too small: {entry_size} bytes"));
        }
        let string_table_offset = section_header_offset
            .checked_add(
                string_table_index
                    .checked_mul(section_header_size)
                    .ok_or_else(|| "string table header offset overflow")?,
            )
            .ok_or_else(|| "string table header offset overflow")?;
        byte_range(bytes, string_table_offset, section_header_size)?;
        let (strings_offset, strings_size) = match class {
            Class::Elf32 => (
                u32_at(bytes, string_table_offset + 16)? as u64,
                u32_at(bytes, string_table_offset + 20)? as u64,
            ),
            Class::Elf64 => (
                u64_at(bytes, string_table_offset + 24)?,
                u64_at(bytes, string_table_offset + 32)?,
            ),
        };
        let section_offset =
            usize::try_from(section_offset).map_err(|_| "symbol table offset is too large")?;
        let section_size =
            usize::try_from(section_size).map_err(|_| "symbol table size is too large")?;
        let strings = byte_range(
            bytes,
            usize::try_from(strings_offset).map_err(|_| "string table offset is too large")?,
            usize::try_from(strings_size).map_err(|_| "string table size is too large")?,
        )?;
        if section_size % entry_size != 0 {
            return Err("symbol table size is not a multiple of its entry size".to_owned());
        }
        for symbol_offset in
            (section_offset..section_offset.saturating_add(section_size)).step_by(entry_size)
        {
            let symbol = byte_range(bytes, symbol_offset, entry_size)?;
            let name_offset = u32_at(symbol, 0)? as usize;
            let (value, section_index) = match class {
                Class::Elf32 => (u32_at(symbol, 4)? as u64, u16_at(symbol, 14)?),
                Class::Elf64 => (u64_at(symbol, 8)?, u16_at(symbol, 6)?),
            };
            if section_index == 0 || name_offset >= strings.len() {
                continue;
            }
            let end = strings[name_offset..]
                .iter()
                .position(|byte| *byte == 0)
                .map(|length| name_offset + length)
                .unwrap_or(strings.len());
            if let Ok(name) = std::str::from_utf8(&strings[name_offset..end]) {
                symbols.push(Symbol {
                    name: name.to_owned(),
                    value,
                });
            }
        }
    }
    Ok(symbols)
}

/// Validates the hardened firmware segment layout.
///
/// Zero-size segments are excluded from all checks: they are linker artefacts
/// that carry no address or permission information.
pub fn validate(elf: &Elf) -> Result<(), String> {
    let non_empty: Vec<LoadSegment> = elf
        .load_segments
        .iter()
        .copied()
        .filter(|s| s.memory_size > 0)
        .collect();
    if non_empty.is_empty() {
        return Err("ELF has no PT_LOAD segments with non-zero size".to_owned());
    }
    if non_empty.iter().copied().any(LoadSegment::is_writable_and_executable) {
        return Err("ELF has a writable-and-executable PT_LOAD segment".to_owned());
    }
    if !non_empty.iter().copied().any(LoadSegment::is_read_execute) {
        return Err("ELF has no read-execute PT_LOAD segment".to_owned());
    }
    Ok(())
}

/// Validates the target architecture and entry point expected by a firmware target.
pub fn validate_identity(elf: &Elf, machine: u16, entry: u64) -> Result<(), String> {
    if elf.machine != machine {
        return Err(format!(
            "ELF machine is {}, expected {machine}",
            elf.machine
        ));
    }
    if elf.entry != entry {
        return Err(format!(
            "ELF entry point is 0x{:x}, expected 0x{entry:x}",
            elf.entry
        ));
    }
    Ok(())
}

/// Validates load-segment addresses and alignments against a target memory region.
///
/// Zero-size segments (MemSiz = 0) are skipped: they carry no address
/// information and appear as a linker artefact when a PHDR is declared but no
/// input sections are placed in it (e.g. an empty .data segment).
pub fn validate_memory(elf: &Elf, start: u64, end: u64) -> Result<(), String> {
    if start >= end {
        return Err("memory range must have a non-zero size".to_owned());
    }
    for segment in &elf.load_segments {
        if segment.memory_size == 0 {
            continue;
        }
        let segment_end = segment
            .virtual_address
            .checked_add(segment.memory_size)
            .ok_or_else(|| "PT_LOAD segment address overflows".to_owned())?;
        if segment.virtual_address < start || segment_end > end {
            return Err(format!(
                "PT_LOAD segment 0x{:x}..0x{:x} is outside 0x{start:x}..0x{end:x}",
                segment.virtual_address, segment_end
            ));
        }
        if segment.alignment != 0 && !segment.alignment.is_power_of_two() {
            return Err(format!(
                "PT_LOAD segment at 0x{:x} has non-power-of-two alignment 0x{:x}",
                segment.virtual_address, segment.alignment
            ));
        }
    }
    Ok(())
}

/// Validates that a named, defined startup symbol is present in the ELF.
pub fn validate_required_symbol(elf: &Elf, required: &str) -> Result<(), String> {
    if elf.symbols.iter().any(|symbol| symbol.name == required) {
        Ok(())
    } else {
        Err(format!("ELF is missing required symbol {required}"))
    }
}

// ── SHA-256 and artifact manifest parsing ────────────────────────────────────

use sha2::{Digest, Sha256};

pub fn sha256_hex(data: &[u8]) -> String {
    Sha256::digest(data)
        .iter()
        .fold(String::with_capacity(64), |mut s, b| {
            use std::fmt::Write;
            let _ = write!(s, "{b:02x}");
            s
        })
}

/// Extracts (path, sha256) pairs from the board `artifacts.json` manifest.
///
/// Recursively scans all JSON objects and collects those that directly own both
/// a `"path"` and a `"sha256"` field (i.e., the leaf artifact records, not the
/// enclosing document or board objects).
pub fn parse_artifact_hashes(json: &str) -> Vec<(String, String)> {
    let mut results = Vec::new();
    collect_artifact_objects(json, &mut results);
    results
}

fn collect_artifact_objects(s: &str, results: &mut Vec<(String, String)>) {
    let mut remaining = s;
    while let Some(obj_start) = remaining.find('{') {
        remaining = &remaining[obj_start + 1..];
        let depth_end = object_end(remaining);
        let obj = &remaining[..depth_end];
        remaining = &remaining[depth_end + 1..];
        // Recurse into nested objects first (depth-first).
        collect_artifact_objects(obj, results);
        // Check if this object directly owns "path" and "sha256" by looking at
        // a flattened copy where all nested {} are replaced with {}.
        let flat = flatten_nested_objects(obj);
        if let (Some(path), Some(hash)) = (
            extract_json_string(&flat, "\"path\""),
            extract_json_string(&flat, "\"sha256\""),
        ) {
            results.push((path, hash));
        }
    }
}

fn flatten_nested_objects(s: &str) -> String {
    let mut result = String::new();
    let mut remaining = s;
    while let Some(start) = remaining.find('{') {
        result.push_str(&remaining[..start]);
        result.push_str("{}");
        remaining = &remaining[start + 1..];
        let end = object_end(remaining);
        remaining = &remaining[end + 1..];
    }
    result.push_str(remaining);
    result
}

fn object_end(s: &str) -> usize {
    let mut depth = 1usize;
    let mut in_string = false;
    let mut escaped = false;
    for (i, c) in s.char_indices() {
        if escaped {
            escaped = false;
            continue;
        }
        if c == '\\' && in_string {
            escaped = true;
            continue;
        }
        if c == '"' {
            in_string = !in_string;
            continue;
        }
        if in_string {
            continue;
        }
        match c {
            '{' => depth += 1,
            '}' => {
                depth -= 1;
                if depth == 0 {
                    return i;
                }
            }
            _ => {}
        }
    }
    s.len()
}

fn extract_json_string(obj: &str, key: &str) -> Option<String> {
    let key_pos = obj.find(key)?;
    let after_key = &obj[key_pos + key.len()..];
    let after_colon = after_key[after_key.find(':')? + 1..].trim_start();
    if !after_colon.starts_with('"') {
        return None;
    }
    let inner = &after_colon[1..];
    let mut result = String::new();
    let mut escaped = false;
    for c in inner.chars() {
        if escaped {
            escaped = false;
            result.push(c);
            continue;
        }
        if c == '\\' {
            escaped = true;
            continue;
        }
        if c == '"' {
            return Some(result);
        }
        result.push(c);
    }
    None
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
            // ELF64 p_memsz at PHDR offset 40: set non-zero so segments are
            // not silently skipped by the zero-size filter in validate/validate_memory.
            bytes[offset + 40..offset + 48].copy_from_slice(&0x1000_u64.to_le_bytes());
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
    fn rejects_non_elf_input() {
        assert_eq!(
            parse(b"not an elf").unwrap_err(),
            "truncated ELF at offset 0x0"
        );
    }

    #[test]
    fn accepts_executable_only_segment() {
        // Text-only firmware (e.g. with only .noinit writable memory, no PT_LOAD
        // for writable data) must pass — W+X absence is the critical check.
        let elf = parse(&elf64_with_segments(&[PF_R | PF_X])).unwrap();
        validate(&elf).unwrap();
    }

    #[test]
    fn rejects_elf_with_no_nonempty_segments() {
        // An ELF whose only PT_LOAD segments have MemSiz = 0 is rejected.
        // This matches the case where an empty PHDR creates a spurious segment.
        let elf = Elf {
            class: Class::Elf64,
            machine: EM_AARCH64,
            entry: 0x800,
            load_segments: vec![LoadSegment {
                flags: PF_R | PF_W,
                virtual_address: 0,
                memory_size: 0,
                alignment: 0x1000,
            }],
            symbols: Vec::new(),
        };
        assert_eq!(
            validate(&elf).unwrap_err(),
            "ELF has no PT_LOAD segments with non-zero size"
        );
    }

    #[test]
    fn rejects_writable_only_segment() {
        // A PT_LOAD with only RW (no RX) is rejected — firmware must be executable.
        let elf = parse(&elf64_with_segments(&[PF_R | PF_W])).unwrap();
        assert_eq!(
            validate(&elf).unwrap_err(),
            "ELF has no read-execute PT_LOAD segment"
        );
    }

    #[test]
    fn skips_zero_size_segments_in_memory_range_check() {
        // A zero-size segment at VMA 0x0 must not cause a range-check failure
        // even when the valid range is entirely above 0x0.
        let elf = Elf {
            class: Class::Elf64,
            machine: EM_AARCH64,
            entry: 0xffff0000,
            load_segments: vec![
                LoadSegment {
                    flags: PF_R | PF_X,
                    virtual_address: 0xffff0000,
                    memory_size: 0x1000,
                    alignment: 0x1000,
                },
                LoadSegment {
                    flags: PF_R | PF_W,
                    virtual_address: 0x0,
                    memory_size: 0, // spurious empty data segment
                    alignment: 0x1000,
                },
            ],
            symbols: Vec::new(),
        };
        validate_memory(&elf, 0xffff0000, 0x1_0000_0000).unwrap();
    }

    #[test]
    fn rejects_unexpected_machine_and_entry_point() {
        let elf = parse(&elf64_with_segments(&[PF_R | PF_X, PF_R | PF_W])).unwrap();
        assert_eq!(
            validate_identity(&elf, EM_ARM, 0x800).unwrap_err(),
            "ELF machine is 183, expected 40"
        );
        assert_eq!(
            validate_identity(&elf, EM_AARCH64, 0).unwrap_err(),
            "ELF entry point is 0x800, expected 0x0"
        );
    }

    #[test]
    fn rejects_out_of_range_or_misaligned_load_segments() {
        let elf = Elf {
            class: Class::Elf64,
            machine: EM_AARCH64,
            entry: 0,
            symbols: Vec::new(),
            load_segments: vec![LoadSegment {
                flags: PF_R | PF_X,
                virtual_address: 0x8000,
                memory_size: 0x1000,
                alignment: 0x1000,
            }],
        };
        assert_eq!(
            validate_memory(&elf, 0, 0x8000).unwrap_err(),
            "PT_LOAD segment 0x8000..0x9000 is outside 0x0..0x8000"
        );
        let mut misaligned = elf.clone();
        misaligned.load_segments[0].virtual_address = 0;
        misaligned.load_segments[0].alignment = 3;
        assert_eq!(
            validate_memory(&misaligned, 0, 0x8000).unwrap_err(),
            "PT_LOAD segment at 0x0 has non-power-of-two alignment 0x3"
        );
    }

    #[test]
    fn requires_startup_symbols() {
        let elf = Elf {
            class: Class::Elf64,
            machine: EM_AARCH64,
            entry: 0,
            load_segments: Vec::new(),
            symbols: vec![Symbol {
                name: "_vector_table".to_owned(),
                value: 0,
            }],
        };
        validate_required_symbol(&elf, "_vector_table").unwrap();
        assert_eq!(
            validate_required_symbol(&elf, "reset_handler").unwrap_err(),
            "ELF is missing required symbol reset_handler"
        );
    }

    #[test]
    fn sha256_matches_known_vectors() {
        // Standard SHA-256 test vectors (all 64 lowercase hex characters = 32 bytes)
        assert_eq!(
            sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(
            sha256_hex(b""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            sha256_hex(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );
    }

    #[test]
    fn parse_artifact_hashes_extracts_fields() {
        let json = r#"{"artifacts":[
            {"path":"foo.bit","role":"bitstream","sha256":"aabbcc"},
            {"path":"bar.tcl","role":"init","sha256":"112233"}
        ]}"#;
        let hashes = parse_artifact_hashes(json);
        assert_eq!(hashes.len(), 2);
        assert_eq!(hashes[0], ("foo.bit".into(), "aabbcc".into()));
        assert_eq!(hashes[1], ("bar.tcl".into(), "112233".into()));
    }

    #[test]
    fn parse_artifact_hashes_skips_objects_without_both_fields() {
        let json = r#"{"board":{"model":"X"},"artifacts":[{"path":"a","sha256":"1234"}]}"#;
        let hashes = parse_artifact_hashes(json);
        // board object has no sha256, outer document object is filtered out;
        // only the leaf artifact object is collected
        assert_eq!(hashes.len(), 1);
        assert_eq!(hashes[0].0, "a");
    }
}
