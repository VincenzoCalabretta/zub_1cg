use std::env;
use std::fs;
use std::path::Path;

fn main() {
    let mut expected_machine = None;
    let mut expected_entry = None;
    let mut path = None;
    let mut args = env::args_os().skip(1);
    while let Some(arg) = args.next() {
        match arg.to_string_lossy().as_ref() {
            "--machine" => {
                let value = next_value(&mut args, "--machine");
                expected_machine = Some(match value.as_str() {
                    "arm" => elf_check_lib::EM_ARM,
                    "aarch64" => elf_check_lib::EM_AARCH64,
                    _ => usage_error("--machine must be arm or aarch64"),
                });
            }
            "--entry" => {
                let value = next_value(&mut args, "--entry");
                expected_entry = Some(parse_entry(&value));
            }
            "-h" | "--help" => {
                println!("Usage: elf_check [--machine arm|aarch64] [--entry ADDRESS] <firmware.elf>");
                return;
            }
            _ if path.is_none() => path = Some(arg),
            _ => usage_error("expected one firmware path"),
        }
    };
    let path = path.unwrap_or_else(|| usage_error("missing firmware path"));
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) => fail(Path::new(&path), &error.to_string()),
    };
    let elf = match elf_check_lib::parse(&bytes) {
        Ok(elf) => elf,
        Err(error) => fail(Path::new(&path), &error),
    };
    if let Err(error) = elf_check_lib::validate(&elf) {
        fail(Path::new(&path), &error);
    }
    if let (Some(machine), Some(entry)) = (expected_machine, expected_entry) {
        if let Err(error) = elf_check_lib::validate_identity(&elf, machine, entry) {
            fail(Path::new(&path), &error);
        }
    } else if expected_machine.is_some() || expected_entry.is_some() {
        usage_error("--machine and --entry must be used together");
    }
    println!(
        "{}: {} machine={} entry=0x{:x}, {} PT_LOAD segment(s) validated",
        Path::new(&path).display(),
        elf.class,
        elf.machine,
        elf.entry,
        elf.load_segments.len(),
    );
}

fn next_value(args: &mut impl Iterator<Item = std::ffi::OsString>, option: &str) -> String {
    args.next()
        .map(|value| value.to_string_lossy().into_owned())
        .unwrap_or_else(|| usage_error(&format!("{option} requires a value")))
}

fn parse_entry(value: &str) -> u64 {
    let value = value.strip_prefix("0x").unwrap_or(value);
    u64::from_str_radix(value, 16).unwrap_or_else(|_| usage_error("--entry must be hexadecimal"))
}

fn usage_error(message: &str) -> ! {
    eprintln!("elf_check: {message}\nUsage: elf_check [--machine arm|aarch64] [--entry ADDRESS] <firmware.elf>");
    std::process::exit(2);
}

fn fail(path: &Path, message: &str) -> ! {
    eprintln!("elf_check: {}: {message}", path.display());
    std::process::exit(1);
}
