# Plan: end-to-end verification of real M3 firmware trace via Orbtrace

Goal: prove that a real program running on the PL-hosted Cortex-M3 produces
genuine CoreSight ITM/TPIU trace, that Orbtrace captures it correctly over
TCP 3402, and (separately) that the CMSIS-DAP/JTAG debug path over TCP 3240
reaches the same M3 for real. This is the concrete test of "the orbtrace
testing capabilities" the M3 integration was built for.

## Status (2026-08-17, updated)

| Phase | What | Status |
|---|---|---|
| 1 | M3 as a real trace source (RTL/firmware/toolchain) | DONE |
| 2 | Real JTAG debug bridge (not SWD) | DONE |
| A | Acquire and confirm the M3 IP | DONE |
| B | Fix every `CONFIRM` marker in `create_bd.tcl` | DONE |
| C | Build, flash, and bring up the board | DONE — hybrid build passes all gates (WNS=+0.071ns, CDC/methodology clean), flashed to real hardware, `orbtrace info` confirms `ZUBoard-Orbtrace/1` |
| D | Load the M3 firmware image | DONE (D1/JTAG path). Two distinct hardware bugs found and fixed across 2026-08-16/17: (1) `axi_bram_ctrl` defaulted to dual-port mode with an unconnected, tied-off second port (fixed: `SINGLE_PORT_BRAM {1}`); (2) a later regression, a 4:1 word-drop caused by a write-data pacing fault inside `control_ic` (smartconnect), fixed 2026-08-17 by routing `m3_mem_ctrl` through a dedicated `axi_interconnect` (`m3_mem_ctrl_ic`) instead of `control_ic`'s M02 leg — verified 16/16 words correct via direct JTAG `mwr`/`mrd` and via `load_m3.tcl`. D2 (`orbtrace load-m3`, native A53 path) is separately blocked, but root-caused as NOT a hardware bug at all — see below |
| E | Configure Orbtrace and start capture | BLOCKED on D2 (or a D1-only workaround) before the real ITM/TPIU STIM-FIFO-stall investigation from 2026-08-16 can be trusted — that investigation ran against a firmware image that, in hindsight, may never have loaded correctly |
| F | Verify the captured trace is genuinely correct | NOT STARTED — blocked on E |
| G | Verify the real JTAG debug path | NOT STARTED — blocked on E; the JTAG-DAP bit-bang path to `m3_control` was previously confirmed working and is unrelated to D2's NetX-level block (different code path entirely) |

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

**Update 2026-08-16 (continued): root cause found via real ILA capture —
the M3 CPU is permanently stuck on its first blocking ITM stimulus write.**
This supersedes the bandwidth/clocking-mismatch hypothesis above, which is
now ruled out.

