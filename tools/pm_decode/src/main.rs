//! Decode a Cortex-R5F postmortem record produced by board/rpu/postmortem.c.
//!
//! Usage:
//!   pm_decode [<dump.bin>] [--elf <elf_file>]
//!
//! The tool scans the input for the PM_MAGIC sentinel (0x504D0001) at any
//! 4-byte-aligned offset, verifies the CRC32, and prints a human-readable
//! report.  If --elf is given, it calls arm-none-eabi-addr2line to annotate
//! the fault PC and LR with source locations.
//!
//! Exit codes: 0 = valid record found and printed, 1 = not found or invalid.

use std::io::{self, Read};
use std::process::{self, Command};

// ── Record constants (must match board/rpu/postmortem.h) ──────────────

const PM_MAGIC: u32 = 0x504D_0001;
const PM_VERSION: u32 = 1;

const PM_EXC_UNDEF: u32 = 1;
const PM_EXC_PREFETCH: u32 = 2;
const PM_EXC_DABT: u32 = 3;

// Byte offsets within pm_record_t (little-endian, no padding).
const OFF_MAGIC: usize = 0;
const OFF_VERSION: usize = 4;
const OFF_EXC_TYPE: usize = 8;
const OFF_CPSR: usize = 12;
const OFF_PC: usize = 16;
const OFF_LR_RAW: usize = 20;
const OFF_R: usize = 24; // r[0..13] = 52 bytes
const OFF_DFSR: usize = 76;
const OFF_DFAR: usize = 80;
const OFF_IFSR: usize = 84;
const OFF_CRC32: usize = 88;
const RECORD_SIZE: usize = 92;

// ── CRC32 (ISO 3309 / Ethernet polynomial) ────────────────────────────

fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &byte in data {
        let mut b = byte;
        for _ in 0..8 {
            if (crc ^ b as u32) & 1 != 0 {
                crc = (crc >> 1) ^ 0xEDB8_8320;
            } else {
                crc >>= 1;
            }
            b >>= 1;
        }
    }
    crc ^ 0xFFFF_FFFF
}

// ── Helpers ───────────────────────────────────────────────────────────

fn le32(buf: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(buf[offset..offset + 4].try_into().unwrap())
}

fn exc_name(exc_type: u32) -> &'static str {
    match exc_type {
        PM_EXC_UNDEF => "undefined-instruction",
        PM_EXC_PREFETCH => "prefetch-abort",
        PM_EXC_DABT => "data-abort",
        _ => "unknown",
    }
}

fn cpsr_mode(cpsr: u32) -> &'static str {
    match cpsr & 0x1F {
        0x10 => "USR",
        0x11 => "FIQ",
        0x12 => "IRQ",
        0x13 => "SVC",
        0x17 => "ABT",
        0x1B => "UND",
        0x1F => "SYS",
        _ => "???",
    }
}

// ── addr2line lookup ──────────────────────────────────────────────────

fn addr2line(elf: &str, addr: u32) -> Option<String> {
    let out = Command::new("arm-none-eabi-addr2line")
        .args(["-f", "-e", elf, &format!("{addr:#010x}")])
        .output()
        .ok()?;
    if out.status.success() {
        let s = String::from_utf8_lossy(&out.stdout);
        Some(s.trim().replace('\n', " @ "))
    } else {
        None
    }
}

// ── Record parsing and printing ───────────────────────────────────────

