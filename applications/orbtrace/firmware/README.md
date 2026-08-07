# Embedded Rust targets

The firmware logic is split into allocation-free Rust libraries so it can be
tested on the host and linked into the existing board startup/runtime:

- `//applications/orbtrace/firmware/vexriscv:trace_workload` is RV32IMAC-safe deterministic
  ITM/TPIU stimulus logic.
- `//applications/orbtrace/firmware/a53:control_firmware` owns the control protocol,
  incremental TCP 3401/3240 framing, CoreSight selection, and AXI DMA S2MM
  register programming. Its only C ABI is the existing ThreadX/NetX Duo GEM
  transport and cache-maintenance boundary.
- `//applications/orbtrace/firmware/common:firmware_common` contains fixed-capacity TCP
  framing and the native 64-byte AXI DMA scatter/gather descriptor ring.

The Vivado design fixes the Orbtrace AXI-Lite block at `0xa0000000` and the
AXI DMA register block at `0xa0010000`. Descriptor and payload memory remain
in board-reserved DDR; the platform integration must flush descriptors before
updating `S2MM_TAILDESC` and invalidate completed payload buffers before the
Ethernet service reads them.

Final bare-metal ELF linkage requires the checked-in generated VexRiscv core
and the board-specific A53 memory/startup integration. The libraries remain
`no_std`; their unit tests use `std` only under `cfg(test)`.
