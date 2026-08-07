//! zub_ctl — test driver for the AES-ZUB-1CG board.
//!
//! Subcommands
//!   serial-watch : open a serial port, print each line, exit 0 when all
//!                  --expect regexes have matched, or 1 on --fail-on / timeout.
//!   watch-r5     : run `openocd -f <cfg> -f <script>` while a serial-watch is
//!                  running in a background thread. openocd's stdout/stderr are
//!                  streamed to this process with an [OCD] prefix.
//!   watch-a53    : run `xsct <inline.tcl>` where the TCL programs the PL
//!                  bitstream, runs psu_init, and loads the ELF onto A53#0.
//!                  Concurrent serial-watch behaves the same as watch-r5.
//!
//! Exit codes
//!   0  every --expect regex matched before timeout
//!   1  timeout expired with unmatched expectations, or a --fail-on regex hit,
//!      or the boot subprocess exited nonzero
//!   2  argument / I/O error

use anyhow::{bail, Context, Result};
use clap::{Args, Parser, Subcommand};
use elf_check_lib::{parse_artifact_hashes, sha256_hex};
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use zub_ctl_lib::{build_xsct_tcl, compile_regexes, match_line, push_bytes_to_lines, LineResult};

// ── CLI ──────────────────────────────────────────────────────────────────────

#[derive(Parser)]
#[command(name = "zub_ctl", version, about = "AES-ZUB-1CG test driver.")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Open a serial port and match expected patterns with a timeout.
    SerialWatch(SerialArgs),
    /// Boot R5 via openocd, then watch UART for expected patterns.
    WatchR5(WatchR5Args),
    /// Boot A53 via xsct, then watch UART for expected patterns.
    WatchA53(WatchA53Args),
    /// Preflight check: TTY, OpenOCD/xsct availability, and artifact hashes.
    Doctor(DoctorArgs),
}

#[derive(Args, Clone)]
struct SerialArgs {
    /// TTY device (e.g. /dev/ttyUSB1).
    #[arg(long)]
    tty: PathBuf,
    /// Baud rate.
    #[arg(long, default_value_t = 115200)]
    baud: u32,
    /// Seconds to wait for all --expect patterns to match.
    #[arg(long, default_value_t = 30)]
    timeout: u64,
    /// Regex(es) that must appear on serial output.  All must match to pass.
    #[arg(long = "expect")]
    expect: Vec<String>,
    /// Regex(es) that indicate immediate failure.  Any match → exit 1.
    #[arg(long = "fail-on")]
    fail_on: Vec<String>,
    /// If set, only print lines without matching (useful for debugging).
    #[arg(long)]
    print_only: bool,
}

#[derive(Args)]
struct WatchR5Args {
    /// Path to openocd binary (default: from PATH).
    #[arg(long, default_value = "openocd")]
    openocd: String,
    /// OpenOCD board/interface config file.
    #[arg(long)]
    openocd_cfg: PathBuf,
    /// OpenOCD scripts to run in order (typically psu_init_run.tcl followed
    /// by load_r5.tcl).
    #[arg(long, required = true)]
    openocd_script: Vec<PathBuf>,
    /// R5 ELF to load.  Injected into OpenOCD as `set ELF <abs-path>` before
    /// the script runs, overriding any hardcoded path inside the script.
    #[arg(long)]
    elf: Option<PathBuf>,
    /// Optional xsct binary to run psu_init before openocd. Fixes MIO/UART
    /// pinmux on boards where the PS state is not restored on power-on.
    #[arg(long)]
    pre_xsct: Option<PathBuf>,
    /// psu_init.tcl (Vitis-generated) — required when --pre-xsct is set.
    #[arg(long)]
    psinit: Option<PathBuf>,
    /// xsct wrapper script that sources psinit — required when --pre-xsct is set.
    #[arg(long)]
    pre_xsct_script: Option<PathBuf>,
    #[command(flatten)]
    serial: SerialArgs,
}

