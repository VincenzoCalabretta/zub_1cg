use orbtrace::{
    length_frame, stats_from_le, Command, Source, TraceFormat, CMSIS_DAP_PORT, CONTROL_PORT,
    MAX_CONTROL_PAYLOAD, MAX_DAP_PACKET, ORBFLOW_PORT,
};
use std::fs::File;
use std::io::{self, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::path::Path;
use std::time::{Duration, Instant};

fn usage() -> ! {
    eprintln!("usage:\n  orbtrace info|stats|start|stop|reset HOST\n  orbtrace configure HOST SOURCE FORMAT SWO_BAUD\n  orbtrace capture HOST FILE [BYTES]\n  orbtrace replay FILE LISTEN_ADDR\n  orbtrace dap HOST HEX_PACKET\n  orbtrace remote-bitbang HOST LISTEN_ADDR\n  orbtrace gen-registers");
    std::process::exit(2)
}

fn connect(host: &str, port: u16) -> io::Result<TcpStream> {
    let address = if host.contains(':') && !host.starts_with('[') {
        host.to_owned()
    } else {
        format!("{host}:{port}")
    };
    let stream = TcpStream::connect(address)?;
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    stream.set_write_timeout(Some(Duration::from_secs(5)))?;
    Ok(stream)
}

fn transact(host: &str, command: &Command) -> io::Result<Vec<u8>> {
    let mut stream = connect(host, CONTROL_PORT)?;
    stream.write_all(&length_frame(&command.encode(), MAX_CONTROL_PAYLOAD).map_err(invalid)?)?;
    read_frame(&mut stream, MAX_CONTROL_PAYLOAD)
}

fn read_frame(stream: &mut TcpStream, maximum: usize) -> io::Result<Vec<u8>> {
    let mut size = [0; 4];
    stream.read_exact(&mut size)?;
    let size = u32::from_le_bytes(size) as usize;
    if size > maximum {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "frame too large",
        ));
    }
    let mut data = vec![0; size];
    stream.read_exact(&mut data)?;
    Ok(data)
}

fn invalid(error: impl ToString) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, error.to_string())
}

fn parse_source(value: &str) -> io::Result<Source> {
    match value {
        "vex" | "vexriscv" => Ok(Source::VexRiscv),
        "r5" => Ok(Source::R5),
        "a53" => Ok(Source::A53),
        "test" => Ok(Source::Test),
        _ => Err(invalid("source must be vex, r5, a53, or test")),
    }
}

fn parse_format(value: &str) -> io::Result<TraceFormat> {
    match value {
        "tpiu1" => Ok(TraceFormat::Tpiu1),
        "tpiu2" => Ok(TraceFormat::Tpiu2),
        "tpiu4" => Ok(TraceFormat::Tpiu4),
        "swo-nrz" => Ok(TraceFormat::SwoNrz),
        "swo-manchester" => Ok(TraceFormat::SwoManchester),
        _ => Err(invalid("unknown trace format")),
    }
}

fn parse_hex(value: &str) -> io::Result<Vec<u8>> {
    let clean: String = value
        .chars()
        .filter(|c| !c.is_ascii_whitespace() && *c != ':')
        .collect();
    if clean.len() % 2 != 0 {
        return Err(invalid("hex packet has odd length"));
    }
    (0..clean.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&clean[i..i + 2], 16).map_err(invalid))
        .collect()
}

fn capture(host: &str, path: &Path, limit: Option<u64>) -> io::Result<()> {
    let mut stream = connect(host, ORBFLOW_PORT)?;
    stream.set_read_timeout(None)?;
    let mut output = File::create(path)?;
    let mut total = 0u64;
    let started = Instant::now();
    let mut buffer = [0; 64 * 1024];
    while limit.map(|n| total < n).unwrap_or(true) {
        let wanted = limit
            .map(|n| (n - total).min(buffer.len() as u64) as usize)
            .unwrap_or(buffer.len());
        let count = stream.read(&mut buffer[..wanted])?;
        if count == 0 {
            break;
        }
        output.write_all(&buffer[..count])?;
        total += count as u64;
    }
    eprintln!(
        "captured {total} bytes in {:.3}s",
        started.elapsed().as_secs_f64()
    );
    Ok(())
}

fn replay(path: &Path, listen: &str) -> io::Result<()> {
    let payload = std::fs::read(path)?;
    let listener = TcpListener::bind(listen)?;
    eprintln!("replay listening on {listen} ({} bytes)", payload.len());
    let (mut stream, peer) = listener.accept()?;
    eprintln!("client {peer}");
    stream.write_all(&payload)?;
    stream.shutdown(Shutdown::Write)
}

fn dap_transaction(stream: &mut TcpStream, packet: &[u8]) -> io::Result<Vec<u8>> {
    stream.write_all(&length_frame(packet, MAX_DAP_PACKET).map_err(invalid)?)?;
    read_frame(stream, MAX_DAP_PACKET)
}