fn try_parse_and_print(buf: &[u8], elf: Option<&str>) -> bool {
    if buf.len() < RECORD_SIZE {
        return false;
    }
    if le32(buf, OFF_MAGIC) != PM_MAGIC {
        return false;
    }
    if le32(buf, OFF_VERSION) != PM_VERSION {
        eprintln!("pm_decode: unsupported record version {}", le32(buf, OFF_VERSION));
        return false;
    }
    let stored_crc = le32(buf, OFF_CRC32);
    let computed_crc = crc32(&buf[..OFF_CRC32]);
    let crc_ok = computed_crc == stored_crc;

    let exc_type = le32(buf, OFF_EXC_TYPE);
    let cpsr = le32(buf, OFF_CPSR);
    let pc = le32(buf, OFF_PC);
    let lr_raw = le32(buf, OFF_LR_RAW);
    let dfsr = le32(buf, OFF_DFSR);
    let dfar = le32(buf, OFF_DFAR);
    let ifsr = le32(buf, OFF_IFSR);
    let mut r = [0u32; 13];
    for (i, reg) in r.iter_mut().enumerate() {
        *reg = le32(buf, OFF_R + i * 4);
    }

    println!("[POSTMORTEM DECODED]");
    println!("  exception : {} ({})", exc_name(exc_type), exc_type);
    println!("  cpsr      : {cpsr:#010x}  mode={}", cpsr_mode(cpsr));
    print!("  pc        : {pc:#010x}");
    if let Some(elf_path) = elf {
        if let Some(sym) = addr2line(elf_path, pc) {
            print!("  <- {sym}");
        }
    }
    println!();
    print!("  lr        : {lr_raw:#010x}");
    if let Some(elf_path) = elf {
        if let Some(sym) = addr2line(elf_path, lr_raw) {
            print!("  <- {sym}");
        }
    }
    println!();
    for (i, &v) in r.iter().enumerate() {
        println!("  r{i:<2}       : {v:#010x}");
    }
    println!("  dfsr      : {dfsr:#010x}");
    println!("  dfar      : {dfar:#010x}");
    println!("  ifsr      : {ifsr:#010x}");
    println!(
        "  crc32     : {stored_crc:#010x}  {}",
        if crc_ok { "OK" } else { "BAD (record may be corrupt)" }
    );
    crc_ok
}

// ── Entry point ───────────────────────────────────────────────────────

fn main() {
    let args: Vec<String> = std::env::args().collect();

    let mut input_path: Option<&str> = None;
    let mut elf_path: Option<&str> = None;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--elf" => {
                i += 1;
                if i < args.len() {
                    elf_path = Some(&args[i]);
                }
            }
            arg if !arg.starts_with('-') => {
                input_path = Some(arg);
            }
            other => {
                eprintln!("pm_decode: unknown option '{other}'");
                process::exit(1);
            }
        }
        i += 1;
    }

    let buf: Vec<u8> = if let Some(path) = input_path {
        std::fs::read(path).unwrap_or_else(|e| {
            eprintln!("pm_decode: cannot read '{path}': {e}");
            process::exit(1);
        })
    } else {
        let mut b = Vec::new();
        io::stdin().read_to_end(&mut b).unwrap_or_else(|e| {
            eprintln!("pm_decode: cannot read stdin: {e}");
            process::exit(1);
        });
        b
    };

    // Scan 4-byte-aligned offsets for a valid record.
    let mut found = false;
    let mut offset = 0;
    while offset + RECORD_SIZE <= buf.len() {
        if le32(&buf, offset) == PM_MAGIC && try_parse_and_print(&buf[offset..], elf_path) {
            found = true;
            break;
        }
        offset += 4;
    }

    if !found {
        eprintln!("pm_decode: no valid postmortem record found in input");
        process::exit(1);
    }
}

