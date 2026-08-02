# Rust-oriented Orbtrace for ZUBoard 1CG

This tree defines the new trace/debug contract without changing the existing
A53 or R5 applications:

- `model/`: independent Rust wire model and host CLI for control, statistics,
  capture, deterministic replay, CMSIS-DAP, and OpenOCD remote-bitbang.
- `rtl/`: SystemVerilog CDC, TPIU demux, SWO NRZ, packet boundaries, COBS,
  checksum, loss counters, and AXI-Lite registers.
- `firmware/`: allocation-free common, A53, and RV32IMAC application logic,
  including fragmented TCP framing and the AXI DMA scatter/gather ring.
- `vivado/`: batch-only Vivado 2023.2 project and artifact generation.

Network ports are TCP 3401 (versioned control), 3402 (Orbflow), and 3240
(unchanged CMSIS-DAP payload with a little-endian u32 length prefix).

The CMSIS-DAP payload crosses the AXI-Lite mailbox at `0x80`--`0x98`; command
and response bytes carry an explicit last flag, and status bits provide
backpressure. The PL engine implements deterministic Info, Connect, Transfer,
TransferAbort/WriteABORT, SWJ-Pins, and JTAG-Sequence behavior including
injectable WAIT, FAULT, and parity-error responses.

V1 is internal-only. It deliberately has no external trace connector, target
power, DFU, USB emulation, or serial-port compatibility layer.