/// OpenOCD remote-bitbang to CMSIS-DAP JTAG-sequence bridge. Every clock edge
/// remains inside the CMSIS-DAP engine; the deliberately lockstep operation is
/// useful for debug correctness, while trace traffic uses its own port.
fn remote_bitbang(host: &str, listen: &str) -> io::Result<()> {
    let mut dap = connect(host, CMSIS_DAP_PORT)?;
    let listener = TcpListener::bind(listen)?;
    eprintln!("remote-bitbang listening on {listen}");
    let (mut client, peer) = listener.accept()?;
    eprintln!("OpenOCD client {peer}");
    let mut last_tdo = false;
    let mut byte = [0u8; 1];
    loop {
        match client.read(&mut byte)? {
            0 => return Ok(()),
            _ => match byte[0] {
                b'0'..=b'7' => {
                    let pins = byte[0] - b'0';
                    // Remote-bitbang bits are TCK:TMS:TDI. Only rising edges
                    // clock JTAG, avoiding a duplicate CMSIS-DAP sequence.
                    if pins & 4 != 0 {
                        let tms = pins & 2 != 0;
                        let tdi = pins & 1;
                        let response =
                            dap_transaction(&mut dap, &[0x14, 1, 1 | ((tms as u8) << 7), tdi])?;
                        if response.first() != Some(&0x14) || response.get(1) != Some(&0) {
                            return Err(io::Error::new(
                                io::ErrorKind::InvalidData,
                                "CMSIS-DAP JTAG sequence failed",
                            ));
                        }
                        last_tdo = response.get(2).copied().unwrap_or(0) & 1 != 0;
                    }
                }
                b'R' => client.write_all(if last_tdo { b"1" } else { b"0" })?,
                b'r'..=b'u' => {
                    // TRST/SRST combinations: DAP_SWJ_Pins
                    let value = match byte[0] {
                        b'r' => 0,
                        b's' => 0x80,
                        b't' => 0x20,
                        _ => 0xa0,
                    };
                    let _ = dap_transaction(&mut dap, &[0x10, value, 0xa0, 0, 0, 0, 0])?;
                }
                b'Q' => return Ok(()),
                b'B' | b'b' => {} // blink indication is optional in remote-bitbang
                other => {
                    return Err(invalid(format!(
                        "unknown remote-bitbang byte 0x{other:02x}"
                    )))
                }
            },
        }
    }
}

fn registers_sv() {
    println!("// Generated by //orbtrace/model:orbtrace gen-registers; do not edit.");
    println!("`ifndef ORBTRACE_REGS_SVH\n`define ORBTRACE_REGS_SVH");
    let entries = [
        ("ID", 0x0000u32),
        ("VERSION", 0x0004),
        ("CONTROL", 0x0008),
        ("SOURCE_FORMAT", 0x000c),
        ("SWO_BAUD", 0x0010),
        ("DMA_BASE_LO", 0x0018),
        ("DMA_BASE_HI", 0x001c),
        ("DMA_RING_SIZE", 0x0020),
        ("IRQ_STATUS", 0x0024),
        ("IRQ_ENABLE", 0x0028),
        ("RX_BYTES_LO", 0x0040),
        ("RX_BYTES_HI", 0x0044),
        ("DROP_BYTES_LO", 0x0048),
        ("DROP_BYTES_HI", 0x004c),
        ("SYNC_LOSS_LO", 0x0050),
        ("SYNC_LOSS_HI", 0x0054),
        ("FIFO_HIGH_WATER", 0x0058),
        ("DMA_FAULTS_LO", 0x0060),
        ("DMA_FAULTS_HI", 0x0064),
        ("DAP_COMMAND", 0x0080),
        ("DAP_RESPONSE", 0x0084),
        ("DAP_STATUS", 0x0088),
        ("DAP_CONTROL", 0x008c),
        ("DAP_TRANSFERS_LO", 0x0090),
        ("DAP_TRANSFERS_HI", 0x0094),
        ("DAP_ABORTS", 0x0098),
    ];
    for (name, value) in entries {
        println!("localparam logic [15:0] ORBTRACE_REG_{name} = 16'h{value:04x};");
    }
    println!("`endif");
}

fn run() -> io::Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        usage();
    }
    match args[0].as_str() {
        "gen-registers" if args.len() == 1 => {
            registers_sv();
            Ok(())
        }
        "capture" if (3..=4).contains(&args.len()) => capture(
            &args[1],
            Path::new(&args[2]),
            args.get(3)
                .map(|n| n.parse())
                .transpose()
                .map_err(invalid)?,
        ),
        "replay" if args.len() == 3 => replay(Path::new(&args[1]), &args[2]),
        "remote-bitbang" if args.len() == 3 => remote_bitbang(&args[1], &args[2]),
        "configure" if args.len() == 5 => {
            let response = transact(
                &args[1],
                &Command::Configure {
                    source: parse_source(&args[2])?,
                    format: parse_format(&args[3])?,
                    swo_baud: args[4].parse().map_err(invalid)?,
                },
            )?;
            println!("{}", String::from_utf8_lossy(&response));
            Ok(())
        }
        "dap" if args.len() == 3 => {
            let packet = parse_hex(&args[2])?;
            let mut stream = connect(&args[1], CMSIS_DAP_PORT)?;
            stream.write_all(&length_frame(&packet, MAX_DAP_PACKET).map_err(invalid)?)?;
            for byte in read_frame(&mut stream, MAX_DAP_PACKET)? {
                print!("{byte:02x}");
            }
            println!();
            Ok(())
        }
        command @ ("info" | "stats" | "start" | "stop" | "reset") if args.len() == 2 => {
            let command = match command {
                "info" => Command::GetInfo,
                "stats" => Command::GetStats,
                "start" => Command::Start,
                "stop" => Command::Stop,
                _ => Command::Reset,
            };
            let response = transact(&args[1], &command)?;
            if command == Command::GetStats {
                let s = stats_from_le(&response).map_err(invalid)?;
                println!(
                    "rx_bytes={} dropped_bytes={} sync_loss={} fifo_high_water={} dma_faults={}",
                    s.rx_bytes, s.dropped_bytes, s.sync_loss, s.fifo_high_water, s.dma_faults
                );
            } else {
                println!("{}", String::from_utf8_lossy(&response));
            }
            Ok(())
        }
        _ => usage(),
    }
}

fn main() {
    if let Err(error) = run() {
        eprintln!("orbtrace: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_colon_hex() {
        assert_eq!(parse_hex("05:00 01:02").unwrap(), [5, 0, 1, 2]);
    }
}
