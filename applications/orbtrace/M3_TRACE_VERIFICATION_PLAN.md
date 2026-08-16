# Plan: end-to-end verification of real M3 firmware trace via Orbtrace

Goal: prove that a real program running on the PL-hosted Cortex-M3 produces
genuine CoreSight ITM/TPIU trace, that Orbtrace captures it correctly over
TCP 3402, and (separately) that the CMSIS-DAP/JTAG debug path over TCP 3240
reaches the same M3 for real. This is the concrete test of "the orbtrace
testing capabilities" the M3 integration was built for.

## Status (2026-08-16)

| Phase | What | Status |
|---|---|---|
| 1 | M3 as a real trace source (RTL/firmware/toolchain) | DONE |
| 2 | Real JTAG debug bridge (not SWD) | DONE |
| A | Acquire and confirm the M3 IP | DONE |
| B | Fix every `CONFIRM` marker in `create_bd.tcl` | DONE |
| C | Build, flash, and bring up the board | DONE — hybrid build passes all gates (WNS=+0.071ns, CDC/methodology clean), flashed to real hardware, `orbtrace info` confirms `ZUBoard-Orbtrace/1` |
| D | Load the M3 firmware image | DONE — root cause found and fixed 2026-08-16: `axi_bram_ctrl` defaulted to dual-port mode (`SINGLE_PORT_BRAM=0`) with only one BRAM port ever wired per controller, so each controller's unconnected second port was tied to a constant (confirmed: `m3_mem_ctrl`'s tied-off port decoded to exactly `0x00000008`, the fixed value every read returned) — writes landed via the connected port the whole time, reads always came from the disconnected one. Fix: `CONFIG.SINGLE_PORT_BRAM {1}` on both `m3_mem_ctrl`/`m3_mem_ctrl_core` in `create_bd.tcl`. Verified on real hardware via **both** load paths: `tooling/xsct/load_m3.tcl` (D1, JTAG) reads back correct vector words (`0x00010000`, `0x00000299`, ...) and `orbtrace load-m3` (D2, A53-native TCP) completes end-to-end, both releasing M3 reset. See the `orbtrace-m3-integration` memory note for full diagnostic history (a real-but-irrelevant AXI write-channel anomaly was ruled out first via ILA before the actual bug was found via post-route netlist inspection) |
| E | Configure Orbtrace and start capture | NOT STARTED — unblocked, next up |
| F | Verify the captured trace is genuinely correct | NOT STARTED — unblocked, follows E |
| G | Verify the real JTAG debug path | NOT STARTED — unblocked (the JTAG-DAP path to `m3_control` itself was already confirmed working) |

Phases 1 through D are all done and verified against real hardware, not
just tooling or reasoning — this includes a full synth/impl/route cycle on
real Vivado 2023.2 against the real Arm IP, a flashed bitstream, and actual
JTAG/TCP round-trips against the physical board. Phase E is next. See the
`orbtrace-m3-integration` memory note for the full detail on every phase,
including two real structural bugs found only once the stub was replaced
with the real IP and hardware existed to actually exercise the design: the
Phase A/B `axi_bram_ctrl`-can't-serve-two-address-views bug (fixed with a
true dual-port BRAM and two independent controllers) and the Phase D
BRAM-load bug described below.

## Phase A — Acquire and confirm the M3 IP — DONE

`~/Downloads/AT426-r0p1-00rel0-1.tar.gz` is the real package (Arm
DesignStart Cortex-M3 FPGA Xilinx Edition). Extract it somewhere stable
outside `~/Downloads` and set `ARM_DESIGNSTART_IP_ROOT` to its
`vivado/Arm_ipi_repository` subdirectory (contains the `CM3DbgAXI`
IP-XACT component — real VLNV `Arm.com:CortexM:CORTEXM3_AXI:1.1`, not the
earlier guessed `arm.com:DesignStart:...`). The RTL itself is IEEE
P1735/Xilinx-encrypted (only Vivado can elaborate it), but `component.xml`
plus a real `nix develop -c vivado -mode batch` probe (`create_bd_cell`,
`get_bd_pins`/`get_bd_intf_pins`) gave every real pin/interface name — see
the memory note for the full list.