Built a diagnostic Vivado bitstream (not committed; scratch-only, see
below) adding a `system_ila` tapped directly on `m3_core`'s own
`TRACECLK`/`TRACEDATA` output pins (IP boundary, matching the Phase D BRAM
bug's proven methodology) and captured real hardware waveforms via Vivado
Hardware Manager:

1. **With the real firmware running** (`emit_next()`'s blocking
   `m3_itm_write()`/`m3_itm_write_width()` calls), `TRACEDATA` showed a
   perfectly rigid `0111`/`1111` alternation on every single `trace_clk_m3`
   cycle for the entire 8192-sample (~82µs) capture window, with zero
   deviation. That is not varying CoreSight trace content — it is a fixed
   idle/toggle pattern.
2. **With a diagnostic firmware build that bypasses the FIFO-ready poll
   entirely** (`M3_ITM_STIM(0) = counter++` in a tight loop, no `while
   (!(STIM & 1))` wait), `TRACEDATA` immediately showed real variation —
   all 16 possible nibble values appeared across the capture. This proves
   the ILA tap, its `trace_clk_m3` clock choice, and the physical
   TRACEDATA pins are all working correctly (ruling out the diagnostic
   build's own very tight timing margin — 0.010–0.015 ns slack, since it
   used default, not `AggressiveExplore`, P&R directives — as an
   explanation for the earlier static pattern).
3. Together, (1) and (2) mean: **the real firmware's very first blocking
   `m3_itm_write()` call inside `emit_next()` never returns.** The ITM
   stimulus-port FIFO's "ready" bit (`M3_ITM_STIM(n) & 1`) never asserts
   under the real firmware's configuration, so the M3 CPU spins forever on
   the first stimulus write and `emit_next()` never advances past
   `sequence == 0`. The Phase E capture-FIFO overflow symptom (`rx_bytes=0`,
   `dropped_bytes`/`sync_loss` in the billions, `fifo_high_water` pegged at
   max) is a *downstream symptom*, not the root problem: Orbtrace's
   `orbtrace_ddr_capture` naively DDR-samples and byte-reconstructs
   whatever is electrically present on `trace_data_m3` whenever `running`
   is set, with no concept of "idle" vs "real data" — so the M3's
   permanently-stuck idle-pattern output floods the capture FIFO forever,
   and (since it's not real CoreSight-framed data) never contains the
   `0xFFFFFF7F` sync pattern the demux is looking for.
4. Tried switching to SWO NRZ mode (`M3_TPIU_SPPR_SWO_NRZ`) with the same
   blocking-write pattern, to test whether Parallel mode specifically is
   the problem. Inconclusive: `TRACEDATA` read a flat, unchanging `0` the
   entire capture in SWO mode — most likely because SWO output isn't
   actually routed through `TRACEDATA[3:0]` on this IP at all (a different,
   unconnected physical pin), not because the write blocked or unblocked.
   `orbtrace_pl.v`/`create_bd.tcl` both currently assume SWO-for-M3 arrives
   via `trace_data_m3[0]` (mirroring the PS path's wiring) — this
   assumption is now suspect and unverified for the M3 IP specifically.

**Not yet determined:** *why* the STIM FIFO ready bit never asserts under
Parallel mode. Candidates not yet individually tested: `ITM_TPR` (Trace
Privilege Register, never written — defaults may restrict unprivileged
STIM access on this IP, though ARMv7-M reset should leave the CPU
privileged), `TPIU_FFCR` (Formatter and Flush Control, also never written
by `m3_itm_init()`), or — the more structurally significant possibility —
this DesignStart FPGA edition's TPIU may not actually implement/drain a
functional 4-bit synchronous parallel trace port at all (only SWO), in
which case Phase 1's "M3 as a 4-bit-parallel CoreSight trace source"
premise would need to pivot to SWO instead, reusing the SWO NRZ/Manchester
decode path `orbtrace_pl.v` already has for the PS source — but this
requires first confirming where the M3's real SWO output pin actually is
(if this IP exposes one at all outside `TRACEDATA`/`TRACECLK`) and fixing
`create_bd.tcl`'s wiring to match, before the SWO hypothesis can be tested
cleanly.

**Diagnostic tooling used (not committed, scratch-only):**
`create_bd_debug.tcl` (copy of `create_bd.tcl` plus a `system_ila` NATIVE-
mode tap on `m3_core/TRACECLK`+`TRACEDATA`), `build_debug_m3_ila.tcl` (copy
of `build.tcl` sourcing the above, default P&R directives, CDC/methodology/
timing gates downgraded to warnings-only since it's a throwaway diagnostic
bitstream), and `program_only.tcl`/`arm_and_capture.tcl` (Vivado Hardware
Manager Tcl: JTAG-program via `tooling/xsct/jtag_flash.sh` — NOT a raw
`program_hw_devices` call, which was tried first and left the board's A53
control firmware unresponsive/ARP-dead until a proper full
`rst -system`-based reflash recovered it — then `PROBES.FILE` +
`TRIGGER_COMPARE_VALUE` set to `eq4'hX` (all-don't-care, i.e. trigger
immediately) to capture real M3 trace activity). All of this lived in the
session scratchpad; recreate it from this description if repeating the
methodology (same caveat as the Phase D ILA tooling note).

The board has been restored to the last known-good production bitstream
(`bazel-out/orbtrace-vivado-hybrid-fix2` cache artifacts) before ending
this session. `sdk/bsp/m3/itm.h`'s CSPSR fix (real, committed) and
`applications/orbtrace/firmware/m3_app/src/main.c` are both back to their
committed state — no uncommitted diagnostic changes remain in the repo.

### Next steps to a fully functional system

1. **Root-cause why the ITM STIM FIFO never signals ready — the actual
   blocker.** Nothing below can proceed until real trace data flows.
   Cheapest to most expensive:
   - Try writing `ITM_TPR = 0` explicitly (never touched by
     `m3_itm_init()` — if its reset default restricts stimulus-port
     access, this alone could unblock it). Firmware-only, no rebuild.
   - Try enabling `TPIU_FFCR` (also never written). Firmware-only.
   - Check the AT426 DesignStart package docs
     (`~/Downloads/AT426-r0p1-00rel0-1.tar.gz` extract) for whether this
     FPGA edition's TPIU actually implements a functional
     parallel/synchronous trace port, or only SWO — this determines
     whether the two firmware-only tries above can work in principle at
     all.
   - If parallel mode genuinely isn't implemented: pivot to SWO. Means
     finding where this IP's real SWO output pin actually lives
     (`component.xml` should show it, if it exists outside
     `TRACEDATA`/`TRACECLK`), fixing `create_bd.tcl`'s wiring, and
     correcting `orbtrace_pl.v`'s assumption that M3 SWO rides on
     `trace_data_m3[0]`. This is the one path here that needs a real
     Vivado rebuild.
2. **Re-run Phase E** once real trace data flows: `orbtrace
   configure`/`start`/`stats` should show `rx_bytes > 0` and near-zero
   `dropped_bytes`/`sync_loss` — the acceptance check that has been
   failing throughout this investigation.
3. **Phase F** — decode `captured.bin` with the model's existing Orbflow
   decoder and diff it against the Rust `Workload` reference (seed 7) to
   prove the bytes on the wire match `emit_next()`'s real sequence, not
   just "some bytes arrived." Cross-check with Orbuculum if available, as
   an independent decoder.
4. **Phase G** — set `ORBTRACE_REG_M3_CONTROL` bits `0x3`, bridge OpenOCD
   through `orbtrace remote-bitbang`, halt and read PC (confirm it's
   inside `m3_app`'s `.text`), single-step, resume, and confirm trace
   resumes — proving the same M3 is simultaneously a real trace source
   and a real debug target.

Loose ends along the way: commit this plan doc's updates once reviewed,
and consider deleting the stray untracked `.srcs/` directory (an old ILA
leftover, not from this investigation) once confirmed unneeded.

**Update 2026-08-17: two of the four candidate causes ruled out, and the
real SWO pin found, entirely from the AT426 package's own documentation
(`~/projects/arm_designstart_m3/docs/arm_cortexm3_processor_trm_100165_0201_00_en.pdf`
and `.../CM3DbgAXI/component.xml`) — no hardware needed for this pass, since
the board was unreachable this session (`ping 192.168.1.50` failed,
`nmcli` shows the `zub_1cg-board` profile present but no device attached —
the USB-Ethernet adapter isn't currently plugged into this host).**

1. **`ITM_TPR` ruled out.** Per the CM3 TRM §9.3: "You can only write to
   this register in privileged mode" and its PRIVMASK bits only gate
   *unprivileged (user-mode)* access to the Stimulus/Trace-Enable
   registers — "ITM registers are fully accessible in privileged mode."
   `m3_app`'s firmware never drops out of privileged Thread mode (nothing
   touches `CONTROL.nPRIV`), so `ITM_TPR` cannot be the blocker regardless
   of its value. Do not spend a hardware cycle on this.
2. **`TPIU_FFCR` ruled out for Parallel mode.** Per the CM3 TRM §11.3.3:
   "If TPIU_SPPR is set to select Parallel Port Mode, the formatter is
   automatically enabled" — `EnFCont` only controls whether the formatter
   can be *bypassed*, and bypass is only selectable in one of the two SWO
   modes ("When one of the two SWO modes is selected, bit [1] of
   TPIU_FFCR enables the formatter to be bypassed"). In Parallel mode
   (`TPIU_SPPR=0`, what `m3_itm_init()` selects) `TPIU_FFCR` has no
   documented effect at all. Do not spend a hardware cycle on this either.
3. **The formatter idle pattern the ILA saw is architecturally expected,
   not itself evidence of a stall.** CM3 TRM §11.2.2: "When the formatter
   is enabled, half-sync packets can be inserted if there is no data to
   output after a frame" — the rigid `0111`/`1111` alternation previously
   read as "stuck" is consistent with correctly-functioning idle-frame
   insertion while the formatter waits for the ITM to hand it real data.
   This doesn't change the diagnosis (the STIM FIFO ready bit still
   genuinely never asserts, confirmed by the CPU-side blocking behavior,
   which is a different signal than what's on the wire) but narrows where
   the bug must be: something inside the ITM's own FIFO-to-formatter
   handoff, not the TPIU/formatter/pins, which all look correctly alive.
4. **Real, structural bug found for the SWO contingency path:**
   `CM3DbgAXI/component.xml` shows the IP exposes a **dedicated `SWV`
   output pin** (always present, no `TRACE_LVL`-gated enablement unlike
   `TRACECLK`/`TRACEDATA`/`TRCENA`) — this is the real Serial Wire Output
   pin, entirely separate from `TRACEDATA[3:0]`. `create_bd.tcl` never
   wires `m3_core/SWV` anywhere, and `orbtrace_pl.v`'s
   `source_select==0 ? trace_data_m3[0] : trace_data[0]` mux (lines ~160,
   162, feeding `orbtrace_swo_nrz`/`orbtrace_swo_manchester`) reads
   `trace_data_m3[0]` for the M3's SWO input — confirmed wrong. This
   explains Phase E finding 4 above (flat, unchanging `0` when SWO mode
   was tried against the real firmware): the mux was reading a TRACEDATA
   line that never carries SWO data in the first place, not evidence SWO
   itself doesn't work on this IP. **If parallel mode is ultimately
   abandoned, the SWO pivot needs: a new `m3_swo` port threaded from
   `m3_core/SWV` through `create_bd.tcl` into `orbtrace_pl`, and the two
   mux expressions above changed to read it instead of
   `trace_data_m3[0]`.** This is real, useful prep work but doesn't by
   itself explain or fix the Parallel-mode STIM-FIFO stall — Parallel mode
   remains the primary path until something concrete rules it out on real
   hardware (docs say plain ITM-only Parallel trace, no ETM, is a fully
   supported, ordinary configuration — nothing found suggests this IP
   edition lacks it).
5. **Not resolved by documentation, still needs real hardware:** why the
   ITM's internal FIFO never hands data to the formatter. `TPIU_SSPSR`
   (Supported Parallel Port Size Register, `0xE0040000`, read-only) has
   never actually been read back — it reports which port widths the IP
   genuinely supports in silicon, independent of what `TPIU_CSPSR` is told
   to select. Reading it (e.g. stash the value somewhere a debugger/JTAG
   read can see, or emit it as the very first ITM word before the
   blocking-write loop starts) is a cheap firmware-only addition worth
   doing on the next hardware session, before another ILA rebuild cycle —
   confirms or rules out "this particular synthesized instance doesn't
   actually support 4-bit" for free.

**Next hardware session, in order:** (1) reconnect the board — plug in the
USB-Ethernet adapter, confirm `nmcli device status` shows the interface,
`nmcli connection up zub_1cg-board`, then `orbtrace info 192.168.1.50`; (2)
add the `TPIU_SSPSR` readback from point 5 and reflash/retest before
reaching for another ILA bitstream; (3) only if that's inconclusive, go
back to ILA — this time tapping inside the ITM/TPIU boundary if a tap
point exists, or at minimum confirm `TPIU_FFSR`/`TPIU_FFCR` readback
(not just the write side) once real hardware is available again, since
this document research could only reason about writes, not read live
register state.

**Update 2026-08-17 (later, real hardware): Phase D has REGRESSED — the
M3 BRAM load bug is back, in a new form, on the exact bitstream previously
verified working end-to-end. This supersedes everything above; nothing
about the ITM/TPIU stall can be trusted until this is fixed, since it
means the M3 may never have been executing the real firmware image in the
first place.**

Board reconnected (USB-Ethernet adapter plugged back in), confirmed
powered (JTAG saw `PS TAP`/`PMU`/`PL`) and reachable (`orbtrace info` →
`ZUBoard-Orbtrace/1` — **note: plain ICMP `ping` is not a reliable
liveness check for this board; the A53 control firmware's minimal network
stack appears not to answer ICMP echo reliably even when fully up and
TCP-reachable — use `orbtrace info` instead of `ping` from now on**).
Reflashed with the exact same known-good cache artifacts as Phase D's
original success (`bazel-out/orbtrace-vivado-hybrid-fix2/zub_orbtrace.bit`
+ `sdk/boards/zub_1cg/generated/psu_init.tcl`), rebuilt
`//applications/orbtrace/firmware/m3_app:m3_app` fresh (now including this
session's harmless `g_tpiu_sspsr_at_boot` addition).

Both load paths fail readback, reproducibly, across a full fresh reflash
(ruling out a one-off corrupted JTAG transfer):
- **D1** (`tooling/xsct/load_m3.tcl`, JTAG): word 0 (`_stack_top`) reads
  back correctly; word 1 (`reset_handler+1`) reads back `0x00000000`
  instead of the expected `0x000002b9`.
- **D2** (`orbtrace load-m3`, native A53 TCP store — no JTAG/DAP involved
  at all, so this isn't a debug-probe artifact): fails its own internal
  readback check outright (`orbtrace: readback mismatch: M3 BRAM does not
  contain the uploaded image`).

Direct probing (`mwr -force`/`mrd -force` over JTAG, bypassing both
loaders) makes the actual pattern clear and 100% reproducible across a
fresh reflash: writing 16 sequential words (`0x1000..0x100f`) to
`0xA0020000..0xA002003C` and reading back shows **only every 4th word
(16-byte/128-bit-aligned addresses) actually lands in BRAM — the 3 words
in between always read back `0x00000000` regardless of what was written**:
```
idx=0  addr=0xa0020000 val=0x00001000   (correct)
idx=1  addr=0xa0020004 val=0x00000000   (wrong — wrote 0x1001)
idx=2  addr=0xa0020008 val=0x00000000   (wrong — wrote 0x1002)
idx=3  addr=0xa002000c val=0x00000000   (wrong — wrote 0x1003)
idx=4  addr=0xa0020010 val=0x00001004   (correct)
... (pattern repeats every 4 words)
```
This is a **different failure signature from the original Phase D bug**
(which returned a single *fixed* constant, `0x00000008`, from every
address regardless of write — a tied-off disconnected BRAM port). This
one returns genuine per-address `0x00000000` at 3 out of 4 word addresses
and correct, distinct written values at the 4th — consistent with a
write (not read) path only accepting/completing transactions at 16-byte
granularity, silently dropping the 3 unaligned ones, on an AXI4 path
that's supposed to be uniformly 32-bit end-to-end.

**Ruled out already (static netlist check, free, no rebuild):**
`open_project`/`open_run impl_1` on the exact `orbtrace-vivado-hybrid-fix2`
implemented design confirms `control_ic` (the `smartconnect` between PS
`M_AXI_HPM0_FPD` and `m3_mem_ctrl`) really does have a 32-bit `S00_AXI`
(`S00_AXI_wdata[0:31]`, no more) — the earlier `PSU__MAXIGP0__DATA_WIDTH
{32}` fix genuinely took effect, and no `dwidth_converter`/
`protocol_converter` cell exists near `control_ic`. So the down-converter
hypothesis from the original Phase D investigation is NOT what's
happening here — this is something else, most likely inside
`m3_mem_ctrl` (`axi_bram_ctrl`) itself or `m3_mem`'s (`blk_mem_gen`)
actual generated cascade structure, even though `create_bd.tcl` declares
`Write_Width_A {32}`/`Write_Depth_A {16384}` explicitly and those look
correct on paper. **Not yet checked:** whether `m3_mem`'s *implemented*
BRAM primitives actually got cascaded/configured consistently with that
32-bit declaration — this design's own history already flags the
16384×32 True Dual Port RAM as needing 16 cascaded 36Kb tiles, a
non-trivial address-decode structure or per-cascade metadata that
`open_run`-level `get_cells`/`get_pins` inspection (same free technique
as the original Phase D root-cause) hasn't been pointed at yet this
session.

**Why this matters for everything above:** if the M3's firmware image was
never actually loaded correctly (75% of it silently zeroed), the CPU may
never have been running real `emit_next()` code during any of the
ITM/TPIU investigation above — the "CPU stuck on first blocking STIM
write" ILA finding could equally be explained by the CPU executing
corrupted/undefined instructions and landing somewhere that happens to
loop, rather than a genuine ITM/TPIU configuration issue. **This must be
fixed and re-verified before trusting any conclusion above it.** Given
the previously-recorded Phase D verification only ever tested two
addresses (`0xA0020000` and `0xA0020100`), both coincidentally 16-byte
aligned, it's plausible this bug existed the whole time and was simply
never exercised by that narrower test.

**Static netlist dig (free, no board time) done, found the real shape of
`m3_mem`'s cascade — genuinely 16 physical tiles, each only 2 bits wide:**
`open_run impl_1` + `get_cells -filter {REF_NAME =~ "RAMB*"}` on `m3_mem`
finds exactly 16 `RAMB36E2` primitives
(`...ramloop[0..15].ram.r/prim_noinit.ram/...SIMPLE_PRIM36...`), every one
reporting `READ_WIDTH_A=2 WRITE_WIDTH_A=2` (and same for the B port).
16 tiles × 2 bits = 32 bits — this is **width-cascading** (16 tiles in
parallel, each holding one 2-bit vertical slice of the 32-bit word, all at
the same address/depth), not the depth-cascading the earlier "16 cascaded
36Kb tiles" note assumed. This is a plausible, if inefficient, way for
Vivado's `blk_mem_gen`→`RAMB36E2` mapper to have realized
`Write_Width_A {32}`/`Write_Depth_A {16384}` given this cell's exact
attribute set (no `Use_Byte_Write_Enable`/`Byte_Size` set in
`create_bd.tcl`, so `axi_bram_ctrl` and `blk_mem_gen` may have negotiated
a single-WE-bit-per-tile shape instead of per-byte lanes).

**Not yet conclusive — this doesn't yet explain the observed *per-word*
(not per-bit) failure pattern.** A genuine WE-fanout bug across 16
bit-sliced tiles would be expected to corrupt specific *bit positions*
consistently across every word (e.g. always losing the same 2-8 bits of
every 32-bit value), not entire words at specific *addresses* while
neighboring words are perfect. The actual data on real hardware shows
whole-word correctness alternating by address (word 0 exactly right,
words 1-3 exactly zero, word 4 exactly right, ...), which points more at
an address/enable *decode* issue (e.g. per-tile chip-select or per-tile
address-bit wiring) than a bit-lane WE-fanout issue — but this hasn't
been traced pin-by-pin yet. Next free step: `get_pins`/`get_nets` on one
"good" tile (`ramloop[0]`, whichever slice backs word-index-0-mod-4) vs
one "bad" tile to see what's actually different in what drives their
`WEA`/`ADDRA`/enable pins — same technique that found the Phase D
`SINGLE_PORT_BRAM` bug. Not yet done this session.

**Paused here to report this finding before committing to more netlist
forensics or a real ILA rebuild cycle (~30-90 min) — this is a
significant, real regression from the previously-recorded "Phase D DONE"
state and changes what the rest of this document can be trusted to mean
until it's fixed.**

**Root cause found (free, static netlist inspection, user approved
continuing the dig): both `m3_mem_ctrl` and `m3_mem_ctrl_core` are
128-bit wide on their AXI side while `m3_mem` (the real BRAM) is genuinely
32-bit — a declared/actual width mismatch, same *class* of bug as the
original Phase D fix (an interface wider than what's actually wired gets
its unconnected portion silently tied off), just in AXI data width this
time instead of BRAM port count.**

Confirmed directly on the implemented netlist:
- `get_pins */m3_mem_ctrl/U0/*s_axi_wdata*` → **128** pins (not 32).
- `get_pins */m3_mem_ctrl_core/U0/*s_axi_wdata*` → **128** pins too — the
  M3 core's own code-fetch controller has the identical mismatch, not
  just the PS-preload one.
- `m3_mem_ctrl/U0/bram_rddata_a[*]`/`bram_wrdata_a[*]` → 256 pins (128
  read + 128 write) declared on the controller's `BRAM_PORTA` side, even
  though `m3_mem` (`blk_mem_gen`) is genuinely `Write_Width_A {32}` (16
  `RAMB36E2` tiles × 2 bits each = 32 bits total, confirmed above).
- `create_bd.tcl` never sets `CONFIG.DATA_WIDTH` on either
  `m3_mem_ctrl` or `m3_mem_ctrl_core` — only `CONFIG.SINGLE_PORT_BRAM {1}`
  is set on them (the Phase D fix). Both silently took some non-32-bit
  default/auto-negotiated width instead.

**This exactly explains the observed 4:1 word-drop, including the read
side:** a 32-bit AXI write from the PS lands (via `control_ic`'s internal
width handling — no separate discrete `dwidth_converter` cell needed,
`smartconnect` can do this internally) at the correct byte lane of a
128-bit-aligned "container" based on address bits `[3:2]`. But
`axi_bram_ctrl` believes it's driving a genuinely 128-bit-wide BRAM word
per container address, while its physical `BRAM_PORTA` data connection to
the real `m3_mem` is only ever 32 bits (the low lane) — the other 96 bits
of what it thinks is BRAM_PORTA are unconnected/tied off by the
auto-generated wrapper, structurally identical to the original Phase D
tied-off-port mechanism. So: writes at byte-lane 0 (word index ≡ 0 mod 4)
reach real memory and read back correctly; writes at lanes 1/2/3 go
nowhere, and reads of those lanes return whatever constant the
unconnected wrapper bits are tied to (0), regardless of what was written
— matching every byte of the observed data exactly, including why
non-lane-0 addresses read back `0`, not the lane-0 word's value.

**Why this also unifies with the entire Phase E ITM/TPIU stall
investigation above: `m3_mem_ctrl_core` (the M3 core's own
`CM3_CODE_AXI3` instruction-fetch controller) has the identical bug.**
If the M3 has only ever correctly fetched every 4th instruction word and
read `0x00000000` for the other three, its actual instruction stream is
mostly zeroed — and `0x0000` decodes as a valid, harmless 16-bit Thumb
NOP-equivalent (`movs r0, r0`). A CPU NOP-sliding through 75%-zeroed
memory, only occasionally executing a real instruction when the PC lands
on a 16-byte-aligned word, could easily *look* like a permanent stall
(never reaching real branch/loop-control logic scattered into the lost
lanes) without ever actually being an ITM/TPIU register configuration
problem at all. **Everything in the Phase E section above must be
considered unverified until this is fixed and Phase D is re-run.**

**Update 2026-08-17 (two rebuild cycles later): the fix does NOT work —
same 4:1 pattern persists — and the real trigger has been narrowed to
`control_ic` (smartconnect), not `m3_mem_ctrl`'s own DATA_WIDTH.**

- **Attempt 1:** `CONFIG.DATA_WIDTH {32}` alone on both controllers. Real
  rebuild (user-approved), flashed, retested: identical 4:1 pattern.
  Netlist check (`open_run impl_1`) showed `s_axi_wdata` still 128 bits on
  both controllers — the property had simply been silently overridden
  somewhere downstream of `create_bd.tcl`, never took effect at all.
- **Attempt 2:** added `CONFIG.MEM_DEPTH {16384}` alongside (matching
  `m3_mem`'s real depth), on the theory that `MEM_DEPTH` — marked
  `propagate_only`, auto-derived independently of the connected external
  BRAM's actual geometry — was the free variable `assign_bd_address`'s
  address-range reconciliation used to silently recompute width upward
  (DEPTH known, RANGE fixed at 64K → solves WIDTH). **Verified this held
  through a fast BD-only elaboration first** (a `create_bd.tcl` source +
  `validate_bd_design` + `generate_target` probe, no synthesis, ~1 minute
  — mirrors the technique from Phase A/B's original verification harness):
  confirmed `CONFIG.DATA_WIDTH=32`/`CONFIG.MEM_DEPTH=16384` on both cells
  post-`generate_target`, and the XCI itself shows
  `"DATA_WIDTH": {"value": "32", "value_src": "user", ...}`. Real rebuild,
  flashed, retested anyway (since BD-level holding doesn't guarantee
  synthesis-time behavior) — **still the identical 4:1 pattern.** Netlist
  check on this second build's actual implemented design: `s_axi_wdata`
  still 128 bits on both controllers, despite the XCI-level config being
  provably correct. Something during `launch_runs synth_1` itself
  re-widens the controllers, independent of their own declared
  `CONFIG.DATA_WIDTH`.
- **New finding, likely the real trigger:** `control_ic` (the
  `smartconnect` between PS and `m3_mem_ctrl`/`trace_pl`/`trace_dma`) has
  an internal crossbar/pipeline stage that is genuinely 128-bit — a
  differently-cased pin-name query (`*control_ic*s00_axi_wdata*`,
  matching a *different* internal stage than the `S00_AXI_wdata` boundary
  pins checked earlier) found 128 wires, even though the true S00
  boundary (right at the PS connection) is still confirmed 32-bit. This
  is consistent with smartconnect internally upsizing to some wider
  shared crossbar width (chosen to satisfy some other requirement,
  possibly unrelated to any of `create_bd.tcl`'s explicit settings) and
  then *not* narrowing back down specifically on the M02 leg (to
  `m3_mem_ctrl`) the way it evidently does correctly for M00/M01 (to
  `trace_pl`/`trace_dma`, both working correctly, both 32-bit natively).
  `create_bd.tcl` sets no explicit per-leg width on `control_ic` at all —
  only `CONFIG.NUM_MI {3}` — so this crossbar width and per-leg narrowing
  is entirely smartconnect's own automatic (and, on this leg,
  apparently wrong) negotiation.

**CORRECTION 2026-08-17 — the 128-bit finding above that drove both
rebuild cycles was a measurement error. Both controllers were genuinely
32-bit all along, including in the ORIGINAL bitstream before any of this
session's `create_bd.tcl` changes. The real 4:1 word-drop bug is still
completely unexplained.**

Before spending a third rebuild on the "swap smartconnect for
axi_interconnect" idea above, checked `m3_core_ic`'s own `M00_AXI` (the
axi_interconnect leg *already* feeding `m3_mem_ctrl_core` — no
smartconnect involved on that path at all) directly: it correctly shows
32 bits, refuting the "smartconnect specifically" theory before wasting
the cycle. Digging into why `m3_mem_ctrl_core` could show 128 bits
despite its own upstream interconnect being genuinely 32-bit led to
re-checking the measurement itself — and that's where the error was
found.

The `get_pins -hier -filter {NAME =~ "*$ctrl/U0/*s_axi_wdata*"}` query
used throughout this investigation matched 128 pins on both controllers
across three separate builds — but a precise re-check restricted to pins
directly on the `U0` cell boundary (`get_pins -of_objects [get_cells
.../m3_mem_ctrl/U0] -filter {NAME =~ "*s_axi_wdata*"}`) shows **32 pins**
on both controllers, matching the real generated VHDL instantiation
(`C_S_AXI_DATA_WIDTH => 32, C_MEMORY_DEPTH => 16384`, read directly out
of `zub_orbtrace_m3_mem_ctrl_0.vhd`'s `GENERIC MAP`). The loose `-hier`
wildcard was evidently matching some other, unrelated internal signal
elsewhere in the hierarchy that merely fit the pattern — not the real
port — and this wasn't caught until after two full rebuild cycles had
already been spent chasing it.

**What this means:** the `CONFIG.DATA_WIDTH {32}` / `CONFIG.MEM_DEPTH
{16384}` additions already applied to `create_bd.tcl` are harmless (they
make explicit what was already true) but fixed nothing, because there was
nothing wrong at that level to begin with. The identical 4:1 word-drop
across the original cached bitstream and both rebuilds has a different,
still-unknown cause.

Ruled out along the way, and these conclusions remain valid regardless of
the width-measurement correction: a JTAG/debug-probe access-size artifact
(D2, `orbtrace load-m3`, uses genuine native A53 STR instructions with no
JTAG involved at all, and fails identically to D1) and a simple
address-scaling/shift bug (self-inconsistent with the observed data — a
consistently-wrong address formula would still read back whatever was
written at that same wrong location on a subsequent read, not the
mix of correct-value/zero actually observed).

Static netlist/BD-property reasoning has now produced one confirmed false
lead on this specific bug, after being reliable for the original Phase D
bug. The same class of investigation (properties, generated sources,
interface parameters) has been checked exhaustively at this point without
finding the real mechanism. **The next real step is ILA hardware capture
on `m3_mem_ctrl/S_AXI`** — ground-truth signal observation on real
hardware, the same methodology that actually found the original Phase D
bug — rather than further static reasoning about configuration. Two full
rebuild cycles (~30 min each, thanks to the cached M3 OOC EDIF) were spent
on the now-corrected width theory; this session's Vivado-forward progress
on this specific bug is exhausted without an ILA capture.

**Update 2026-08-17 (continued): real root cause found via real ILA
capture — the bug is NOT a width mismatch anywhere. It's a write-channel
sequencing fault, most likely inside `control_ic` (smartconnect) itself,
common to both the JTAG and native A53 write paths.**

Built a third diagnostic bitstream (default P&R directives, CDC/
methodology/timing gates downgraded to warnings-only, same proven
methodology as the original Phase D ILA build) with a `system_ila`
(`C_MON_TYPE=INTERFACE`) tapped as a 3-way monitor on the existing
`control_ic/M02_AXI` ↔ `m3_mem_ctrl/S_AXI` net (AR/R channel capture
dropped, depth 1024, to fit this part's LUT budget — same constraints as
the original Phase D ILA build). Build succeeded cleanly (no CDC/
methodology/timing violations were actually hit even with the gates
downgraded — a genuinely clean design). Flashed via `jtag_flash.sh` (not
a raw `program_hw_devices` reprogram), armed via Vivado's own Hardware
Manager (`open_hw_manager`/`connect_hw_server`/`refresh_hw_device` with
`PROBES.FILE` set to the impl run's `.ltx` — attaching to the
already-programmed device, not reprogramming it).

**First capture (triggered on first AWVALID, JTAG `mwr` writes as the
stimulus): only caught a single beat.** Individual JTAG `mwr` calls are
each their own slow, independent JTAG/hw_server round-trip — nowhere near
back-to-back — so the ILA's ~10µs (1024-sample @ ~100MHz) window closed
long before a second beat arrived. Not useless: it confirmed the ILA tap
itself works (captured a real, correct AWADDR=0/WDATA=0x1000/WSTRB=f/
BRESP=OKAY transaction) and revealed the fast-burst-vs-slow-JTAG timing
mismatch that needed working around.

**Second capture, surgical: re-armed with `TRIGGER_COMPARE_VALUE` on
`AWADDR` itself (`eq16'h0004`, the first address that always fails
readback), single JTAG `mwr -force 0xa0020004 0xcafef00d`, confirmed the
write "succeeded" (`BRESP=OKAY`) but read back `0` immediately after
(reproducing the bug live) — then read the ILA. The captured window shows
`WDATA` stuck at `0000100c` (a stale value from a completely unrelated
*earlier* xsct session, still sitting in the write-data path) with
`WSTRB=0x0` for the entire visible transaction, including the one cycle
where `WVALID`/`WREADY` handshake — i.e. the write-data channel simply
never got the new value at all, and asserted zero byte-enables.**

**Third capture, decisive: re-armed on first AWVALID again, this time
using `orbtrace load-m3` (D2, genuine native A53 STR instructions, zero
JTAG involvement) as the stimulus — a real CPU executing a tight store
loop is fast enough that the whole visible sequence landed inside one
capture window. This is unambiguous ground truth:**
```
AWADDR=0x0000  WDATA=0x00010000  WSTRB=f   (real data)
AWADDR=0x0004  WDATA=0x00010000  WSTRB=0   (stale — repeats the PREVIOUS beat's data)
AWADDR=0x0008  WDATA=0x00010000  WSTRB=0   (stale — still repeating)
AWADDR=0x000c  WDATA=0x00010000  WSTRB=0   (stale — still repeating)
AWADDR=0x0010  WDATA=0x000002d3  WSTRB=f   (real data — matches default_handler+1)
AWADDR=0x0014  WDATA=0x000002d3  WSTRB=0   (stale again)
AWADDR=0x0018  WDATA=0x000002d3  WSTRB=0
AWADDR=0x001c  WDATA=0x000002d3  WSTRB=0
AWADDR=0x0020  WDATA=0x00000000  WSTRB=f   (real data — word 8 is legitimately 0)
AWADDR=0x0024  WDATA=0x00000000  WSTRB=0
AWADDR=0x0028  WDATA=0x00000000  WSTRB=0
AWADDR=0x002c  WDATA=0x00000000  WSTRB=0
AWADDR=0x0030  WDATA=0x000002d3  WSTRB=f   (real data — matches default_handler+1)
AWADDR=0x0034  WDATA=0x000002d3  WSTRB=0
```
Every `BRESP` for all 14 of these was `OKAY`. The pattern is exact and
unconditional: **the 1st write of every 4-word (16-byte) block gets its
real `WDATA` with `WSTRB=0xF`; the following 3 writes each get the
*previous* block's real value (not their own intended value — genuinely
stale, one write-data-cycle behind) with `WSTRB=0x0`**, so nothing is
actually written for 3 out of every 4 words, and the AXI protocol reports
success regardless. This holds identically for JTAG-originated and
native-A53-originated writes, which only share one thing in common: the
`M_AXI_HPM0_FPD` → `control_ic` → `m3_mem_ctrl` path.

**Width is conclusively ruled out as the cause this time — checked
precisely (cell-boundary pins, not loose wildcards) at every hop:** PS's
own `ps/inst` boundary `maxigp0_wdata` = 32 pins; `control_ic/inst`
boundary `S00_AXI_wdata` = 32 pins; `control_ic/inst` boundary
`M02_AXI_wdata` = 32 pins; `m3_mem_ctrl` `s_axi_wdata` = 32 pins (already
confirmed earlier). Every hop is genuinely, uniformly 32-bit. This is a
**write-data pacing/sequencing bug**, not a width bug — most likely
inside `control_ic` (smartconnect)'s own write-channel pipelining/
arbitration logic (the one component common to every path that exhibits
this), given `m3_core_ic` (the `axi_interconnect` feeding
`m3_mem_ctrl_core`) has not been differentially tested yet — the earlier
"m3_mem_ctrl_core also shows 128-bit" finding that seemed to implicate it
was itself part of the retracted width-measurement error, so whether
`m3_mem_ctrl_core`'s *own* write-data sequencing has the same bug (via a
different interconnect) is still open and worth checking before assuming
this is smartconnect-specific.

**Not yet tried:** (1) differentially test whether `m3_mem_ctrl_core`
(behind `m3_core_ic`, an `axi_interconnect`, not smartconnect) shows the
same stale-data/WSTRB=0 pattern via a native M3-core-side write (harder
to stimulus — the M3 can't fetch/run code to write its own memory yet,
so this may need a JTAG-driven write through the M3's own DAP once Phase
G's real-DAP route is usable, or ILA-tapping `m3_core_ic`'s output
instead and finding another way to generate traffic); (2) if this does
turn out smartconnect-specific, replace `control_ic`'s connection to
`m3_mem_ctrl` with a dedicated `axi_interconnect` or an explicit
`axi_register_slice`, isolating it from whatever in smartconnect's shared
3-master arbitration (M00 `trace_pl`, M01 `trace_dma`, M02 `m3_mem_ctrl`)
causes this; (3) research whether this is a known `smartconnect`
erratum for this Vivado version/usage pattern (single-outstanding,
back-to-back same-slave AWLEN=0 writes) rather than something fixable in
`create_bd.tcl` at all.

**Update 2026-08-17 (continued): attempting fix (2) — interposing a
dedicated `axi_interconnect` between `control_ic`'s M02 leg and
`m3_mem_ctrl`.**

First tried giving `m3_mem_ctrl` a fully separate PS master port
(`M_AXI_HPM1_FPD`, `PSU__USE__M_AXI_GP1`) instead, on the theory that
completely bypassing `control_ic` (not just re-timing after it) would be
the more conclusive test. Caught by a fast BD-only elaboration probe
(`validate_bd_design`/`assign_bd_address`, no synthesis, ~1 minute, same
technique as the DATA_WIDTH/MEM_DEPTH check two updates up) before wasting
a real build on it: real Vivado's address-decode rejects `0xA0xxxxxx` —
the address firmware (`sdk/bsp/m3/memory.lds` indirectly, via
`m3_mem_ctrl`'s fixed offset) and host tooling (`load_m3.tcl`'s
`m3_bram_base`, `orbtrace load-m3`) already hardcode — as unreachable
through `M_AXI_HPM1_FPD` at all: `"must fit an available aperture ...
{<0xB000_0000 [256M]>, <0x5_0000_0000 [4G]>, <0x48_0000_0000 [224G]>}"`.
Changing the address would mean touching firmware and host-tooling
constants too, well beyond the scope of this fix, so this option is
dropped in favor of the smaller change.

**What's actually being tried:** `create_bd.tcl` now inserts
`m3_mem_ctrl_ic` (a plain `xilinx.com:ip:axi_interconnect`, `NUM_SI
{1}`/`NUM_MI {1}`, same IP type as the already-proven `m3_core_ic`)
between `control_ic/M02_AXI` and `m3_mem_ctrl/S_AXI` — same address path
as before (segment names under `/ps/Data` are unchanged, confirmed by the
same fast BD-only probe), just one extra AXI hop. `control_ic` stays at
`NUM_MI {3}`, unchanged. Verified clean via the fast BD-only probe first
(no new errors/critical warnings beyond the pre-existing, already-explained
`MEM_DEPTH`-is-read-only and `m3_core/WICENREQ`-has-no-pin ones) before
committing to a real rebuild.

**Real build launched** (`M3_OOC_EDIF` pointed at the cached
`bazel-out/m3-ooc-2019/m3_core.edf`, output to a fresh
`bazel-out/orbtrace-vivado-hybrid-fix3` so `hybrid-fix2`'s known-good
bitstream stays available as a fallback) — result not yet known as of this
write; see the next update for the outcome (build success/failure, gate
results, and real-hardware retest).

**Update 2026-08-17 (continued): build succeeded, all gates passed, and
the fix is CONFIRMED on real hardware — the 4:1 word-drop bug is fixed.**

Build completed clean: `synth_design Complete!`, `write_bitstream
Complete!`, zero unwaived CDC violations, zero unwaived methodology
violations, positive setup and hold slack. Flashed via `jtag_flash.sh`
(full `rst -system`-based reflash, not a raw reprogram) to
`bazel-out/orbtrace-vivado-hybrid-fix3/zub_orbtrace.bit` +
its own generated `psu_init.tcl`, with `//...a53_app:a53_app` as the ELF.
`orbtrace info` confirmed the board back up.

Retested with `tooling/xsct/load_m3.tcl` (D1, JTAG): **all 4 verified
words now correct** (`0x00010000`, `0x000002b9`, `0x000002d3`,
`0x000002d3`) — previously only word 0 survived. Followed up with a wider
direct `mwr`/`mrd` sweep (16 sequential words across 4 separate 16-byte
blocks, the same pattern that originally exposed the bug): **16/16
correct**, zero mismatches. The `m3_mem_ctrl_ic` isolation fix works —
`control_ic` (smartconnect) really was the fault, and routing
`m3_mem_ctrl` through a dedicated `axi_interconnect` instead of
`control_ic`'s M02 leg genuinely fixes the write-data pacing fault, not
just masks it under slow JTAG timing.

**Second, separate finding — a pre-existing hang bug in the M3_CONTROL
register path, unrelated to this fix and not a regression from it:**
testing D2 (`orbtrace load-m3`, the native A53-side load path) on the
*new* `fix3` bitstream, the very first command it issues —
`Command::M3Control` (`applications/orbtrace/model/src/main.rs`'s
`load_m3()`, step "[1] Holding M3 in reset...", which does
`self.io.write(REG_M3_CONTROL, 0)` at `firmware/a53/src/lib.rs:284`,
`REG_M3_CONTROL = 0xa0` inside `trace_pl`'s own register block) — hung
the CLI with `orbtrace: Resource temporarily unavailable (os error 11)`
(a 5-second TCP read timeout, per `model/src/main.rs`'s
`set_read_timeout(Some(Duration::from_secs(5)))`, not a special error).
Every subsequent command (`info`, `stop`, `reset`) hung identically until
a full JTAG reflash recovered the board — consistent with the A53 CPU
itself freezing mid-instruction on the `trace_mmio_write` store to that
register (a plain `*(volatile u32*) = value` in
`firmware/a53_app/src/main.c`, no software wait-loop involved) rather
than any software-level bug, i.e. the physical AXI write to that specific
register genuinely never completes (no `BRESP`) — a real RTL/hardware
issue, not a CLI or firmware logic bug.

**Confirmed NOT caused by today's `create_bd.tcl` fix:** reflashed back
to the untouched, previously-verified-good `hybrid-fix2` bitstream from a
completely clean boot, confirmed `info` (x3) and `stop` both succeed
instantly and reliably with no prior M3 interaction — then the *same*
`load-m3` D2 call hung identically on its very first `M3Control` command,
on a bitstream this session never touched. `REG_M3_CONTROL` (offset
`0xa0`) lives inside `trace_pl`'s `orbtrace_axi_regs` submodule
(`applications/orbtrace/rtl/orbtrace_pl.v`), reached via `control_ic`'s
*M00* leg (`trace_pl`, not `m3_mem_ctrl`'s M02 leg this session's fix
touched) — and `stop`, which also writes an `orbtrace_axi_regs` register
(`CONTROL`, offset `0x08`) over that exact same M00 leg, works instantly.
So this is specific to the `M3_CONTROL` register's own AXI write-handling
logic inside `orbtrace_axi_regs` — not `control_ic` generally, and not
anything this session's fix inserted or touched.

**Why this was never caught before:** the plan's last recorded D2 test
(the 2026-08-17 "Phase D has REGRESSED" update, before today's fix) *did*
get past this exact step — `orbtrace load-m3`'s own log showed it reaching
step 3 ("readback mismatch") before failing, meaning `Command::M3Control`
completed successfully at that time. Between then and now the only
material change to real hardware state is the number of full
reflash/reset cycles this board has been through — whether that's a red
herring or an actual clue (e.g. some latch/register that only sticks
after N resets, a marginal timing path, a JTAG-vs-power-on-reset
difference) is unknown and not yet investigated.

**Net status:** this session's actual goal — fixing the 4:1 BRAM
word-drop bug — is DONE and verified via D1 (JTAG). D2 (native A53 load)
is blocked by this separate, pre-existing `M3_CONTROL` hang, which needs
its own investigation (most likely another real ILA capture, this time
tapping `trace_pl`'s `s_axi` port around the `M3_CONTROL` write, or
inspecting `orbtrace_axi_regs`' RTL directly for anything
address-specific around offset `0xa0`) before D2, and therefore the rest
of Phase E/F/G (all of which assume a running M3, releasable via either
load path), can proceed. The board has been left on the known-good,
unmodified `hybrid-fix2` bitstream (responsive, `orbtrace info`
confirmed) — not `fix3` — since flashing `fix3` provides no additional
usability until the `M3_CONTROL` hang itself is fixed (D1 still needs a
fresh `m3_app.bin` reload after every reflash regardless of which
bitstream is used, and the hang blocks D2 either way).

### Next steps

1. ~~Root-cause the `M3_CONTROL` AXI write hang~~ — **RE-SCOPED, see the
   2026-08-17 (continued) update immediately below: this is NOT an AXI/RTL
   hang at all.** The `orbtrace_axi_regs`.sv RTL read (free, no hardware)
   ruled out an address-specific hardware bug; a live JTAG investigation
   then proved the actual mechanism is a NetX/Ethernet-stack block, not
   anything in `create_bd.tcl`/`orbtrace_pl.v`/`m3_mem_ctrl_ic`. See below
   for the real next step.
2. **Once D1 or D2 reliably releases a correctly-loaded M3**, resume the
   still-open Phase E ITM/TPIU STIM-FIFO-stall investigation from the
   2026-08-17 (earlier) updates — now finally trustworthy, since the
   firmware image loads completely instead of being 75% zeroed. Read
   `g_tpiu_sspsr_at_boot` (already latched by this session's firmware
   change) via JTAG halt to settle the "does this synthesized instance
   support 4-bit parallel trace at all" question before another ILA
   cycle.
3. Then Phase F/G as originally planned.

**Update 2026-08-17 (continued): root-caused the `M3_CONTROL` hang —
it is NOT an AXI/hardware/RTL bug. `control_thread` is blocked inside
NetX's `_nx_tcp_server_socket_accept`, not anywhere near the AXI write.**

Investigated entirely via free (no-rebuild) JTAG/xsct inspection of the
already-flashed `hybrid-fix2` board, in this order:

1. **RTL read (`orbtrace_axi_regs.sv`), free:** the register block's write
   path (`write_fire`/`s_axi_bvalid`) asserts unconditionally for ANY
   address via a `case` statement with a harmless `default: ;` arm —
   `ORBTRACE_REG_M3_CONTROL` (`0xa0`) is handled exactly like every other
   register, including `CONTROL` (`0x08`, used by the already-working
   `stop`/`start`/`reset` commands). No address-specific gating exists
   here at all — ruling out the RTL as the cause before touching hardware.
2. **Exception-vector check (`tx_initialize_low_level.S`), free:** the
   EL3 vector table's Synchronous and SError entries are both a bare
   `B .` (infinite self-branch) — if the AXI write had actually faulted
   (SLVERR/DECERR/bus timeout → Data Abort/SError), the CPU would be
   frozen at that exact, fixed vector address forever. It never was.
3. **Live PC sampling via repeated JTAG halt/resume:** the CPU is mostly
   sitting in ThreadX's own idle scheduler loop (`_tx_thread_schedule`),
   not spinning on anything AXI-related.
4. **Peeked the Rust FFI's global `LOCK` (`AtomicBool`) directly at its
   symbol address (`0x937b0`):** reads back `0x00` — not held. Rules out
   a stuck mutex/unreleased lock from a `panic=abort` panic inside the
   `self.io.write(REG_M3_CONTROL, ...)` critical section (the panic
   handler is a bare `loop {}` with no unwinding, so a panic there would
   leave `LOCK` permanently `true` — it doesn't).
5. **Waited 90s post-hang and retried `info`:** still fails identically —
   rules out a slow-but-eventually-successful TCP retry/backoff.
6. **Decisive: walked `control_thread`'s ThreadX TCB directly** (found via
   `nm` on the ELF: `control_thread` at `0x53aa0`; `TX_THREAD_STRUCT`
   layout from the vendored `tx_api.h`/`tx_port.h` for this
   `cortex_a53/gnu` port, `ULONG`/`UINT` both 4 bytes). Confirmed a valid
   TCB (`tx_thread_id` = `0x54485244` = ASCII `"THRD"`, the ThreadX magic;
   `tx_thread_name` pointer dereferences to `"orbtrace_control"`).
   **`tx_thread_state = 0x0000000c` = `TX_TCP_IP`** (from `tx_api.h`'s own
   state enum) — the thread is genuinely suspended inside a NetX TCP/IP
   call, not busy-spinning on anything. Walked the saved stack from
   `tx_thread_stack_ptr` (`0x76e50`) and found return addresses that
   `addr2line` resolves to **`_nx_tcp_server_socket_accept`, called from
   `control_thread_entry`** (`main.c`'s outer `for(;;)` accept loop, after
   the previous connection's `nx_tcp_socket_disconnect`/
   `nx_tcp_server_socket_unaccept`/`_relisten` sequence) — **not** anywhere
   inside `Controller::command()`'s opcode-9 handling, `Mmio::write()`, or
   `serve_control()`'s response-send path.

**What this means:** by the time this state was captured, the M3Control
request had almost certainly already been fully handled (matching
`LOCK=0`) and the connection torn down — `control_thread` had already
looped back around to wait for the *next* TCP connection, which then
never completes. This points at the GEM2 Ethernet driver / NetX socket
lifecycle (accept/disconnect/relisten sequence, specifically after
handling a `control` connection that behaves differently from `info`'s —
possibly tied to `nx_tcp_server_socket_listen`'s backlog of `1U` in
`main.c`, or a lost/dropped reply packet on the GEM2 TX/RX path this
codebase's own extensive pre-existing `diag`/`diag2`-`diag8` `xil_printf`
instrumentation in `main.c` was clearly already built to chase, per its
comments about GEM2/NetX bring-up issues), **not `create_bd.tcl`, not
`orbtrace_pl.v`/`orbtrace_axi_regs.sv`, and not anything this session's
`m3_mem_ctrl_ic` fix touched.** No physical serial console is available
in this environment (`/dev/ttyUSB*`/`/dev/ttyACM*`: none present) to read
the existing diagnostic prints directly, which would otherwise be the
fastest way to see exactly what the GEM2/NetX diagnostics already report
at the moment of the hang.

**Real next step (different domain from anything else in this doc — no
more Vivado/RTL work indicated):** either (a) get a physical serial
cable connected to this board's UART and read the already-extensive
`diag`/`diag2`-`diag8` output live during a reproduction, by far the
fastest path given how much relevant instrumentation already exists, or
(b) continue via JTAG-only register/struct inspection — decode the
`NX_TCP_SOCKET`/GEM2 BD-ring C structs referenced by `main.c`'s own diag
prints (`rx_bd_base`, `txqbase`, etc. — same technique used here for the
ThreadX TCB) to see the actual socket/ring state at the moment
`_nx_tcp_server_socket_accept` is stuck, without needing a UART at all.

**Update 2026-08-17 (continued): took path (b) — pinned it down further.
`control_socket.nx_tcp_socket_state = 4` (`NX_TCP_SYN_RECEIVED`) at the
moment of the hang. The TCP 3-way handshake never completes; it does not
just fail to see a new connection attempt.**

Found `nx_tcp_socket_state`'s exact offset (`+84`/`0x54`) the same way as
the ThreadX TCB fields — not from NetX Duo's header layout (which would
need computing through `NX_TCP_SOCKET`'s many preceding fields by hand,
error-prone) but straight from the **compiler's own generated code**:
disassembled `_nx_tcp_server_socket_accept` itself
(`nx_tcp_server_socket_accept.o`, address `0x7140` in the ELF) and read
off its own `ldr w0, [x0, #84]; cmp w0, #0x5` (checking for
`NX_TCP_ESTABLISHED`) at the very top — a technique worth reusing for any
future NetX/ThreadX struct-offset need, since it's immune to hand-arithmetic
mistakes (which is what happened trying to hand-derive `Gem2Ctx`'s layout
byte-by-byte from its `.c` struct definition below — `tx_buf`'s declared
`[64][10240]` size alone (655,360 bytes) doesn't fit inside the compiled
struct's actual reported size (`nm -S`: `0x19640` = 104,000 bytes) at
all, meaning something about the field either isn't what the source
currently shows or isn't sized the way it looks — never resolved, and
irrelevant once the compiler's own generated offsets were used instead
via the same `gem2_diag_get*` accessor-disassembly method).

Read `control_socket` (`0x534e8`, from `nm`) `+84` directly via JTAG
while the hang was live: **`4` = `NX_TCP_SYN_RECEIVED`**, not `2`
(`NX_TCP_LISTEN_STATE`, which would mean "genuinely idle, nothing
attempted a connection yet"). A client's SYN was received and (per
`_nx_tcp_server_socket_accept`'s own logic, which treats `LISTEN`/
`SYN_RECEIVED` as the two "keep waiting" states before calling
`_nx_tcp_socket_thread_suspend`) the board's side of the handshake got
as far as sending a SYN-ACK — but the final ACK that would move the
connection to `ESTABLISHED` never arrived or never got processed, and it
just sits there forever (waited past 90s with no recovery, confirmed
earlier). This also revises the earlier "the M3Control request was
already handled" reading of `LOCK=0` — more likely `orbtrace_control_feed`
was **never called at all** for this connection, since NetX never hands
an unestablished connection up to the application; `LOCK=0` is simply
because nothing ever acquired it for this connection, not evidence of a
completed handshake.

**GEM2 hardware/DMA itself checked clean at the same moment (rules out a
DMA/BD-ring stall as the cause):** `diag_isr_calls=37`, `diag_rx_frames=22`,
`diag_tx_frames=13` (real interrupt-driven traffic has been flowing all
session, not frozen); `tx_head==tx_tail==13`, `tx_count=0` (TX ring fully
drained, nothing in flight); all 4 dumped TX BD slots show the hardware
`USED` bit set (`stat` values all have bit31 set, e.g. `0x80008036` —
successfully completed, not stuck); all 4 dumped RX BD slots show
`stat=0`, consistent with `rxused_count=0` (nothing currently pending,
not backed up); `GEM2_NWSR`/`ISR`/`RXSR` registers show ordinary
steady-state values, nothing resembling a link-down or DMA-halt pattern.
`diag_tx_recover_attempts=2` (the existing watchdog *has* intervened
twice this session, confirming the driver's own self-healing logic is
active and not itself wedged) but that's a separate, already-known
recovery path, not what's blocking `control_socket` right now.

**Not yet determined:** *why* this specific handshake's final ACK is
lost/unprocessed — `diag_last_tx_dst_msw/lsw` (the last-TX destination
MAC, which would settle the "wrong ARP entry" hypothesis directly) read
back `0xefefefef` (an uninitialized-memory pattern), meaning that
particular diagnostic capture point was never exercised this session and
can't confirm or rule out a stale/wrong ARP-cache-driven MAC on the
SYN-ACK. Whether this specific stuck connection is the CLI's own
`load-m3` attempt (most likely, given it's the deterministic trigger
every time) or some later retry queued behind it (given
`nx_tcp_server_socket_listen`'s backlog of `1U` means only one pending
connection can ever be tracked at a time) is also not yet distinguished —
either way, `backlog=1U` means whichever connection gets stuck this way
blocks every later one until a reflash.

**Next step, concrete and cheap (A53 firmware rebuild + `jtag_flash.sh`
only — no Vivado rebuild needed):** try raising
`nx_tcp_server_socket_listen`'s backlog (`main.c`, currently `1U`) so a
single stuck half-open connection can't starve every later attempt, as a
first mitigation to test/confirm this theory in practice — this would
very likely unblock D2 even without understanding *why* one handshake
gets stuck, though it wouldn't explain the root cause. Not yet
implemented this session — flagged here for the next session or for
explicit approval before changing firmware behavior.

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