#[derive(Args)]
struct WatchA53Args {
    /// Path to xsct binary.
    #[arg(long)]
    xsct: PathBuf,
    /// A53 ELF to flash.
    #[arg(long)]
    elf: PathBuf,
    /// PL bitstream (.bit).
    #[arg(long)]
    bitstream: PathBuf,
    /// psu_init.tcl.
    #[arg(long)]
    psinit: PathBuf,
    #[command(flatten)]
    serial: SerialArgs,
}

#[derive(Args)]
struct DoctorArgs {
    /// Serial device to probe for presence and permissions.
    #[arg(long, default_value = "/dev/ttyUSB1")]
    tty: PathBuf,
    /// openocd binary (checked for presence and version).
    #[arg(long, default_value = "openocd")]
    openocd: String,
    /// xsct binary (optional; skipped when absent).
    #[arg(long)]
    xsct: Option<PathBuf>,
    /// Directory containing artifacts.json and committed board blobs.
    /// When given, SHA-256 of every listed artifact is compared to the manifest.
    #[arg(long)]
    artifacts_dir: Option<PathBuf>,
}

// ── main ─────────────────────────────────────────────────────────────────────

fn main() {
    let cli = Cli::parse();
    let rc = match cli.cmd {
        Cmd::SerialWatch(a) => run_serial_watch(a),
        Cmd::WatchR5(a) => run_watch_r5(a),
        Cmd::WatchA53(a) => run_watch_a53(a),
        Cmd::Doctor(a) => run_doctor(a),
    };
    match rc {
        Ok(ok) => std::process::exit(if ok { 0 } else { 1 }),
        Err(e) => {
            eprintln!("zub_ctl: {e:#}");
            std::process::exit(2);
        }
    }
}

// ── doctor ───────────────────────────────────────────────────────────────────

fn run_doctor(args: DoctorArgs) -> Result<bool> {
    let mut failed = false;

    // 1. TTY presence.
    let tty_exists = args.tty.exists();
    print_check(
        tty_exists,
        false,
        &format!("TTY device {}", args.tty.display()),
        if tty_exists { "" } else { "device not found" },
    );
    failed |= !tty_exists;

    // 2. TTY read/write permission.
    if tty_exists {
        let perm_ok = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&args.tty)
            .is_ok();
        print_check(
            perm_ok,
            false,
            &format!("TTY permissions (r/w) on {}", args.tty.display()),
            if perm_ok {
                ""
            } else {
                "cannot open for read+write; try: sudo usermod -aG dialout $USER"
            },
        );
        failed |= !perm_ok;
    }

    // 3. USB serial enumeration hint (advisory only).
    let by_id = std::path::Path::new("/dev/serial/by-id");
    if by_id.exists() {
        let ft2232_found = fs::read_dir(by_id)
            .map(|entries| {
                entries
                    .filter_map(|e| e.ok())
                    .any(|e| e.file_name().to_string_lossy().contains("FT2232"))
            })
            .unwrap_or(false);
        print_check(
            ft2232_found,
            true,
            "FT2232H in /dev/serial/by-id",
            if ft2232_found {
                ""
            } else {
                "no FT2232H enumerated; check USB cable / driver"
            },
        );
    }

    // 4. openocd version.
    let ocd_result = Command::new(&args.openocd)
        .arg("--version")
        .stderr(Stdio::piped())
        .stdout(Stdio::piped())
        .output();
    match ocd_result {
        Ok(out) => {
            let ver = String::from_utf8_lossy(&out.stderr);
            let first = ver.lines().next().unwrap_or("").trim().to_string();
            print_check(true, false, &format!("openocd ({first})"), "");
        }
        Err(e) => {
            print_check(
                false,
                false,
                &format!("openocd ({})", args.openocd),
                &format!("{e}"),
            );
            failed = true;
        }
    }

    // 5. xsct (optional).
    match &args.xsct {
        None => print_check_skip("xsct binary (--xsct not provided)"),
        Some(path) => {
            let ok = path.exists();
            print_check(
                ok,
                false,
                &format!("xsct at {}", path.display()),
                if ok { "" } else { "file not found" },
            );
            failed |= !ok;
        }
    }

    // 6. Artifact hash verification (optional).
    match &args.artifacts_dir {
        None => print_check_skip("artifact hash check (--artifacts-dir not provided)"),
        Some(dir) => {
            let manifest_path = dir.join("artifacts.json");
            match fs::read_to_string(&manifest_path) {
                Err(e) => {
                    print_check(
                        false,
                        false,
                        "artifacts.json readable",
                        &format!("{}: {e}", manifest_path.display()),
                    );
                    failed = true;
                }
                Ok(json) => {
                    let entries = parse_artifact_hashes(&json);
                    if entries.is_empty() {
                        print_check(
                            false,
                            false,
                            "artifacts.json has entries",
                            "parse yielded zero (path, sha256) pairs",
                        );
                        failed = true;
                    }
                    for (rel, expected) in &entries {
                        let full = dir.join(rel);
                        match fs::read(&full) {
                            Err(e) => {
                                print_check(
                                    false,
                                    false,
                                    &format!("artifact {rel}"),
                                    &format!("cannot read: {e}"),
                                );
                                failed = true;
                            }
                            Ok(data) => {
                                let actual = sha256_hex(&data);
                                let ok = &actual == expected;
                                let mismatch = if ok {
                                    String::new()
                                } else {
                                    format!("computed {actual}, manifest {expected}")
                                };
                                print_check(
                                    ok,
                                    false,
                                    &format!("artifact {rel} SHA-256"),
                                    &mismatch,
                                );
                                failed |= !ok;
                            }
                        }
                    }
                }
            }
        }
    }

    if failed {
        eprintln!("[doctor] one or more required checks FAILED — fix the issues above before running board tests.");
    } else {
        eprintln!("[doctor] all checks passed.");
    }
    Ok(!failed)
}