// ── Unit tests ────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn make_record(exc_type: u32, pc: u32, corrupt_crc: bool) -> Vec<u8> {
        let mut buf = vec![0u8; RECORD_SIZE];
        // magic
        buf[OFF_MAGIC..OFF_MAGIC + 4].copy_from_slice(&PM_MAGIC.to_le_bytes());
        // version
        buf[OFF_VERSION..OFF_VERSION + 4].copy_from_slice(&PM_VERSION.to_le_bytes());
        // exc_type
        buf[OFF_EXC_TYPE..OFF_EXC_TYPE + 4].copy_from_slice(&exc_type.to_le_bytes());
        // cpsr (SVC mode, I bit set)
        let cpsr: u32 = 0x80000013;
        buf[OFF_CPSR..OFF_CPSR + 4].copy_from_slice(&cpsr.to_le_bytes());
        // pc
        buf[OFF_PC..OFF_PC + 4].copy_from_slice(&pc.to_le_bytes());
        // lr_raw = pc + 4
        let lr: u32 = pc + 4;
        buf[OFF_LR_RAW..OFF_LR_RAW + 4].copy_from_slice(&lr.to_le_bytes());
        // r[0..13] = index values
        for i in 0..13usize {
            let v = i as u32 * 0x11;
            buf[OFF_R + i * 4..OFF_R + i * 4 + 4].copy_from_slice(&v.to_le_bytes());
        }
        // Compute and store CRC
        let crc = crc32(&buf[..OFF_CRC32]);
        buf[OFF_CRC32..OFF_CRC32 + 4]
            .copy_from_slice(&(if corrupt_crc { crc ^ 1 } else { crc }).to_le_bytes());
        buf
    }

    #[test]
    fn crc32_empty() {
        assert_eq!(crc32(&[]), 0x0000_0000);
    }

    #[test]
    fn crc32_known() {
        // CRC32 of "123456789" is 0xCBF43926.
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn magic_mismatch_rejected() {
        let mut buf = make_record(PM_EXC_UNDEF, 0xffff_0100, false);
        buf[0] = 0xAA; // corrupt magic
        assert!(!try_parse_and_print(&buf, None));
    }

    #[test]
    fn corrupt_crc_rejected() {
        let buf = make_record(PM_EXC_UNDEF, 0xffff_0100, true);
        // try_parse_and_print returns false when CRC is bad
        assert!(!try_parse_and_print(&buf, None));
    }

    #[test]
    fn valid_undef_record_accepted() {
        let buf = make_record(PM_EXC_UNDEF, 0xffff_0140, false);
        assert!(try_parse_and_print(&buf, None));
    }

    #[test]
    fn valid_dabt_record_accepted() {
        let buf = make_record(PM_EXC_DABT, 0xffff_0200, false);
        assert!(try_parse_and_print(&buf, None));
    }

    #[test]
    fn scan_finds_record_at_offset() {
        let record = make_record(PM_EXC_PREFETCH, 0xffff_01c0, false);
        // Pad with 16 bytes before the record
        let mut buf = vec![0u8; 16];
        buf.extend_from_slice(&record);
        let mut found = false;
        let mut offset = 0;
        while offset + RECORD_SIZE <= buf.len() {
            if le32(&buf, offset) == PM_MAGIC && try_parse_and_print(&buf[offset..], None) {
                found = true;
                break;
            }
            offset += 4;
        }
        assert!(found);
    }

    #[test]
    fn exc_names_correct() {
        assert_eq!(exc_name(PM_EXC_UNDEF), "undefined-instruction");
        assert_eq!(exc_name(PM_EXC_PREFETCH), "prefetch-abort");
        assert_eq!(exc_name(PM_EXC_DABT), "data-abort");
        assert_eq!(exc_name(99), "unknown");
    }

    #[test]
    fn cpsr_modes_correct() {
        assert_eq!(cpsr_mode(0x13), "SVC");
        assert_eq!(cpsr_mode(0x1B), "UND");
        assert_eq!(cpsr_mode(0x17), "ABT");
        assert_eq!(cpsr_mode(0x1F), "SYS");
        assert_eq!(cpsr_mode(0x11), "FIQ");
        assert_eq!(cpsr_mode(0x12), "IRQ");
    }

    #[test]
    fn record_size_constant() {
        // The constant must match the C struct sizeof(pm_record_t) = 92.
        assert_eq!(RECORD_SIZE, 92);
    }
}