## Phase B — Fix every `CONFIRM` marker in `create_bd.tcl` — DONE

Every marker is resolved and the fixes are applied to the file (no more
`grep -n CONFIRM` hits). Highlights (full detail in the memory note):

- Real VLNV, and `m3_core` exposes **two** separate AXI3 masters
  (`CM3_CODE_AXI3` instruction fetch, `CM3_SYS_AXI3` data/peripheral, both
  AXI3 not AXI4) rather than one `M_AXI` — only `CM3_CODE_AXI3` is wired;
  `CM3_SYS_AXI3` is intentionally left unconnected (nothing in this
  firmware's memory map reaches it).
- `xilinx.com:ip:smartconnect` cannot elaborate against this IP's AXI3
  interfaces (reproduced: hard failure reading
  NUM_READ_OUTSTANDING/NUM_WRITE_OUTSTANDING) — replaced with the legacy
  `axi_interconnect`, which auto-inserts an AXI3-to-AXI4 protocol
  converter, matching Arm's own Arty A7 reference design.
- Real clock/reset/JTAG pin names: no `HRESETn`/`PORESETn`/`TCK`/`TMS` —
  instead `SYSRESETn`+`DBGRESETn`, and JTAG/SWD share `SWCLKTCK`/`SWDITMS`
  pins on a combined SWJ-DP (autodetects JTAG vs SWD; no mode-select
  wiring needed).
- New required tie-offs the old script didn't have: `CFGITCMEN` (2 bits),
  `NMI`, `EDBGRQ`, `DBGRESTART`, `STCLK`, `WICENREQ`.
- A structural fix beyond simple renames: the shared `axi_bram_ctrl`
  originally serving both the A53 preload path and `m3_core`'s own fetch
  can't serve two non-contiguous address views (a real Vivado error, not
  a warning) — fixed with a true dual-port BRAM and two independent
  `axi_bram_ctrl` instances, one per address view.

Verified for real: `validate_bd_design` → `save_bd_design` →
`generate_target all` → `make_wrapper` against the real IP repo, all
clean except one expected, documented critical warning
(`m3_core/IRQ` intentionally unconnected — no external interrupt source
in this design).

## Phase C — Build, flash, and bring up the board

1. Full build:
   ```sh
   nix develop -c vivado -mode batch -source applications/orbtrace/vivado/build.tcl
   ```
   with `AVNET_BDF_ROOT` and `ARM_DESIGNSTART_IP_ROOT` set. This runs
   synth/impl for real and enforces the existing timing/CDC/methodology
   gates in `build.tcl` — the M3 adds real resource usage and new CDC
   crossings (the `trace_clk_m3` domain), so don't assume the existing
   margins hold; read `cdc.rpt`/`methodology.rpt`/`timing_summary.rpt`
   before treating a clean exit as sufficient.
2. Flash per [[board_bitstream_state]]'s known-good procedure — confirm
   `zub_orbtrace.bit` and its matching `generated/psu_init.tcl` are the pair
   actually used (memory note: mismatched pairs previously caused a DMA
   init failure that looked unrelated).