fn print_check(ok: bool, advisory: bool, label: &str, detail: &str) {
    if ok {
        eprintln!("[OK  ] {label}");
    } else if advisory {
        eprintln!(
            "[WARN] {label}{}",
            if detail.is_empty() {
                String::new()
            } else {
                format!(" — {detail}")
            }
        );
    } else {
        eprintln!(
            "[FAIL] {label}{}",
            if detail.is_empty() {
                String::new()
            } else {
                format!(" — {detail}")
            }
        );
    }
}

fn print_check_skip(label: &str) {
    eprintln!("[SKIP] {label}");
}

// ── serial-watch (single command) ────────────────────────────────────────────

fn run_serial_watch(args: SerialArgs) -> Result<bool> {
    let (stop, handle) = spawn_serial_watch(&args)?;
    let ok = handle.join().expect("serial watch thread panicked")?;
    // stop is a moved Arc; nothing to do on this path (thread already returned).
    drop(stop);
    Ok(ok)
}

// ── watch-r5 ─────────────────────────────────────────────────────────────────

fn run_watch_r5(args: WatchR5Args) -> Result<bool> {
    // 1. Optional psu_init via xsct BEFORE opening serial or JTAG — this
    //    configures MIO pinmux + PLLs so PS UART TX actually reaches the
    //    FTDI channel-1 pin. xsct owns the JTAG cable during this step,
    //    so openocd cannot be running yet.
    if let Some(xsct) = args.pre_xsct.as_ref() {
        let script = args
            .pre_xsct_script
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("--pre-xsct requires --pre-xsct-script"))?;
        let psinit = args
            .psinit
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("--pre-xsct requires --psinit"))?;
        let psinit_abs = fs::canonicalize(psinit)
            .with_context(|| format!("resolve psu_init: {}", psinit.display()))?;
        eprintln!("[zub_ctl] launching xsct for psu_init…");
        let mut cmd = Command::new(xsct);
        cmd.arg(script).arg(psinit_abs);
        let s = run_subprocess(&mut cmd, "XSCT")?;
        if !s.success() {
            eprintln!("[zub_ctl] xsct psu_init exited with status {s:?}");
            return Ok(false);
        }
    }

    // 2. Start serial reader BEFORE launching openocd so no boot output is lost.
    let (stop, serial_handle) = spawn_serial_watch(&args.serial)?;

    // 3. Spawn openocd as a background child so the serial watch is not blocked
    //    waiting for the JTAG tool to exit.  Inject ELF path before the script
    //    so the script's $ELF variable is set to an absolute path.
    eprintln!("[zub_ctl] launching openocd…");
    let mut cmd = Command::new(&args.openocd);
    cmd.arg("-f").arg(&args.openocd_cfg);
    if let Some(elf) = &args.elf {
        let elf_abs =
            fs::canonicalize(elf).with_context(|| format!("resolve ELF {}", elf.display()))?;
        cmd.arg("--command")
            .arg(format!("set ELF {{{}}}", elf_abs.display()));
    }
    for script in &args.openocd_script {
        cmd.arg("-f").arg(script);
    }
    cmd.stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut ocd_child: Child = cmd.spawn().context("spawn openocd")?;

    let ocd_stdout = ocd_child.stdout.take().unwrap();
    let ocd_stderr = ocd_child.stderr.take().unwrap();
    let t_out = thread::spawn(move || pipe_lines(ocd_stdout, "OCD", false));
    let t_err = thread::spawn(move || pipe_lines(ocd_stderr, "OCD", true));

    // 4. Wait for serial watch (pattern matched or timeout).
    let ok = serial_handle.join().expect("serial thread panicked")?;
    drop(stop);

    // 5. Reap openocd: kill it if still running (normal when the serial pattern
    //    matched before openocd's own `shutdown` call), otherwise collect its
    //    exit status.  A kill is not counted as a failure.
    let ocd_success = match ocd_child.try_wait().context("poll openocd")? {
        Some(status) => {
            if !status.success() {
                eprintln!("[zub_ctl] openocd exited with status {status:?}");
            }
            status.success()
        }
        None => {
            let _ = ocd_child.kill();
            let _ = ocd_child.wait();
            true
        }
    };
    let _ = t_out.join();
    let _ = t_err.join();

    Ok(ok && ocd_success)
}

