# M3 BRAM load bug — handoff — 2026-08-16

## RESOLVED — 2026-08-16 (later same day)

Root cause found and fixed, confirmed on real hardware. `m3_mem_ctrl` and
`m3_mem_ctrl_core` (`axi_bram_ctrl`) were left at their default
`SINGLE_PORT_BRAM=0` (dual-port) configuration, but `create_bd.tcl` only
ever wires ONE of each controller's two BRAM ports. The generated wrapper
ties each controller's unconnected second port to a hardcoded constant —
confirmed in the generated Verilog that `m3_mem_ctrl`'s tied-off port
decodes to exactly `32'h00000008`, the fixed value every read below
reported. In this dual-port configuration `axi_bram_ctrl` evidently
services AXI writes via the connected port but AXI reads via the
disconnected one: writes were silently landing in the real BRAM the whole
time (hence always `BRESP=OKAY`), while every readback came from a
constant that could never change.

Fix: `CONFIG.SINGLE_PORT_BRAM {1}` on both controllers in `create_bd.tcl`
— confirmed this makes `axi_bram_ctrl` expose only a single BRAM port at
all, so the failure mode is structurally impossible now. Verified on real
hardware: `mrd -force 0xA0020000` after a write now returns the written
value (previously stuck at `00000008` always); both `tooling/xsct/load_m3.tcl`
(JTAG) and `orbtrace load-m3` (A53-native) now complete end-to-end with
correct readback and successfully release M3 reset. See the
`orbtrace-m3-integration` memory note and `M3_TRACE_VERIFICATION_PLAN.md`
(Phase D now DONE) for the full detail, including the real-but-ultimately-
irrelevant AXI write-channel anomaly (found via ILA, ruled out via a
second ILA capture and direct netlist inspection) that had to be
eliminated first before the actual bug was found.

The rest of this document is the original diagnostic handoff, preserved
for the investigation history and methodology (real hardware ILA capture
technique, LUT-budget/P&R-directive lessons for debug builds on this
part) — it no longer describes the current state of the bug.

## TL;DR

The hybrid Vivado build is fully passing and flashed to real hardware
(Phase C of `M3_TRACE_VERIFICATION_PLAN.md` is done and verified). Phase D
(loading a program into the M3's BRAM) is blocked by a real PL hardware bug:
**writes to the M3's BRAM window (`0xA0020000`, `m3_mem_ctrl`) do not take
effect, from either of the two independent paths that were tried.** Reads
from that address always return the same fixed word, `0x00000008`,
regardless of what was written, the access size used, or which bus master
issued the write. This is not a tooling problem — it reproduces identically
from JTAG-DAP and from the A53 CPU's own native stores, which have nothing
in common except the final AXI slave they both go through
(`m3_mem_ctrl`/`m3_mem` in `create_bd.tcl`).

A complete, tested "D2" fallback load path (stream the image over TCP via
the A53, instead of JTAG) was implemented specifically to route around a
suspected JTAG-only problem. It reproduced the exact same symptom, which is
the key new evidence: **this rules out JTAG/xsct as the cause.** The bug is
in the PL design itself (or its `create_bd.tcl` wiring), not in either load
path.

## Current confirmed-working state

- The hybrid Vivado build (`applications/orbtrace/vivado/build.tcl` +
  `orbtrace.xdc`) is fully clean: `write_bitstream Complete!`, WNS =
  +0.071ns / TNS = 0 / 0 failing endpoints, 0 unwaived critical CDC
  violations, 0 unwaived critical methodology violations. Outputs are in
  `bazel-out/orbtrace-vivado-hybrid/` (`zub_orbtrace.bit`, `.xsa`,
  `psu_init.tcl`, `artifacts.json` with checksums). See
  `documentation/M3_HYBRID_VIVADO_HANDOFF_2026-08-16.md` for the full build
  history (it needed real fixes along the way: `trace_clk_m3`/`swclktck`
  clock constraints, a CDC waiver for the M3 JTAG-DP boundary, and
  AggressiveExplore P&R directives — all now baked into `build.tcl`/
  `orbtrace.xdc`, not a bypass).
- That exact bitstream + matched `psu_init.tcl` pair is flashed and
  confirmed running on real hardware: `orbtrace info 192.168.1.50` returns
  `ZUBoard-Orbtrace/1`, and the UART shows the A53 control firmware alive
  (PHY link up, TCP stack ticking).
- `m3_control` (`ORBTRACE_REG_M3_CONTROL`, `0xA00000A0`) works correctly
  over **both** paths: JTAG-DAP bit-toggle (0→1→0, verified) and the new D2
  `M3Control` command. So does the base identity register at `0xA0000000`
  (reads back `0x4f52_4254`, "ORBT"). The debug-AP → PL-AXI path and the
  A53's own AXI-Lite register access are both fine in general.

## The bug, precisely

Target: `m3_mem_ctrl`'s S_AXI window, `0xA0020000`–`0xA002ffff` (64 KiB,
`create_bd.tcl:188-189`). This is the **PS/A53-preload view** of the M3's
BRAM (port A of a true-dual-port `blk_mem_gen`, `create_bd.tcl:84-98`); the
M3 core's own fetch view is a separate controller (`m3_mem_ctrl_core`, port
B) not reachable from the PS side.