3. Boot the A53 control firmware (needs its own bring-up first per
   `ORBTRACE_TEST_REPORT_2026-08-08.md`'s suggested-next-steps #1, if that's
   still unresolved — check current state, don't assume) and confirm TCP
   3401/3402/3240 are reachable, e.g. `orbtrace info HOST`.

## Phase D — Load the M3 firmware image — DONE

### The issue

Both load paths below were fully implemented ahead of time, but on first
real-hardware execution (2026-08-16) neither actually worked: writes to
the M3's PS-preload BRAM window (`0xA0020000`, `m3_mem_ctrl`) silently had
no effect. Every read from that window — any offset, any access size,
either load path — returned the same fixed word, `0x00000008`, regardless
of what had just been written. The write itself always reported success
(`BRESP=OKAY`, `dow -force` reporting success), so this looked at first
like a completed-but-ineffective write, not a bus error.

Diagnosis required real hardware signal visibility, not just re-reading
`create_bd.tcl` (`validate_bd_design` was already clean and static review
of the address map / BRAM config / `axi_bram_ctrl`↔`blk_mem_gen`
negotiation found nothing wrong). A diagnostic Vivado build with a
`system_ila` core tapped onto `m3_mem_ctrl/S_AXI` (in a separate output
dir, so the known-good bitstream was never disturbed) captured a real AXI
write transaction and found a genuine anomaly — the write-data channel
sent two beats for a declared one-beat (`AWLEN=0`) burst — but fixing that
(matching the PS's `M_AXI_HPM0_FPD` width to its 32-bit downstream slaves)
built clean, flashed clean, and made no difference to the actual symptom.
A second ILA capture of the read channel showed the read side was
protocol-clean (a single, correctly-formed `RRESP=OKAY` beat) — meaning
the AXI fabric itself worked fine on both sides, and the bug had to be in
`m3_mem_ctrl`/`m3_mem` themselves.

The real cause was found for free by inspecting the already-built,
already-routed design (`open_project`/`open_run impl_1`, no new synthesis)
with `get_cells`/`get_pins`/`get_nets`: `m3_mem` (the actual BRAM primitive)
does not exist anywhere in the implemented netlist — and this was true
even of the very first hybrid build, predating every change made that day.
Reading the generated wrapper Verilog explained why: `m3_mem_ctrl` and
`m3_mem_ctrl_core` (`axi_bram_ctrl` instances) were left at their **default
`SINGLE_PORT_BRAM=0`** (dual-port) configuration, but `create_bd.tcl` only
ever wires ONE of each controller's two BRAM ports. The generated wrapper
ties each controller's unconnected second port to a hardcoded constant —
and `m3_mem_ctrl`'s tied-off port decodes to exactly `32'h00000008`, the
fixed value every read returned. In this dual-port configuration,
`axi_bram_ctrl` evidently services AXI writes via the connected port but
AXI reads via the disconnected one: writes were landing in the real BRAM
correctly the whole time, while every readback came from a constant that
could never change.

### The fix

`create_bd.tcl` now sets `CONFIG.SINGLE_PORT_BRAM {1}` on both
`m3_mem_ctrl` and `m3_mem_ctrl_core`, right after creating them — confirmed
this makes `axi_bram_ctrl` expose only a single BRAM port at all, so the
failure mode is structurally impossible now, not just less likely.

### Current status: verified on real hardware

A full production rebuild with the fix passed every gate (synth/impl,
unwaived CDC, unwaived methodology, timing) using the unmodified
`build.tcl`, was flashed, and retested:

- `mwr -force 0xA0020000 <value>` followed by `mrd -force 0xA0020000` now
  reads back the value just written. Untouched offsets read `00000000` —
  real, distinct, per-address memory content, not a fixed constant.
- **D1** (JTAG): `tooling/xsct/load_m3.tcl` completes end-to-end and reads
  back the correct reset-vector words (`0x00010000` stack pointer,
  `0x00000299` reset handler entry, Thumb bit set), then releases M3
  reset.
- **D2** (A53-native): `orbtrace load-m3 HOST FILE` streams the image over
  TCP, verifies readback, and releases M3 reset — "M3 is running."

Run either path with:
```sh
nix develop -c bazel build //applications/orbtrace/firmware/m3_app:m3_app
arm-none-eabi-objcopy -O binary \
  bazel-bin/applications/orbtrace/firmware/m3_app/m3_app m3_app.bin
# D1 (JTAG):
XILINX_ROOT=/home/v/opt/vitis \
  nix develop -c xsct tooling/xsct/load_m3.tcl m3_app.bin
# D2 (A53-native, no JTAG needed):
bazel-bin/applications/orbtrace/model/orbtrace load-m3 HOST m3_app.bin
```
Build `//applications/orbtrace/firmware/m3_app:m3_app` first: it is the
target that applies the Cortex-M3 platform transition. The resulting
`m3_app` is a symlink to the transitioned ELF and is valid input to
`arm-none-eabi-objcopy`; building `:m3_app_elf` directly instead selects the
host toolchain and fails on Cortex-M compiler flags.

Full diagnostic history (ILA build tooling, LUT-budget and P&R-directive
lessons for debug builds on this part, the ruled-out AXI-width hypothesis)
is in the `orbtrace-m3-integration` memory note and
`documentation/M3_BRAM_LOAD_BUG_HANDOFF_2026-08-16.md`.

### Next steps

Phase D itself needs nothing further. What's next is Phase E (below):
configure Orbtrace for `source=m3` and start a real capture now that a
real program is running on the M3.

## Phase E — Configure Orbtrace and start capture — IN PROGRESS, real bug found

**Status (2026-08-16, this session):** M3 reloaded, `orbtrace configure ... m3
tpiu4 2000000` + `start` run for real against real hardware
(`192.168.1.50`). Result: **not clean** — `orbtrace stats` shows `rx_bytes=0`,
`dropped_bytes`/`sync_loss` in the hundreds of millions to billions (not
zero/near-zero as the plan expects), `fifo_high_water` pegged at its max
(63/63) — the M3's own async CDC FIFO in `orbtrace_pl.v` is saturated and
dropping almost everything.

**Two findings, in order:**

1. **Real bug, fixed:** `sdk/bsp/m3/itm.h`'s `m3_itm_init()` wrote
   `TPIU_CSPSR = 1u << port_width_select`, but `TPIU_CSPSR`'s `PORT_SIZE`
   field is one-hot at bit `(width-1)` — legal values are `0x1` (1-bit),
   `0x2` (2-bit), `0x8` (4-bit); bit 2 (`0x4`, what the old code wrote for
   the 4-bit case, `port_width_select=2`) is not a legal port size. Fixed to
   map `port_width_select==2` to bit 3 (`0x8`) instead. This alone did not
   fix the symptom (still `rx_bytes=0` after rebuild+reload+retest at
   4-bit), but is a real, confirmed-by-architecture bug worth keeping fixed
   regardless.
2. **Root cause still open:** with a temporary diagnostic firmware build
   using `m3_itm_init(0)` (1-bit port, unambiguous CSPSR encoding even in
   the old code) + `orbtrace configure ... m3 tpiu1 ...`, `rx_bytes` went
   **nonzero (232)** — real, correctly-framed CoreSight bytes did arrive and
   sync. This proves the M3 trace source, the CSPSR fix, TPIU sync framing,
   and Orbtrace's decode chain are all fundamentally working. But
   `dropped_bytes`/`sync_loss` were *higher*, not lower, at 1-bit than at
   4-bit (billions either way), and `fifo_high_water` stayed pegged at max
   regardless of width — meaning the M3's TPIU is driving a continuous
   trace stream that overwhelms the `m3_trace_fifo` CDC FIFO's drain rate
   into `aclk`, independent of port width. This looks like a genuine
   bandwidth/clocking mismatch between `trace_clk_m3` (the M3 IP's own
   `TRACECLK` output, nominally HCLK-derived per `create_bd.tcl`, i.e.
   ~100MHz) and `aclk` (also `pl_clk0`, ~100MHz) rather than anything
   width-encoding-related — plausible causes not yet distinguished: (a) the
   real `TRACECLK` rate is higher than the assumed 100MHz (the DesignStart
   IP may internally multiply it for the trace port), (b) `m3_trace_fifo`'s
   read side isn't draining at the assumed 1 byte/cycle for some other
   reason, or (c) the M3's own idle/continuous-formatter behavior at reset
   defaults is producing far more trace traffic than the deterministic
   `emit_next()` stimulus alone would justify. Resolving this conclusively
   likely needs the same real-ILA-on-real-hardware methodology as the
   Phase D BRAM bug (a `system_ila` tapped on `trace_clk_m3`/the
   `m3_trace_fifo` write side, to see the actual toggle rate and content) —
   not yet attempted this session, since that's another ~30-90 min Vivado
   build cycle and this felt like a natural checkpoint to report before
   committing to it.

The diagnostic 1-bit firmware change in
`applications/orbtrace/firmware/m3_app/src/main.c` (`m3_itm_init(0)`
instead of `m3_itm_init(2)`) is TEMPORARY and uncommitted — revert to
`m3_itm_init(2)` once back to testing the real 4-bit configuration the rest
of this plan assumes.

## Phase E — Configure Orbtrace and start capture (original plan text)

Using the `orbtrace` host CLI (`applications/orbtrace/model`):
```sh
orbtrace configure HOST m3 tpiu4 2000000   # source=M3, 4-bit TPIU DDR, baud unused for tpiu4
orbtrace start HOST
orbtrace capture HOST captured.bin 8388608 &   # background, generous byte budget
```
`tpiu4` matches both `create_bd.tcl`'s `PSU__TRACE__WIDTH {4Bit}` precedent
and `m3_app`'s `main.c` (`m3_itm_init(2)` selects the 4-bit sync port) —
keep these three in sync if either changes. After a run:
```sh
orbtrace stats HOST
```
and confirm `dropped_bytes`/`sync_loss`/`dma_faults` are all zero (or at
least not advancing beyond a small, explained baseline) — these map
directly to `orbtrace_pl.v`'s new `m3_cdc_drops_sync`/`m3_overrun_s2`
signals, so a nonzero count here specifically implicates the new M3 capture
chain, not the pre-existing PS path.

## Phase F — Verify the captured trace is genuinely correct

This is the step with no existing tooling — write it as part of this
verification, not assume it exists:

1. `captured.bin` is COBS/Orbflow-framed, channel-multiplexed bytes (per
   `orbtrace_orbflow_encoder.sv`/the model's own decode logic in
   `model/src/lib.rs`). Decode it the same way the model's own tests do —
   reuse those decode functions rather than re-deriving the framing.
2. `main.c`'s `emit_next()` is a hand port of `firmware/m3/src/lib.rs`'s
   `Workload` — same xorshift state, same event-index-mod-16 dispatch, no
   automated test ties them together (documented in `firmware/README.md`).
   Write a small comparison script/test now that real hardware exists to
   compare against: run the Rust `Workload` with seed 7 for N iterations,
   derive the expected channel/value/width sequence, and diff it against
   the decoded ITM stimulus-port packets from `captured.bin`. This is the
   actual proof that what's on the wire is real CoreSight traffic
   correctly generated by the M3 and correctly captured by Orbtrace — not
   just "some bytes arrived."
3. Cross-check independently with Orbuculum (`orbuculum` decoding TCP 3402
   directly, per `TESTING.md` acceptance item 3) if available, as a second,
   externally-implemented decoder — agreement between two independent
   decoders is much stronger evidence than one.

## Phase G — Verify the real JTAG debug path

Separate from trace capture, exercises `orbtrace_dap_engine.sv`'s
`use_real_target` path (Phase 2):

1. Set `ORBTRACE_REG_M3_CONTROL` bits 1 and 0 (`0x3`) — bit 1 routes the
   real M3 DAP and bit 0 must remain set so this write does not re-assert M3
   reset. Do this via a direct AXI-Lite write for now (no Command-protocol
   opcode exists for this either; the same D2-style follow-up applies).
2. `orbtrace remote-bitbang HOST LISTEN_ADDR`, then point OpenOCD's
   `remote_bitbang` transport at `LISTEN_ADDR`, targeting a JTAG (not SWD)
   config matching the M3's JTAG-DP.
3. Halt the target, read the PC, and confirm it's a plausible address
   inside `m3_app`'s `.text` (from Phase D's ELF) — or single-step and watch
   it move through `emit_next()`. This proves TCP 3240 reaches real
   silicon, not the synthetic responder.
4. Release and confirm the M3 resumes emitting trace (ties back into
   Phase E/F running concurrently) — this is the actual end-to-end claim:
   the same physical M3 is simultaneously a real trace source and a real
   debug target through the same Orbtrace instance.

## Risks / open items to flag before starting

- **Confirmed 2026-08-15:** a full `build.tcl` run with the extracted real
  Arm release (`ARM_DESIGNSTART_IP_ROOT=/home/v/projects/arm_designstart_m3/vivado/Arm_ipi_repository`)
  and a real Avnet BDF checkout (`AVNET_BDF_ROOT=/home/v/projects/avnet_bdf`)
  successfully selects `avnet-tria:zuboard_1cg:part0:1.2`, catalogs
  `Arm.com:CortexM:CORTEXM3_AXI:1.1`, and launches all generated IP runs.
  The M3 out-of-context synthesis then fails at encrypted RTL file
  `CM3DbgAXI/rtl/cm3_dap_ahb_ap.v` with Vivado `[Synth 8-5809] Error
  generated from encrypted envelope`. Vivado successfully obtains its normal
  `Synthesis` device license, so this is not an FPGA-license failure. The
  Arm r0p1 release note says the package was developed with **Vivado 2019.1**.
  Vivado **2019.1.3** is now installed alongside 2023.2 at
  `/home/v/opt/vitis`; it successfully synthesized the configured core as
  `bazel-out/m3-ooc-2019/m3_core.edf`.  A full 2019 project is not possible
  because 2019.1 does not contain `xczu1cg-sbva484-1-e`.  `build.tcl` now
  supports `M3_OOC_EDIF`: it disables only the 2023-generated encrypted M3
  IP sources and adapts the generated cell wrapper to the 2019 EDIF, leaving
  the ZU1 board implementation in 2023.2.  A 2023.2 `read_edif` plus
  `link_design -part xczu1cg-sbva484-1-e -top m3_core` check passed before
  the full hybrid build was launched.  Do not use the pre-existing
  2026-08-09 cache bitstream for this M3 plan: it predates the M3 integration.
- **Resolved 2026-08-16, keep as historical context:** Phase A/B/C/D real
  synth/impl/route on the actual M3 netlist is done, repeatedly, both with
  and without debug ILA cores added — resource usage, timing, and CDC are
  known-good (production build passes all gates: WNS positive, unwaived
  CDC/methodology clean). `AVNET_BDF_ROOT=/home/v/projects/avnet_bdf` is a
  confirmed-working real Avnet BDF checkout, used successfully for every
  build this session — no longer a risk. The A53 control firmware is
  confirmed up and serving TCP 3401/3402/3240 (`orbtrace info HOST`
  returns `ZUBoard-Orbtrace/1` reliably) — no longer a risk.
- **New, from the Phase D investigation:** this board's host-side Vivado
  builds can be slow or get killed early under heavy concurrent
  desktop/other-session load on this machine (observed: a debug ILA build
  killed automatically, progressively earlier on repeated retries, no
  clear OOM log — see the `orbtrace-m3-integration` memory note). If a
  future build (e.g. Phase G debug/JTAG-timing work) dies unexpectedly
  early with no error in its own log, check `ps aux`/`free -h` for
  competing load before assuming the build itself is broken.
- JTAG timing (`JTAG_HALF_PERIOD` in `orbtrace_dap_engine.sv`, currently a
  conservative unverified guess) may need tuning once Phase G is actually
  attempted — still untested against real hardware. If Phase G's
  halt/read-PC doesn't respond, that's the first thing to check, not
  assume the wiring is wrong.