// ── watch-a53 ────────────────────────────────────────────────────────────────

fn run_watch_a53(args: WatchA53Args) -> Result<bool> {
    // Absolute paths so xsct doesn't confuse them with relative-to-CWD paths.
    let elf = fs::canonicalize(&args.elf)
        .with_context(|| format!("resolve ELF path {}", args.elf.display()))?;
    let bit = fs::canonicalize(&args.bitstream)
        .with_context(|| format!("resolve bitstream {}", args.bitstream.display()))?;
    let psinit = fs::canonicalize(&args.psinit)
        .with_context(|| format!("resolve psu_init.tcl {}", args.psinit.display()))?;

    // 1. Start serial reader first.
    let (stop, handle) = spawn_serial_watch(&args.serial)?;

    // 2. Build the xsct TCL script.
    let tcl = build_xsct_tcl(&elf, &bit, &psinit);

    // 3. Write TCL to a tempfile, run xsct.
    let tmp = std::env::temp_dir().join(format!("zub_ctl_xsct_{}.tcl", std::process::id()));
    fs::write(&tmp, tcl).context("write xsct tcl tempfile")?;
    let _cleanup = TempFileGuard(tmp.clone());

    eprintln!("[zub_ctl] launching xsct…");
    let mut cmd = Command::new(&args.xsct);
    cmd.arg(&tmp);
    let xsct_status = run_subprocess(&mut cmd, "XSCT")?;
    if !xsct_status.success() {
        eprintln!("[zub_ctl] xsct exited with status {xsct_status:?}");
    }

    let ok = handle.join().expect("serial thread panicked")?;
    drop(stop);
    Ok(ok && xsct_status.success())
}