**Path 1 — JTAG-DAP (`xsct`).** `tooling/xsct/load_m3.tcl`'s `mwr`/`dow`/
`mrd` calls first hit a software safety block ("PL AXI slave ports access
is not allowed... has not beed added to the memory map") — expected and
fixed by adding `-force` to all three, which is xsct's documented,
sanctioned override for exactly this ("no associated hardware-design memory
map" for a bare `connect`-only session). After that fix:

```
[1] Holding M3 in reset...              # mwr -force to m3_control: OK
[2] Downloading binary to M3 BRAM...    # dow -force -data: reports success
[3] Verifying reset-vector words...
ERROR: word 0 readback mismatch: expected 0x00010000, got 0x00000008
```

Follow-up interactive probing (see chat transcript for the exact commands)
showed, for every offset tried across the 64 KiB window and every access
size (`-size w`, `-size h`, `-size b`):

- Every read returns exactly `0x00000008` (byte pattern `08 00 00 00`
  repeating), never the value just written.
- A **direct, isolated** `mwr -force $addr 0x00010000` followed immediately
  by `mrd -force $addr` still returns `0x00000008` — the write is not
  merely stale/racy, it has no observable effect at all.
- The old (no-longer-used) scratch address `0xA0030000` — see "ruled out"
  below — gives a genuine bus fault (`AXI AP transaction error, DAP status
  0x30000021`), a **different** failure mode than `0xA0020000`'s silent
  no-op-with-fixed-readback. This says `0xA0020000` is correctly routed to
  *something* that responds without protocol error; it's just not the real
  BRAM content.

**Path 2 — A53 CPU (new D2 protocol, this session).** Full implementation
(details below) of loading the image via ordinary A53 memory-mapped stores
over TCP, specifically to test whether Path 1's failure was JTAG-DAP-
specific. Host-side and firmware-side unit tests all pass (mocked IO). On
real hardware:

```
[1] Holding M3 in reset...
[2] Streaming 716 bytes to M3 BRAM...
[3] Verifying reset-vector words...
orbtrace: readback mismatch: M3 BRAM does not contain the uploaded image
```

Independently re-checking via JTAG immediately afterward showed the BRAM
still reading `0x00000008` at every offset — **the exact same symptom**,
now via the A53's own native store instructions through its own AXI GP
master, a completely different physical path from JTAG-DAP's debug access
port. Two unrelated masters, identical failure: the common factor is the
slave side (`m3_mem_ctrl`/`m3_mem`) or its routing through `control_ic`.

## What's been ruled out, and why

- **Stale/duplicate address mapping.** `create_bd.tcl` did temporarily route
  `m3_mem_ctrl` through a scratch address (`0xA0030000`) during Phase A/B
  development before settling on `0xA0020000` (see the comment block at
  `create_bd.tcl:170-189`). Checked directly: `0xA0030000` now gives a real
  bus fault, not the `0x8` pattern, so nothing is still listening there —
  the final address (`0xA0020000`) is the one actually in effect.
- **A stale/independent `control_ic` (smartconnect) internal segment.**
  Structurally doesn't apply to Vivado's `assign_bd_address` model: an
  address space's segment offset (`ps/Data/SEG_m3_mem_ctrl_Mem0`,
  `create_bd.tcl:188`) is a single source of truth for the whole PS →
  `control_ic` → `m3_mem_ctrl` path; there is no separate midpoint
  configuration inside the smartconnect that could independently drift out
  of sync with it.
- **Reset held asserted.** `m3_mem_ctrl/s_axi_aresetn` is
  `pl_reset/peripheral_aresetn` (`create_bd.tcl:118-120`) — the same,
  ordinary `proc_sys_reset` output already used successfully by `trace_pl`
  and `m3_mem_ctrl_core`. Not gated by "trace running" or any software
  state; a normal power-on-style reset.
- **JTAG-DAP-specific transport quirk** (narrow/byte-granular AXI writes,
  DAP write-posting semantics, etc.). Directly disproven by Path 2: the
  D2 load path writes full 32-bit words via ordinary CPU store instructions
  (`core::ptr::write_volatile::<u32>`, see `ffi.rs`'s `write_m3_bram`) and
  hits the identical symptom.

## What's implemented and ready (D2 load path)

This is a complete, tested, working implementation of the "proper" load
path the verification plan flagged as future work — it just can't prove
itself end-to-end until the BRAM bug above is fixed. Nothing here needs to
be redone once that's fixed; re-run `orbtrace load-m3 HOST FILE` and it
should just work.

- `applications/orbtrace/model/src/lib.rs`: three new `Command` variants —
  `LoadM3Chunk { offset, data }` (opcode 7), `ReadM3Bram { offset, length }`
  (opcode 8), `M3Control { bits }` (opcode 9) — plus `M3_BRAM_SIZE`/
  `M3_BRAM_CHUNK` constants and round-trip tests
  (`m3_commands_round_trips`).
- `applications/orbtrace/firmware/a53/src/lib.rs`: `RegisterIo` gained two
  new methods, `write_m3_bram`/`read_m3_bram`, both with a no-op default so
  existing register-only test mocks (`Mailbox`, `ServiceIo`) don't need
  changes. `Controller::command()` dispatches the three new opcodes, with
  bounds-checking against `M3_BRAM_SIZE`. New tests: `m3_load_chunk_writes_
  and_reads_back`, `m3_load_chunk_rejects_out_of_range_offset`,
  `m3_control_writes_the_control_register`.
- `applications/orbtrace/firmware/a53/src/ffi.rs`: the real hardware `Mmio`
  impl of the two new methods — word-wise `write_volatile`/`read_volatile`
  loops (deliberately word-granular, not byte-wise, to bias toward the AXI
  access pattern most likely to be handled correctly by `axi_bram_ctrl`,
  given Path 1's evidence that narrow transfers were already suspect).
- `applications/orbtrace/model/src/main.rs`: new `orbtrace load-m3 HOST
  FILE` subcommand — holds M3 in reset, streams the image in
  `M3_BRAM_CHUNK`-sized (2 KiB) pieces, reads back and verifies the first
  ≤16 bytes before releasing reset (mirroring `load_m3.tcl`'s own
  vector-word check), matching `load_m3.tcl`'s reset-bit semantics (bit 0
  only; bit 1, the real-DAP-route bit, stays clear for Phase G).
- `tooling/xsct/load_m3.tcl`: fixed to add `-force` to every `mwr`/`dow`/
  `mrd` call (needed regardless of the BRAM bug — this was a real,
  separate bug in the script, since this xsct session has no associated
  hardware-design memory map). Kept as the direct-JTAG alternative; both
  paths currently hit the same underlying wall.

All of the above passes `bazel test //applications/orbtrace/model:all
//applications/orbtrace/firmware/a53:all --config=host` and `rustfmt
--check`. `//applications/orbtrace/firmware/a53_app:a53_app` and
`//applications/orbtrace/model:orbtrace` both build and were flashed/run on
real hardware (confirmed via `orbtrace info` and the new commands actually
executing end-to-end up to the readback-mismatch point).

## Suggested next steps

This now needs either hardware signal visibility or a closer RTL/BD review
than can be done blind:

1. **Add an ILA on `m3_mem_ctrl`'s `S_AXI` port** (or on `m3_mem`'s
   `BRAM_PORTA` signals directly) in a debug build, trigger a write from
   either load path, and see what's actually arriving — is `AWVALID`/
   `WVALID`/`WSTRB` correct, does `BRESP` come back OKAY, does `ENA`/`WEA`
   toggle on the BRAM primitive at all? This would definitively separate
   "AXI transaction never reaches the BRAM primitive" from "it reaches it
   but something about the primitive/generator config eats it."
2. **Re-check `m3_mem`'s `blk_mem_gen` configuration**
   (`create_bd.tcl:91-94`): `True_Dual_Port_RAM`, `Write_Width_A {32}`,
   `Write_Depth_A {16384}`, `Enable_B {Use_ENB_Pin}`. Port A (the one in
   question) uses default enable handling; port B has an explicit `ENB`
   pin override for the M3 core's own fetch path. Worth double-checking
   that configuring B's enable pin didn't have an unintended effect on how
   Vivado auto-wires port A's enable/write-enable in the generated wrapper.
3. **Compare against `m3_mem_ctrl_core`'s port** (the M3 core's own fetch
   view, port B, `0x0` from the M3's perspective — not reachable from the
   PS/JTAG side at all, so this can only be checked once real M3 firmware
   is actually running and can report what it fetched, e.g. by having it
   write a known pattern to a PL register on boot). If port B has the same
   symptom, the bug is squarely in `m3_mem`/`axi_bram_ctrl` configuration;
   if port B works, the bug is specific to `m3_mem_ctrl`'s (port A) own
   instance or its `control_ic` connection.
