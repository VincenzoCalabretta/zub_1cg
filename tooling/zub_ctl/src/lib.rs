//! Core serial-matching primitives extracted from zub_ctl.
//!
//! These functions operate on plain strings / byte slices and have no I/O
//! dependencies, making them straightforward to unit-test without a board.

use anyhow::{Context, Result};
use regex::Regex;

// ── Regex helpers ─────────────────────────────────────────────────────────────

/// Compile each pattern as an anchored-or-contains regex.  Fails early if any
/// pattern is syntactically invalid.
pub fn compile_regexes(patterns: &[String]) -> Result<Vec<Regex>> {
    patterns
        .iter()
        .map(|p| Regex::new(p).with_context(|| format!("bad regex: {p}")))
        .collect()
}

// ── Line accumulator ──────────────────────────────────────────────────────────

/// Append `bytes` to `buf` (lossy UTF-8) and drain every complete line
/// (terminated by `\n`).  Trailing `\r` is stripped.  Empty lines are
/// discarded.  The caller owns `buf` and retains any partial final line.
pub fn push_bytes_to_lines(buf: &mut String, bytes: &[u8]) -> Vec<String> {
    buf.push_str(&String::from_utf8_lossy(bytes));
    let mut lines = Vec::new();
    while let Some(nl) = buf.find('\n') {
        let raw: String = buf.drain(..=nl).collect();
        let line = raw.trim_end_matches(['\r', '\n']).to_string();
        if !line.is_empty() {
            lines.push(line);
        }
    }
    lines
}

// ── Pattern matching ──────────────────────────────────────────────────────────

/// Outcome returned by [`match_line`].
#[derive(Debug, PartialEq)]
pub enum LineResult {
    /// A `--fail-on` pattern matched; contains the matched pattern string.
    FailOnHit(String),
    /// All remaining expected patterns have been satisfied.
    AllMatched,
    /// No state change — continue reading.
    Continue,
}

/// Match `line` against the active pattern sets.
///
/// Behaviour:
/// * `fail_on` patterns take precedence: the first match short-circuits and
///   returns `FailOnHit`.
/// * `remaining` is an *ordered* list of expected patterns.  Only the **first**
///   element is tested; a match removes it.  When the list empties, returns
///   `AllMatched`.
/// * If neither fires, returns `Continue`.
pub fn match_line(line: &str, remaining: &mut Vec<Regex>, fail_on: &[Regex]) -> LineResult {
    for pat in fail_on {
        if pat.is_match(line) {
            return LineResult::FailOnHit(pat.as_str().to_owned());
        }
    }
    if let Some(pat) = remaining.first() {
        if pat.is_match(line) {
            remaining.remove(0);
            if remaining.is_empty() {
                return LineResult::AllMatched;
            }
        }
    }
    LineResult::Continue
}

// ── xsct TCL generation ───────────────────────────────────────────────────────