struct TempFileGuard(PathBuf);
impl Drop for TempFileGuard {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

// ── shared: subprocess streaming ─────────────────────────────────────────────

fn run_subprocess(cmd: &mut Command, prefix: &str) -> Result<std::process::ExitStatus> {
    cmd.stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child: Child = cmd.spawn().context("spawn subprocess")?;

    let stdout = child.stdout.take().unwrap();
    let stderr = child.stderr.take().unwrap();
    let p1 = prefix.to_string();
    let p2 = prefix.to_string();
    let t1 = thread::spawn(move || pipe_lines(stdout, &p1, false));
    let t2 = thread::spawn(move || pipe_lines(stderr, &p2, true));

    let status = child.wait().context("wait subprocess")?;
    let _ = t1.join();
    let _ = t2.join();
    Ok(status)
}

fn pipe_lines<R: Read>(r: R, prefix: &str, stderr_channel: bool) {
    let br = BufReader::new(r);
    for line in br.lines() {
        let l = line.unwrap_or_else(|e| format!("<read error: {e}>"));
        let msg = format!("[{prefix}] {l}");
        if stderr_channel {
            let _ = writeln!(std::io::stderr(), "{msg}");
        } else {
            println!("{msg}");
        }
    }
}

// ── shared: serial-watch thread ──────────────────────────────────────────────

fn spawn_serial_watch(args: &SerialArgs) -> Result<(Arc<AtomicBool>, JoinHandle<Result<bool>>)> {
    let remaining = compile_regexes(&args.expect)?;
    let fail_on = compile_regexes(&args.fail_on)?;

    let tty = args.tty.clone();
    let baud = args.baud;
    let timeout = Duration::from_secs(args.timeout);
    let print_only = args.print_only;

    // Fail fast if the device is missing so callers see the error before
    // proceeding to spawn openocd / xsct.
    if !tty.exists() {
        bail!("serial device does not exist: {}", tty.display());
    }

    let port = serialport::new(tty.to_string_lossy(), baud)
        .timeout(Duration::from_millis(200))
        .data_bits(serialport::DataBits::Eight)
        .parity(serialport::Parity::None)
        .stop_bits(serialport::StopBits::One)
        .flow_control(serialport::FlowControl::None)
        .open()
        .with_context(|| format!("open serial port {}", tty.display()))?;

    let stop = Arc::new(AtomicBool::new(false));
    let stop_clone = stop.clone();

    let handle = thread::spawn(move || -> Result<bool> {
        let mut port = port;
        let start = Instant::now();
        let mut remaining = remaining;
        let mut line_buf = String::new();
        let mut byte_buf = [0u8; 256];

        loop {
            if stop_clone.load(Ordering::Relaxed) {
                eprintln!("[zub_ctl] serial-watch: stop requested");
                break;
            }
            if start.elapsed() >= timeout {
                if !remaining.is_empty() && !print_only {
                    eprintln!(
                        "[zub_ctl] serial-watch: timeout after {}s, {} unmatched pattern(s):",
                        timeout.as_secs(),
                        remaining.len(),
                    );
                    for pat in &remaining {
                        eprintln!("  ✗ {}", pat.as_str());
                    }
                    return Ok(false);
                }
                break;
            }

            match port.read(&mut byte_buf) {
                Ok(0) => continue,
                Ok(n) => {
                    for line in push_bytes_to_lines(&mut line_buf, &byte_buf[..n]) {
                        println!("[SERIAL] {line}");
                        if print_only {
                            continue;
                        }
                        let prev_len = remaining.len();
                        let next_pat = remaining.first().map(|p| p.as_str().to_owned());
                        match match_line(&line, &mut remaining, &fail_on) {
                            LineResult::FailOnHit(pat) => {
                                eprintln!("[zub_ctl] serial-watch: fail-on regex hit: {pat}");
                                return Ok(false);
                            }
                            LineResult::AllMatched => {
                                eprintln!("[zub_ctl] serial-watch: all patterns matched.");
                                return Ok(true);
                            }
                            LineResult::Continue => {
                                if remaining.len() < prev_len {
                                    if let Some(pat) = next_pat {
                                        eprintln!("[zub_ctl]   ✓ matched: {pat}");
                                    }
                                }
                            }
                        }
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => continue,
                Err(e) => bail!("serial read error: {e}"),
            }
        }
        Ok(print_only || remaining.is_empty())
    });

    Ok((stop, handle))
}