4. Consider whether `axi_bram_ctrl`'s default IP configuration (no
   `CONFIG.*` properties are set on `m3_mem_ctrl` in `create_bd.tcl` at
   all — it's created with all-default settings at line 84) is actually
   appropriate here, e.g. `C_S_AXI_CTRL_ADDR_WIDTH`/ECC-related defaults,
   even though no `C_ECC_TYPE` override is visible. Fully diffing its
   generated `.xci` against `m3_mem_ctrl_core`'s might turn up a
   Vivado-auto-derived difference between the two instances that isn't
   obvious from `create_bd.tcl`'s source alone.

## Repro commands

Board must already be flashed (Phase C); see
`documentation/M3_HYBRID_VIVADO_HANDOFF_2026-08-16.md` or `AGENTS.md`'s
"Flash over JTAG" section.

```sh
# D2 path (A53-native):
nix develop -c bazel build //applications/orbtrace/firmware/m3_app:m3_app \
  //applications/orbtrace/model:orbtrace
nix develop -c arm-none-eabi-objcopy -O binary \
  bazel-bin/applications/orbtrace/firmware/m3_app/m3_app m3_app.bin
bazel-bin/applications/orbtrace/model/orbtrace load-m3 192.168.1.50 m3_app.bin

# JTAG-DAP path (direct):
XILINX_ROOT=/home/v/opt/vitis nix develop -c xsct tooling/xsct/load_m3.tcl m3_app.bin

# Manual probe (any offset/size in the 64 KiB window reproduces this):
XILINX_ROOT=/home/v/opt/vitis nix develop -c xsct -eval {
    connect
    targets -set -nocase -filter {name =~ "APU*"}
    mwr -force 0xA0020000 0xdeadbeef
    puts [mrd -force 0xA0020000 1]
}
```