/// Build the xsct TCL script for A53 boot.
///
/// Paths are substituted into the script.  Tcl's `{…}` quoting is used which
/// handles all printable characters except literal `{` and `}`.  Bazel output
/// paths never contain braces so this is safe for our use-case; callers that
/// pass arbitrary paths should validate them first.
pub fn build_xsct_tcl(
    elf: &std::path::Path,
    bit: &std::path::Path,
    psinit: &std::path::Path,
) -> String {
    format!(
        r#"connect
puts "Connected. Available targets:"
targets

puts "\nResetting system..."
targets -set -nocase -filter {{name =~ "PSU"}}
rst -system
after 3000
mwr 0xffca0038 0x1ff

puts "Programming PL bitstream: {bit}"
fpga -file {bit}

puts "Running psu_init (clocks, MIO, DDR)..."
targets -set -nocase -filter {{name =~ "APU*"}}
source {psinit}
psu_init
psu_ps_pl_isolation_removal
psu_ps_pl_reset_config
after 1000

puts "Loading ELF on A53 #0: {elf}"
targets -set -nocase -filter {{name =~ "*A53*#0"}}
rst -processor
dow {elf}
con
"#,
        bit = bit.display(),
        psinit = psinit.display(),
        elf = elf.display(),
    )
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    fn pat(s: &str) -> Vec<String> {
        vec![s.to_owned()]
    }
    fn pats(ss: &[&str]) -> Vec<String> {
        ss.iter().map(|s| s.to_string()).collect()
    }

    // ── match_line ────────────────────────────────────────────────────────────

    #[test]
    fn single_pattern_match_returns_all_matched() {
        let mut remaining = compile_regexes(&pat("Hello, World!")).unwrap();
        let result = match_line("Hello, World!", &mut remaining, &[]);
        assert_eq!(result, LineResult::AllMatched);
        assert!(remaining.is_empty());
    }

    #[test]
    fn ordered_matching_requires_sequence() {
        let mut remaining = compile_regexes(&pats(&["Hello", "World"])).unwrap();
        let fail_on = vec![];
        // "World" arrives before "Hello" — should not consume it.
        assert_eq!(
            match_line("World", &mut remaining, &fail_on),
            LineResult::Continue
        );
        assert_eq!(remaining.len(), 2);
        // Now "Hello" arrives and is consumed.
        assert_eq!(
            match_line("Hello", &mut remaining, &fail_on),
            LineResult::Continue
        );
        assert_eq!(remaining.len(), 1);
        // Now "World" finally matches the next expected pattern.
        assert_eq!(
            match_line("World", &mut remaining, &fail_on),
            LineResult::AllMatched
        );
    }

    #[test]
    fn fail_on_takes_precedence_over_expected() {
        let mut remaining = compile_regexes(&pat("Hello")).unwrap();
        let fail_on = compile_regexes(&pat("Error")).unwrap();
        let result = match_line("Error in module", &mut remaining, &fail_on);
        assert!(matches!(result, LineResult::FailOnHit(ref s) if s == "Error"));
        // Expected list is unchanged — fail-on fired first.
        assert_eq!(remaining.len(), 1);
    }

    #[test]
    fn unmatched_line_leaves_state_unchanged() {
        let mut remaining = compile_regexes(&pat("Target")).unwrap();
        let result = match_line("Something else", &mut remaining, &[]);
        assert_eq!(result, LineResult::Continue);
        assert_eq!(remaining.len(), 1);
    }

    #[test]
    fn regex_matches_substring() {
        let mut remaining = compile_regexes(&pat("World")).unwrap();
        let result = match_line("Hello, World! And more text.", &mut remaining, &[]);
        assert_eq!(result, LineResult::AllMatched);
    }

    #[test]
    fn multiple_fail_on_first_match_wins() {
        let mut remaining = compile_regexes(&pat("OK")).unwrap();
        let fail_on = compile_regexes(&pats(&["ErrA", "ErrB"])).unwrap();
        let result = match_line("ErrB triggered", &mut remaining, &fail_on);
        assert!(matches!(result, LineResult::FailOnHit(ref s) if s == "ErrB"));
    }

    // ── push_bytes_to_lines ───────────────────────────────────────────────────

    #[test]
    fn complete_lines_are_returned() {
        let mut buf = String::new();
        let lines = push_bytes_to_lines(&mut buf, b"Hello\nWorld\n");
        assert_eq!(lines, vec!["Hello", "World"]);
        assert!(buf.is_empty());
    }

    #[test]
    fn crlf_terminators_are_stripped() {
        let mut buf = String::new();
        let lines = push_bytes_to_lines(&mut buf, b"Hello\r\nWorld\r\n");
        assert_eq!(lines, vec!["Hello", "World"]);
    }

    #[test]
    fn partial_line_retained_in_buffer() {
        let mut buf = String::new();
        let lines = push_bytes_to_lines(&mut buf, b"Hello");
        assert!(lines.is_empty(), "no newline yet → nothing emitted");
        assert_eq!(buf, "Hello");
    }

    #[test]
    fn bytes_split_across_reads_assemble_lines() {
        let mut buf = String::new();
        let first = push_bytes_to_lines(&mut buf, b"Hel");
        assert!(first.is_empty());
        let second = push_bytes_to_lines(&mut buf, b"lo\nWor");
        assert_eq!(second, vec!["Hello"]);
        let third = push_bytes_to_lines(&mut buf, b"ld\n");
        assert_eq!(third, vec!["World"]);
        assert!(buf.is_empty());
    }

    #[test]
    fn invalid_utf8_handled_lossily() {
        let mut buf = String::new();
        let bytes: &[u8] = b"Line \xFF with bad byte\n";
        let lines = push_bytes_to_lines(&mut buf, bytes);
        assert_eq!(lines.len(), 1);
        assert!(lines[0].contains("Line"));
        assert!(lines[0].contains("with bad byte"));
    }

    #[test]
    fn empty_lines_are_discarded() {
        let mut buf = String::new();
        let lines = push_bytes_to_lines(&mut buf, b"\n\nHello\n\n");
        assert_eq!(lines, vec!["Hello"]);
    }

    // ── build_xsct_tcl ───────────────────────────────────────────────────────

    #[test]
    fn xsct_tcl_contains_all_paths() {
        let elf = Path::new("/out/app.elf");
        let bit = Path::new("/out/design.bit");
        let psinit = Path::new("/board/psu_init.tcl");
        let tcl = build_xsct_tcl(elf, bit, psinit);
        assert!(tcl.contains("/out/app.elf"), "ELF path missing");
        assert!(tcl.contains("/out/design.bit"), "bitstream path missing");
        assert!(tcl.contains("/board/psu_init.tcl"), "psu_init path missing");
    }

    #[test]
    fn xsct_tcl_double_braces_produce_literal_braces() {
        let elf = Path::new("/a.elf");
        let bit = Path::new("/b.bit");
        let psinit = Path::new("/p.tcl");
        let tcl = build_xsct_tcl(elf, bit, psinit);
        // The Tcl filter expressions use literal { }; verify they survived
        // Rust's format! string escape with {{ / }}.
        assert!(tcl.contains(r#"filter {name =~ "PSU"}"#));
        assert!(tcl.contains(r#"filter {name =~ "APU*"}"#));
    }
}
