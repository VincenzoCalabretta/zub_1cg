use pdf_to_markdown_lib::convert;
use std::env;
use std::path::PathBuf;

fn usage() -> &'static str {
    "Usage: pdf_to_markdown [--source DIR] [--output DIR] [--clean]"
}

fn main() {
    if let Some(workspace) = env::var_os("BUILD_WORKSPACE_DIRECTORY") {
        if let Err(error) = env::set_current_dir(workspace) {
            eprintln!("pdf_to_markdown: cannot enter workspace: {error}");
            std::process::exit(1);
        }
    }
    let mut source = PathBuf::from("docs");
    let mut output = PathBuf::from("documentation/pdf");
    let mut clean = false;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--source" => {
                source = PathBuf::from(args.next().unwrap_or_else(|| {
                    eprintln!("--source requires a directory\n{}", usage());
                    std::process::exit(2)
                }))
            }
            "--output" => {
                output = PathBuf::from(args.next().unwrap_or_else(|| {
                    eprintln!("--output requires a directory\n{}", usage());
                    std::process::exit(2)
                }))
            }
            "--clean" => clean = true,
            "-h" | "--help" => {
                println!("{}", usage());
                return;
            }
            _ => {
                eprintln!("unknown option: {arg}\n{}", usage());
                std::process::exit(2);
            }
        }
    }
    match convert(&source, &output, clean) {
        Ok(count) => eprintln!("Converted {count} PDF(s) into {}", output.display()),
        Err(error) => {
            eprintln!("pdf_to_markdown: {error}");
            std::process::exit(1);
        }
    }
}
