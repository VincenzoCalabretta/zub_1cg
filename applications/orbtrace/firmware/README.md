# Embedded Rust targets

The firmware logic is split into allocation-free Rust libraries so it can be
tested on the host and linked into the existing board startup/runtime:

- `//applications/orbtrace/firmware/m3:trace_workload` is a host-testable
  deterministic ITM/TPIU stimulus reference model. The bare-metal
  `//applications/orbtrace/firmware/m3_app` entry point that actually runs on
  the PL-hosted Cortex-M3 is a hand-ported C implementation of the same event
  cycle (see its `main.c` and `sdk/bsp/m3/itm.h`) — there's no cross-language
  golden-vector test tying the two together, unlike the wire-format vectors
  in `firmware/common`.
- `//applications/orbtrace/firmware/a53:control_firmware` owns the control protocol,
  incremental TCP 3401/3240 framing, CoreSight selection, and AXI DMA S2MM
  register programming. Its only C ABI is the existing ThreadX/NetX Duo GEM
  transport and cache-maintenance boundary.
- `//applications/orbtrace/firmware/common:firmware_common` contains fixed-capacity TCP
  framing and the native 64-byte AXI DMA scatter/gather descriptor ring.

The Vivado design fixes the Orbtrace AXI-Lite block at `0xa0000000` and the
AXI DMA register block at `0xa0010000` — both predate the M3 work and are
load-bearing (`AXI_DMA_BASE` in `firmware/a53/src/lib.rs`, `TRACE_DMA_BASE`
in `firmware/a53_app/src/main.c`), so `create_bd.tcl`'s address assignment
keeps them fixed and places the new `m3_mem_ctrl` AXI window at `0xa0020000`
instead, even though `assign_bd_address`'s own auto-assignment would rather
put `m3_mem_ctrl` at `0xa0010000`. Descriptor and payload memory remain
in board-reserved DDR; the platform integration must flush descriptors before
updating `S2MM_TAILDESC` and invalidate completed payload buffers before the
Ethernet service reads them.

Final bare-metal ELF linkage for the control path requires the
board-specific A53 memory/startup integration. The M3 side already has one —
`//applications/orbtrace/firmware/m3_app` links against `sdk/bsp/m3` via the
`m3_firmware()` Bazel macro (`sdk/rules/firmware.bzl`), the same pattern
`r5_firmware()` already establishes for the RPU. The Rust libraries remain
`no_std`; their unit tests use `std` only under `cfg(test)`.
