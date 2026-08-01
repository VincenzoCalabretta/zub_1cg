use std::env;
use std::fs;
use std::path::Path;

fn main() {
    let path = match env::args_os().nth(1) {
        Some(path) => path,
        None => {
            eprintln!("Usage: elf_check <firmware.elf>");
            std::process::exit(2);
        }
    };
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
    println!(
        "{}: {} machine={} entry=0x{:x}, {} PT_LOAD segment(s) validated",
        Path::new(&path).display(),
        elf.class,
        elf.machine,
        elf.entry,
        elf.load_segments.len(),
    );
}

fn fail(path: &Path, message: &str) -> ! {
    eprintln!("elf_check: {}: {message}", path.display());
    std::process::exit(1);
}
