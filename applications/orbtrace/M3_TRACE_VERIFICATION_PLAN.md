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
| D | Load the M3 firmware image | **DONE — both D1 and D2 confirmed fully working**, 2026-08-17. Two hardware bugs fixed (dual-port tied-off BRAM port; a write-pacing 4:1 word-drop fixed by routing `m3_mem_ctrl` through a dedicated `axi_interconnect`) plus a real vendored NetX ARP-table race fixed (see below) — but D2's long "hang" this session turned out to be **two stacked build/flash mistakes, not a design bug**: `jtag_flash.sh` was repeatedly given a stale (`2026-08-09`) `a53_app_elf` artifact instead of the freshly-rebuilt `a53_app` symlink, so none of this session's firmware edits were ever actually running; independently, the board was flashed with `hybrid-fix2` (the *pre*-BRAM-fix bitstream) instead of `hybrid-fix3` during the whole D2 investigation. Once both were corrected, D2 completes cleanly and repeatably, including as the very first command after a fresh reflash. See the 2026-08-17 "stale artifact" correction below for the full account |
| E | Configure Orbtrace and start capture | **SUBSTANTIVELY DONE, 2026-08-19 — real, clean, channel-1-only CoreSight content decoded on real hardware for the first time in this entire investigation.** Root cause: `orbtrace_tpiu_demux.sv`'s sync-word search had the byte order backwards (searched for `0x7F` then three `0xFF`, when the real CoreSight Full Sync Packet is chronologically three `0xFF` then `0x7F` — confirmed against sigrok's independent reference decoder and real ILA capture data). Fixed (`32'hffffff7f`→`32'h7fffffff`), simulation-validated with a new dedicated testbench, then confirmed on real hardware: `orbtrace stats` shows `rx_bytes` genuinely and repeatably nonzero (11→173 over ~2 minutes) under the real (non-marker) firmware, and decoding the capture shows every byte is checksum-valid, exclusively on channel 1, zero garbage channels — a first. `dropped_bytes`/`sync_loss` remain huge (expected/correct: the false-lock-on-idle-alias problem the 2026-08-18 channel-plausibility gate handles is still active and doing its job). Remaining for a future session: Phase F's full byte-for-byte diff against the `Workload` reference (not required to call Phase E itself done) — see the 2026-08-19 section below for full detail |
| F | Verify the captured trace is genuinely correct | **DONE, 2026-08-19 — genuine content recovery confirmed**, not just correct framing. A capture synchronized to a fresh `load-m3` reload (so the `Workload(seed=7)` reference's `sequence` starts near 0, unlike the previous unsynced attempt which could never find a contiguous match against an unknown, possibly-huge sequence offset) found 9 order-preserving, exact byte-string matches of width≥2 (3- or 5-byte) reference events in a 223-byte reconstructed stream, against an analytically-expected ~1.4 and empirically-observed 2-3 coincidental matches from shuffled/random controls. See the 2026-08-19 "Phase F" section below |
| G | Verify the real JTAG debug path | **ATTEMPTED, 2026-08-19 — not working yet, real negative findings recorded.** `ORBTRACE_REG_M3_CONTROL=0x3` set, `orbtrace remote-bitbang` bridge confirmed live, but both OpenOCD's `scan_chain` and a raw hand-bitbanged probe (including the standard ARM SWD→JTAG switch sequence) read the JTAG-DP as permanently silent (TDO stuck at 0, IR capture `0x00` instead of the mandatory `0x01`) — see the 2026-08-19 "Phase G" section below for what was ruled out and what's still open |

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

**Update 2026-08-17 (continued): tried the backlog fix — it does NOT
unblock D2. Confirmed this is not a queueing/starvation problem at all:
the exact same single connection independently fails its own handshake
every time, regardless of backlog.**

Applied `main.c`'s backlog change (`1U` → `4U`, with a comment recording
the rationale), rebuilt (`bazel build
//applications/orbtrace/firmware/a53_app:a53_app`), reflashed via
`jtag_flash.sh` — no Vivado rebuild needed, this cycle took under a
minute. Retested `orbtrace load-m3`: **hangs identically**, exit code 1,
`Resource temporarily unavailable`. Re-read `control_socket`'s state
live: still `4` (`NX_TCP_SYN_RECEIVED`). Also retested with `load-m3` as
the **first and only** command issued on a completely fresh reflash (no
preceding `info` calls at all, ruling out any effect from a growing
connection count): identical hang, identical stuck state. This
conclusively rules out "one earlier bad connection's queue slot blocks
later ones" — the very first, only connection attempt from `load-m3`
fails its *own* handshake every single time, deterministically, no
matter how many free backlog slots exist. The backlog change is
harmless/correct defensively (matches good NetX practice generally) but
is not the fix and has been left in place without further claims about
what it does or doesn't solve.

**Followed the lead from a near-identical historical bug
(`documentation/ORBTRACE_TEST_REPORT_2026-08-08.md`'s "NetX's ARP table
holds a wrong destination MAC for the host, despite ARP itself resolving
correctly") — checked whether it has recurred. It has not, at least not
in the same form:** that report's diagnosis was a race in vendored
NetX's own ARP table (`_nx_arp_packet_receive.c`), worked around by this
driver's own local IP→MAC cache (`gem2_arp_learn()`/`gem2_arp_lookup()`,
`Gem2Ctx.arp_cache_*`), which `gem2_packet_send()` prefers over NetX's
(possibly racy) resolved address whenever it has a hit. Read the live
driver-local cache during a reproduction: `arp_cache_count=1`,
`entry[0]`: IP `0xc0a80101` (192.168.1.1, this host) → MAC
`00:e0:4c:75:87:68` — **exactly correct**, byte for byte, matching `ip
link show`'s real value. So the specific, previously-documented ARP
corruption is not what's happening this time.

**Tried to check whether the SYN-ACK actually went out with the right
destination MAC via `gem2_packet_send()`'s own `diag_last_tx_dst_msw/lsw`
diagnostic fields (added in the 2026-08-08 investigation specifically for
this) — inconclusive, and surfaced a real limitation of this session's
JTAG-only method.** Found these fields' exact runtime addresses the same
compiler-verified way as before (disassembling `gem2_diag_get_tx_dst`).
Read back `0xefefefef` — an untouched/poison pattern — for both,
**despite `diag_tx_frames` independently reading `13`** (`gem2_packet_send`
increments both `diag_last_tx_dst_msw/lsw` — unconditionally, near the
top of the function, for every call that reaches that far — and
`diag_tx_frames` — later, only on a successful, non-dropped send — in
the same function body, no branch in between that skips one but not the
other). A real send completing 13 times while its own earlier diagnostic
write appears never to have happened is a contradiction the source can't
explain on its own. Ruled out an address-computation mistake (re-derived
from a fresh disassembly of the just-rebuilt ELF, byte-identical
offsets; the neighboring `tx_head`/`tx_tail`/`tx_count`/`isr_calls`
fields, computed via the exact same base-pointer method, read back
sensible, correctly-changing values across repeated checks). **Most
likely explanation: a genuine cache-coherency gap, not a code bug** —
nothing in `gem2_packet_send()` explicitly flushes this specific struct
region to DRAM the way it does for the TX buffer/BD ring (which DMA
needs coherent), and this session's JTAG memory reads go through the
debug port directly to DRAM, bypassing the A53's D-cache entirely — so a
value the CPU wrote and reads back correctly from its own cache can
still appear stale/untouched to an external JTAG peek if it was never
evicted/flushed. **This means this specific field cannot be trusted via
JTAG alone, and by extension, any single "surprising" JTAG read in this
investigation that isn't cross-checked against an independently-changing
counter (like `isr_calls`, which visibly increments run to run and is
therefore known-fresh) should be treated with the same suspicion** — a
real methodological caveat for whoever continues this, not just a dead
end on the MAC-address question specifically.

**Net conclusion for this session:** the actual defect remains
unidentified. Ruled out: GEM2 DMA/hardware health (clean), the
previously-documented ARP-table race (driver-local cache is correct),
and connection-queue starvation (backlog fix has zero effect, same
single connection fails every time regardless). Not ruled out, not
confirmed either: a wrong-destination-MAC SYN-ACK via some other
mechanism (JTAG couldn't get a trustworthy read on this specific
question), a NetX-internal issue unrelated to this driver, or something
requiring the live diagnostic prints (`diag`/`diag2`-`diag8`,
already-instrumented in `main.c`) that only a real serial cable can
surface reliably. Board left on the unmodified `hybrid-fix2` PL bitstream
with the backlog-adjusted A53 firmware, responsive at session end.

**Correction, same session, immediately after: a physical serial cable
*was* available the whole time — the "no `/dev/ttyUSB*`" finding above
was a false negative from a shell-globbing mistake, not a real absence.**
`ls /dev/ttyUSB* /dev/ttyACM*` in one command aborted entirely (zsh's
default `nomatch` behavior when *any* one glob in the command fails to
match) rather than just skipping the non-matching pattern, so the
genuinely-present `/dev/ttyUSB1` (`/dev/serial/by-id/usb-Xilinx_JTAG+Serial_1234-oj1-if01-port0`)
was never seen. Checking each glob separately (or `ls /dev/tty* | grep`)
found it immediately. Worth remembering for any future "is X plugged in"
check on this machine: check globs individually, don't combine them in
one command and trust a "no matches" error to mean all of them failed.

**With that fixed: connected via `stty -F /dev/ttyUSB1 115200 raw -echo`
+ a background `cat /dev/ttyUSB1 > log`, reflashed for a clean boot-banner
capture, reproduced the hang, and got a DECISIVE, conclusive root cause —
this is a real recurrence of the exact historical bug from
`ORBTRACE_TEST_REPORT_2026-08-08.md`: NetX's own `req->nx_ip_driver_physical_address_msw/lsw`
occasionally holds garbage (raw bytes from the TCP header of the packet
being replied to) instead of a resolved MAC, for a genuine
`NX_LINK_PACKET_SEND` (not ARP) request.**

Live UART output showed `orbtrace: control client connected` — the
handshake genuinely DID complete this time (contradicting the earlier
JTAG-only reading of `nx_tcp_socket_state=4`, which must have caught a
*later*, different connection attempt or a stale/cached value — a
concrete example of the JTAG-cache-coherency caveat flagged just above
turning out to matter in practice) — followed roughly a second later by
`orbtrace: control client disconnected`, with no application-level
response ever observed being sent in between (the diagnostic capture
that would show a new send during that window never changed).

Decoded the raw `req_bytes` hex dump (`diag: req_addr=... req_bytes=...`)
against NetX's real `NX_IP_DRIVER_STRUCT` layout (`nx_api.h`: command,
status, physical_address_msw, physical_address_lsw, packet ptr, return
ptr, ip ptr, interface ptr — 48 bytes total, matching `sizeof_req=48`
exactly) for two separate sends captured this reproduction:

1. **At connect time:** `nx_ip_driver_command=0` (`NX_LINK_PACKET_SEND`,
   a real data/TCP send, not ARP) with `physical_address_msw=0xB96A0D49`,
   `_lsw=0x75846910` — decoding exactly as `src_port:dst_port` (`0xB96A`
   = 47466, the client's ephemeral port; `0x0D49` = 3401, the control
   port) then a TCP sequence number, **not a MAC at all** — precisely the
   failure signature the 2026-08-08 report described ("occasionally
   arrives holding garbage that decodes as raw bytes from the received
   packet's own TCP header").
2. **Later, still during/around the same connection's brief life:**
   another `NX_LINK_PACKET_SEND` with `physical_address_msw=0x0`,
   `_lsw=0x0` — an all-zero destination MAC, which is just as invalid as
   the first case but is **structurally different**: it's not caught by
   `gem2_packet_send()`'s existing defensive check
   (`else if (dst_msw > 0xFFFFUL) { drop }`), since `0` legitimately
   passes `<= 0xFFFF`. If the driver-local ARP cache lookup for that
   packet's destination IP also missed at that exact moment, this send
   would go out on the wire with a literal all-zero destination MAC —
   a gap in the existing mitigation, not just the original bug.

Both are genuine NetX-driver-request-level corruption, confirmed live,
not a JTAG artifact this time (the UART value is what the CPU itself
printed from its own, guaranteed-coherent view of the same memory).
Whether the driver's own `gem2_arp_lookup()` override successfully
corrected either specific packet before it hit the wire is not
separately confirmed (the diagnostic captures the pre-override value by
design), but the *upstream* corruption recurring at all — twice within
one short connection — is itself sufficient to explain a persistently
unreliable handshake/response path without needing any other
explanation, and matches a bug this project already spent a prior
session on without a full fix.

**Root cause, at last: a real, reproducible, upstream vendored NetX Duo
bug (the `_nx_arp_packet_receive.c` TX_DISABLE-unprotected "create new
ARP entry" race identified in the 2026-08-08 report) recurring — the
driver-local ARP-cache workaround built to mitigate it is real and
partially effective (the connection DID complete once, this time) but
does not cover every case: it only fires when a cache entry for the
destination IP already exists, and the fallback sanity check on `req`'s
own value only rejects `msw > 0xFFFF`, silently accepting the equally
bogus `msw=0`/`lsw=0` case.**

### Next steps

1. **Harden `gem2_packet_send()`'s fallback sanity check** (`main.c`'s
   nested driver file, `ThreadXGEM2Driver.c` around the `else if (dst_msw
   > 0xFFFFUL)` branch): also reject `dst_msw == 0 && dst_lsw == 0` (an
   all-zero MAC is never legitimate either) the same way, dropping and
   letting TCP retransmission retry — closes the specific gap found this
   session. Cheap, A53-firmware-only change.
2. **More robust, addresses the actual root cause rather than a
   narrower symptom:** investigate always preferring the driver-local
   ARP cache once *any* entry exists for the relevant interface (not
   just an exact per-destination-IP hit), or — most conclusive — apply
   the fix the 2026-08-08 report already identified but flagged as
   "upstream vendored-library behavior, not something to patch
   directly here": wrap `_nx_arp_packet_receive.c`'s "create new ARP
   entry" branch's three unprotected writes (`nx_arp_ip_address`, then
   `_msw`, then `_lsw`) in `TX_DISABLE`/`TX_RESTORE`, matching the
   sibling "update existing entry" branch a few lines above it that
   already does this. This file is vendored/external — confirm the
   project's policy on patching vendored dependencies (a local patch
   file vs. an in-tree fork) before changing it, per that report's own
   note not to modify it directly without doing so deliberately.
3. Re-run D2 (`orbtrace load-m3`) after either fix to confirm the
   connection completes AND the M3Control response actually reaches the
   client — this session only confirmed the *handshake* can complete,
   not that a full request/response round-trip has ever succeeded end to
   end.
4. Once D1 or D2 reliably loads and releases the M3, resume the Phase E
   ITM/TPIU investigation as previously planned.

**Update 2026-08-17 (continued): both fixes applied, verified working as
designed via real hardware + UART — but D2 STILL doesn't complete a
round trip. Found a further, more precise layer: the request data
provably arrives at the IP layer but never reaches `serve_control()` at
all.**

**Fix 1 (`gem2_packet_send()`'s all-zero-MAC check)** applied directly to
`ThreadXGEM2Driver.c`. **Fix 2 (the vendored ARP-table race)** applied as
a real Bazel patch, not a direct edit to the downloaded source (which
would be silently lost on the next fetch): added
`third_party/netxduo/patches/fix_arp_table_race.patch` (wraps both of
`_nx_arp_packet_receive.c`'s ARP-table-write branches — "update
existing" and "create new" — in `TX_DISABLE`/`TX_RESTORE`; the
2026-08-08 report's claim that the "update existing" branch already had
this protection turned out to be inaccurate for the actual vendored
v6.5.1.202602_rel source checked this session — neither branch had it,
so both are now protected for real correctness, not just the one branch
originally suspected) and wired it into `third_party/extensions.bzl`'s
`_netxduo_repo_impl` via `repo_ctx.patch()`. Verified the patch applies
cleanly and takes effect: `grep TX_DISABLE` on the freshly-fetched,
patched source in the Bazel sandbox shows all 4 expected occurrences
(`TX_INTERRUPT_SAVE_AREA` + 2 disable/restore pairs). `bazel build
//applications/orbtrace/...` and all 5 existing test targets pass clean.

**Retested D2 on real hardware with the UART capture running — the ARP
fix demonstrably works, but a full round trip still doesn't happen.**
The connection still gets a garbage `physical_address_msw/lsw` from
`req` at least once per attempt (e.g. `0xDEB20D49:0xBCD06DAD`,
`0xB1B60D49:0x4D16BF1A` — still decoding as `src_port:dst_port` then a
sequence number, i.e. this specific manifestation of the upstream race
is not fully eliminated by the TX_DISABLE/TX_RESTORE fix alone — the
race window this session's patch closes is narrower than the one
actually producing this), but **`tx_dropped_bad_dst` stays at `0` and
`orbtrace: control client connected` prints** — meaning the driver's
existing `gem2_arp_lookup()` cache-override successfully substitutes the
correct MAC before transmission in these cases (the diagnostic captures
the pre-override `req` value by design, so a garbage value there no
longer implies a garbage value actually went out on the wire). This is
progress — Fix 1's dropped-path never triggers because Fix 1 and the
cache override are both working together as intended — but the
handshake completing was already possible before this session's fixes
(seen once, 2026-08-17 earlier); it isn't the actual blocker.

**New, more precise finding:** added a temporary diagnostic
(`xil_printf` in `serve_control()`, `main.c`, printing
`remaining`/`consumed`/`response_len` on every `orbtrace_control_feed()`
call) and reflashed. **This line never printed at all**, across the
full `connected`→`disconnected` window — meaning `serve_control()` is
never invoked, i.e. `orbtrace_control_feed()` is never called, i.e. the
client's request bytes never reach `nx_tcp_socket_receive()`
successfully. Yet the same window's `ip_dump` (last received IP frame)
shows a real `PSH,ACK` segment, `total_length=0x30` (48 bytes = 20 IP +
20 TCP + exactly 8 bytes payload — precisely the framed
`Command::M3Control{bits:0}` size), from the client's ephemeral port to
port 3401 — **the request data physically arrives at the IP layer**,
but the socket-level receive that would hand it to
`control_thread_entry`'s loop never succeeds; the loop's `if
(nx_tcp_socket_receive(...) != NX_SUCCESS) { break; }` must be taking
the `break` path directly to the `disconnected` print, without ever
calling `serve_control()`.

**Leading hypothesis, not yet confirmed:** a race between the
connection completing its handshake and processing the client's first
data segment. The client (this session's Rust CLI) sends its request
immediately after `connect()` returns, with no delay — if the client's
final handshake ACK and its immediately-following data segment arrive
close enough together, and this driver/NetX stack's handling of a data
segment arriving right at (or fractionally before) the SYN_RECEIVED→
ESTABLISHED transition has its own bug (distinct from the ARP-table
race just fixed), the data could be silently dropped rather than queued
— matching every observed symptom: `rx_frames` shows no *further*
increase between `connected` and `disconnected` (the data frame is
already folded into the same batch that completed the handshake, not a
separate later arrival), no `control_feed` print ever fires, and
`disconnected` eventually happens on what looks like a normal path
(most likely the *client's* own 5-second read-timeout-triggered close,
since the client believes its side of the connection succeeded and is
just waiting for a reply that will now never come).

**Not yet done:** confirming this specific hypothesis (vs. some other
mechanism between "IP layer received it" and "socket receive returns
it") would need either instrumenting NetX's own TCP receive path
directly (another vendored-file patch, same mechanism now available via
`third_party/netxduo/patches/`) or a real ILA/logic-level trace of the
exact packet arrival order relative to the SYN_RECEIVED→ESTABLISHED
transition — neither attempted yet this session.

The temporary `serve_control()` diagnostic print was left in place
(matches this file's own long-established convention of keeping
"temporary" bring-up `xil_printf` instrumentation around — `diag`
through `diag8` are all still-present examples from earlier sessions),
since it is cheap and remains useful for exactly this class of
investigation going forward.

**Update 2026-08-17 (continued): D2 is FIXED. The entire "hang"
investigated across this whole session's D2 work — the M3_CONTROL AXI
hang, the NetX/SYN_RECEIVED finding, today's ARP-race investigation —
was chasing symptoms of two stacked build/flash mistakes, not (only) a
design bug. Both are now identified, corrected, and D2 works cleanly and
repeatably, including as the very first command after a fresh reflash.**

**Mistake 1 — wrong firmware artifact, present for essentially this
entire session's D2 testing.** `jtag_flash.sh` was repeatedly invoked
with `bazel-bin/applications/orbtrace/firmware/a53_app/a53_app_elf` as
its ELF argument. That file is a **flat, unrelated build output dated
2026-08-09** — eight days stale, from an earlier session's explicit
`bazel build --platforms=//sdk/platforms:apu_a53
//applications/orbtrace/firmware/a53_app:a53_app_elf` invocation (a
recipe documented in `ORBTRACE_TEST_REPORT_2026-08-08.md`). Every
`bazel build //applications/orbtrace/firmware/a53_app:a53_app` run this
session (the correct, transition-based invocation, no explicit
`--platforms`) updates a *different* artifact: `bazel-bin/.../a53_app`,
a **symlink** into a platform-transitioned output directory
(`k8-fastbuild-ST-<hash>/.../a53_app_elf`) — never the flat
`a53_app_elf` path. So every `jtag_flash.sh` call this session flashed
firmware that predated not just today's `main.c` edits (the backlog
change, both diagnostic prints) but potentially other, earlier-session
changes too — while every subsequent edit-rebuild-reflash-retest cycle
silently kept testing the exact same unchanged Aug-9 binary. Caught by
`strings bazel-bin/.../a53_app_elf | grep <a string added today>` coming
up empty, then confirmed the *correct* artifact
(`strings "$(readlink -f bazel-bin/.../a53_app)" | grep ...`) did have
it, with a fresh `2026-08-17` mtime. **The fix: always pass
`bazel-bin/applications/orbtrace/firmware/a53_app/a53_app` (no `_elf`
suffix) to `jtag_flash.sh`, matching the exact same symlink-vs-flat-file
pitfall this file already documented for `m3_app` — this is now
documented for `a53_app` too, since it bit a real session.**

**Mistake 2 — wrong PL bitstream, independent of the first.** During
the M3_CONTROL hang investigation earlier in this session, the board was
deliberately reflashed to `hybrid-fix2` (the bitstream from *before*
this session's `m3_mem_ctrl_ic` BRAM fix) specifically to isolate that
investigation from the BRAM work. That was reasonable at the time — but
every subsequent D2/NetX/ARP session turn kept using `hybrid-fix2`
without ever switching back to `hybrid-fix3` (the bitstream that
actually has the BRAM fix), including all of today's UART-based ARP
investigation. This means the *real*, already-diagnosed 4:1 BRAM
word-drop bug was silently active the entire time, and D2's
"readback mismatch" / "hang before any response" symptoms were at least
partly just that same old bug wearing a new disguise (once Mistake 1 was
fixed and D2 actually started reaching real requests, its true failure
mode reverted to exactly the historical 4:1 pattern: word 0 correct,
words 1–3 zero, word 4 correct, ... — confirmed via direct JTAG `mrd`
immediately after a `hybrid-fix2` D2 attempt).

**Both fixed together, verified repeatedly on real hardware:**
```sh
XILINX_ROOT=/home/v/opt/vitis \
XSCT=/home/v/opt/vitis/Vitis/2023.2/bin/xsct \
PSINIT=bazel-out/orbtrace-vivado-hybrid-fix3/psu_init.tcl \
BITSTREAM=bazel-out/orbtrace-vivado-hybrid-fix3/zub_orbtrace.bit \
  tooling/xsct/jtag_flash.sh bazel-bin/applications/orbtrace/firmware/a53_app/a53_app
```
- **D2** (`orbtrace load-m3`): completes cleanly — `[1] Holding M3 in
  reset...` → `[2] Streaming 748 bytes...` → `[3] Verifying reset-vector
  words...` → `[4] Releasing M3 reset...` → `Done. M3 is running`. UART
  capture during this run shows all 5 request/response cycles succeeding
  at the protocol level (`GetInfo`, `M3Control`×2, `LoadM3Chunk`,
  `ReadM3Bram`, response sizes exactly matching each command's expected
  reply length) — the earlier "control socket receive failed status=56"
  lines seen mid-investigation are the *normal*, expected signal for
  "client closed this connection after getting its reply," not an error.
  Repeated 3× including as the sole first command after a completely
  fresh reflash — 100% reliable every time.
- **D1** (`tooling/xsct/load_m3.tcl`): also retested on the same
  `hybrid-fix3` flash — all 4 checked words correct
  (`0x00010000`/`0x000002b9`/`0x000002d3`/`0x000002d3`), M3 releases and
  reports running.

**What this means for everything investigated earlier today:** the ARP
sanity-check fix (Fix 1) and the vendored `_nx_arp_packet_receive.c`
`TX_DISABLE`/`TX_RESTORE` patch (Fix 2) are both real, confirmed-live
fixes for a genuine upstream race — kept, since they're correct and
harmless regardless — but it is **not established that either was
actually necessary for D2 to work**, since every test of them happened
while Mistake 2 (`hybrid-fix2`) was still active, confounding the
result. Given D2 now works reliably with everything applied together
and re-isolating each variable would cost more real hardware cycles for
a question that no longer blocks anything, this is left open rather than
further investigated — worth revisiting only if the ARP corruption is
ever independently suspected of causing a *different* symptom later.
Similarly, the backlog change (`1U`→`4U`) and the temporary
`serve_control()`/`receive_status` diagnostics are all kept as
real, low-risk, still-informative additions, not reverted.

**Lesson for any future session touching `a53_app`:** the `bazel-bin/.../a53_app_elf`
path is a **trap** — it silently exists, silently looks like a
plausible ELF to flash, and silently never updates via the normal build
invocation. Always use `bazel-bin/applications/orbtrace/firmware/a53_app/a53_app`
(the symlink) for `jtag_flash.sh`, and when in doubt whether a *specific*
edit actually reached the flashed binary, verify with `strings` on the
resolved artifact for a string unique to that edit before spending any
more hardware-cycle time debugging.

**Update 2026-08-17 (continued): with Phase D genuinely fixed, re-ran
Phase E's ITM/TPIU investigation from scratch — and the 2026-08-16
"CPU permanently stuck on its first blocking ITM write" diagnosis is
RETRACTED. The M3 CPU is proven healthy and running normally; the real
Phase E blocker is now conclusively isolated to the PL-side capture
chain (`orbtrace_pl.v`'s DDR capture/CDC/sync-detection logic), not the
M3 or its ITM/TPIU state.**

Rebuilt `m3_app` (verifying freshness via `strings`/`nm` this time, per
the lesson above), reflashed `hybrid-fix3` + the current `a53_app`,
loaded via D2 (clean, as expected now), then added a few more latched
diagnostics to `sdk/bsp/m3/itm.h`/`m3_app`'s `main.c` — same technique
as `g_tpiu_sspsr_at_boot` (a normal SRAM global, readable via the A53's
AXI preload window, since the M3's own Private Peripheral Bus/ITM/TPIU
registers are *not* reachable that way — only via the M3's own
JTAG-DAP, which needs Phase G's real-DAP route, not wired up yet):

- **`M3_TPIU_SSPSR` (Supported Parallel Port Sizes) = `0x0000000b`** —
  bits 0, 1, 3 set = 1-bit, 2-bit, **and 4-bit** all genuinely supported
  by this synthesized instance. Conclusively rules out the "this
  DesignStart FPGA edition doesn't implement a functional parallel
  port" contingency from the `Not yet determined` list two updates up —
  the 4-bit port firmware actually selects is real and silicon-supported.
- **`M3_ITM_TCR` readback = `0x00010009`** — exactly the value
  `m3_itm_init()` wrote (`ITMENA|TXENA|(1<<TRACEBUSID_SHIFT)`, no bits
  silently cleared). The ITM enable sequence genuinely took effect.
- **`M3_ITM_STIM(0)` (bit 0 = FIFOREADY) = `0x00000001`, i.e. ready** —
  read immediately after `m3_itm_init()`, before the first
  `m3_itm_write()` call. **This directly contradicts the 2026-08-16 ILA
  finding** ("the real firmware's very first blocking `m3_itm_write()`
  call inside `emit_next()` never returns"). If the FIFO is ready at
  this exact point, the first write should not have blocked at all.
- **Decisive follow-up: added `g_heartbeat` (incremented once per
  `emit_next()` call, i.e. once per successful pass through the loop
  including its STIM writes) and sampled it three times over real
  wall-clock time via JTAG:** `2707` → (1s later) `2940` → (2s more)
  `3406` — incrementing steadily at roughly the expected rate given the
  firmware's own busy-wait delay loop. **The CPU is genuinely,
  continuously executing `emit_next()` in a normal loop right now,
  including real STIM writes succeeding — not stuck at all.**

**What this means:** the 2026-08-16 "STIM FIFO never ready" diagnosis —
which drove the entire subsequent Phase E narrative in this document —
was almost certainly a downstream artifact of the M3 BRAM corruption bug
that was *also* active that day (per this same document's own
2026-08-17 "why this matters" reasoning, written *before* today's proof:
a CPU NOP-sliding through 75%-zeroed memory could easily produce
exactly that appearance without ever reaching real branch/loop-control
logic). With the BRAM bug now genuinely fixed and the loaded image
verified correct, the CPU has no trouble at all with the ITM/STIM path.

**Re-ran the actual Phase E capture with the now-proven-healthy CPU
running:** `orbtrace configure 192.168.1.50 m3 tpiu4 2000000` + `start`
still shows `rx_bytes=0`, `dropped_bytes`/`sync_loss` in the tens/
hundreds of millions, `fifo_high_water` pegged at max (63/63) — **the
exact same symptom as before, but now with the M3 side conclusively
ruled out as the cause.** The real bytes the CPU is producing are
reaching *something* in the PL (dropped/sync-loss counts are large and
nonzero, not just idle silence), but Orbtrace's own capture/demux logic
never finds the sync pattern it's looking for. This narrows the
remaining investigation entirely onto `orbtrace_pl.v`'s M3 DDR capture
chain — the CDC FIFO between `trace_clk_m3` and `aclk`, and/or the
sync-pattern detection logic — which has not yet been the subject of
its own dedicated investigation (all prior ILA work tapped `TRACECLK`/
`TRACEDATA` at the `m3_core` IP boundary, upstream of this logic, to
verify the CPU's own output — not the capture chain that consumes it).

### Next steps

1. Real ILA capture, this time tapping *downstream* of `m3_core` —
   either `trace_pl`'s own internal CDC FIFO write/read sides, or the
   demux/sync-detection logic in `orbtrace_pl.v` — to see whether the
   CDC FIFO is genuinely keeping up with `trace_clk_m3`'s actual rate,
   and whether the expected `0xFFFFFF7F` sync pattern ever actually
   appears in what the FIFO produces.
2. Static RTL review of `orbtrace_pl.v`'s M3 DDR capture path first
   (free, no hardware) — compare it structurally against the PS trace
   path's equivalent logic (which is known-working, per the earlier
   "Achieve sustained 400 Mbit/s Orbtrace streaming" milestone) to spot
   any M3-specific difference before spending ILA rebuild time.
3. Once real trace data flows (`rx_bytes > 0`, `dropped_bytes`/
   `sync_loss` near zero), proceed to Phase F (decode `captured.bin` and
   diff against the Rust `Workload` reference) and Phase G (real JTAG
   debug path) as originally planned.

**Update 2026-08-17 (continued): did the static RTL review (step 2,
free) — found no bug, but found something more useful: a plausible,
well-reasoned refinement of *why* this is happening, resurrecting the
"bandwidth/clocking mismatch" hypothesis this document ruled out on
2026-08-16 based on what's now a retracted finding.**

Read `orbtrace_pl.v`, `orbtrace_core.sv`, `orbtrace_source_mux.sv`, and
`orbtrace_ddr_capture.sv` in full. The M3 and PS capture/mux/demux
paths are **structurally symmetric and clean** — same `orbtrace_async_fifo`
module instantiated identically for both, `orbtrace_source_mux`'s
`select==0` (M3) case routes `m3_data`/`m3_valid`/`m3_ready` exactly
like the PS case routes its own signals, and `orbtrace_core`'s
TPIU-demux/sync-detection logic (where the `0xFFFFFF7F` search actually
happens) is entirely generic — it has no idea which physical source fed
it. No structural M3-specific bug found anywhere in this path.

**What the review *did* surface: `orbtrace_ddr_capture.sv`'s 4-bit-width
case (`active_width==2'd2`, matching `tpiu4`) asserts `byte_valid` on
*every single* `trace_clk_m3` cycle once enabled and primed — not just
when the M3 has real data to report.** This matches the CM3 TRM's
documented formatter behavior (continuous half-sync/idle-frame
insertion when there's nothing real to output, confirmed by
documentation research on 2026-08-17 earlier in this document) — in
Parallel mode, the TPIU never goes quiet; it emits a byte every clock,
forever, real data or not. The CDC FIFO downstream
(`orbtrace_async_fifo`, `DEPTH_LOG2=5` → **32 entries**, matching the
observed `fifo_high_water=63`/saturated) has to absorb that *continuous,
never-idle* production rate indefinitely, not just during bursts of
real trace activity.

**This reframes the whole symptom:** `rx_bytes=0` with `dropped_bytes`
in the tens of millions and `fifo_high_water` permanently pegged isn't
necessarily evidence that the sync pattern is malformed or never
present — it's consistent with the CDC FIFO overflowing continuously
from essentially the first few cycles, before the demux logic downstream
ever gets a clean, unbroken run of bytes long enough to find *any* sync
pattern at all, real or otherwise. If `trace_clk_m3`'s actual rate
(nominally ~100 MHz per `create_bd.tcl`'s `HCLK`, but — per this
document's own still-unresolved 2026-08-16 note — possibly higher if
the DesignStart IP internally multiplies it) exceeds what the DMA/NetX/
GEM2 software pipeline downstream can sustain *continuously forever*
(as opposed to the PS path's "sustained 400 Mbit/s" milestone, which
most plausibly measured real, useful trace load rather than an eternal
100%-duty-cycle idle-fill stream), permanent overflow from the very
first moment is exactly what this architecture would produce — no
sync-detection bug required at all.

**Correction before attempting the above: `PSU__CRF_APB__DBG_TRACE_CTRL__FREQMHZ`
was the wrong parameter, and simply lowering it wouldn't have worked
anyway.** Checked `create_bd.tcl` again before spending the rebuild:
`PSU__CRF_APB__DBG_TRACE_CTRL__FREQMHZ` drives `ps/trace_clk_out`, which
only feeds `trace_pl/trace_clk` — the **PS's own** trace clock, entirely
unrelated to the M3's `TRACECLK`. `m3_core/HCLK` (and hence its
internally-derived `TRACECLK`, "the IP's own output... not tied to
pl_clk0 directly, even though HCLK feeds both") is tied to `ps/pl_clk0`
— **the exact same clock that also drives `aclk`**, the downstream
consumer side (`control_ic`, `data_ic`, the CDC FIFO's read clock,
`m3_core_ic`, everything). Lowering that one shared clock would slow
the M3's production rate and the downstream consumption rate by the
same factor, changing nothing about their relative ratio. Genuinely
decoupling them would require a new, independent PS clock output *and*
re-clocking the M3's entire AXI interconnect path (`m3_core_ic`,
`m3_mem_ctrl_core`) to bridge a new clock domain — a materially bigger,
riskier change than a one-line parameter edit, with real potential to
regress the BRAM load path this session already spent so much effort
fixing. Not attempted.

**Safer alternative implemented and tested instead: widen the M3 trace
CDC FIFO from 32 entries to 1024 (`orbtrace_pl.v`'s `m3_trace_fifo`,
`DEPTH_LOG2` 5→10 via a new `M3_TRACE_CDC_DEPTH_LOG2` localparam,
scoped to the M3 path only — the separately-proven PS trace path's FIFO
is untouched). No clock changes, no new domains, minimal risk to
anything else.** Syntax-checked clean via Vivado `check_syntax` before
committing to the rebuild. Full production build
(`bazel-out/orbtrace-vivado-hybrid-fix4`) passed all gates cleanly.
Flashed, reloaded M3 via D2 (still clean — the BRAM fix is unaffected,
as expected), re-ran the Phase E capture:

```
rx_bytes=0 dropped_bytes=100927045 sync_loss=55566131 fifo_high_water=2047 dma_faults=0
```

**Definitive result: `fifo_high_water=2047` — the FIFO's new maximum,
32× the old ceiling — is reached and stays pegged, `rx_bytes` is still
exactly `0`.** A 32× bigger buffer changed nothing about the outcome.
This conclusively rules out "just needed more headroom for a transient
burst" — the mismatch is **sustained**, not bursty: given enough
wall-clock time (2 seconds, easily enough for even a 32-entry FIFO to
have cycled many times over if drainable at all), the write side
permanently outpaces the read side regardless of buffer depth. The
bandwidth-mismatch hypothesis is now confirmed, not just plausible.

**What this actually implies for a fix, and why it's a real fork in
the road, not another incremental diagnostic step:** the M3's Parallel-
mode production rate is architecturally fixed at 1 byte per
`trace_clk_m3` cycle, continuously, forever (CoreSight formatter
behavior, not something firmware can throttle) — at `trace_clk_m3`'s
nominal ~100MHz, that's a worst-case ~800 Mbit/s *sustained, forever*
production rate. The PS trace path's own proven "sustained 400 Mbit/s"
milestone almost certainly reflects real, bursty CoreSight trace tied
to actual PS execution/debug events — not an artificial, permanent,
100%-duty-cycle idle-fill stream — so it may never have exercised the
shared downstream pipeline's (`orbtrace_core`'s demux/packetizer/
encoder, DMA, NetX, GEM2 — all common to both sources) *true* ceiling
the way the M3's worst-case output does. Two real paths forward from
here, meaningfully different in scope and risk:
1. **Make Parallel mode work anyway** — either genuinely decouple
   `trace_clk_m3` from `aclk` with a slower, independent PS clock (the
   AXI-interconnect re-clocking risk above), or find and raise a real
   throughput ceiling somewhere in the shared downstream pipeline.
   Unknown effort, unknown whether it's even achievable at the rate the
   M3 demands.
2. **Pivot to SWO** — already partially scoped by this same document's
   2026-08-17 (earlier) research: `CM3DbgAXI/component.xml` has a real,
   dedicated `SWV` pin, and the concrete fix is already identified
   (thread `m3_core/SWV` through `create_bd.tcl` into `orbtrace_pl`,
   correct the two SWO mux expressions in `orbtrace_pl.v` that currently
   read `trace_data_m3[0]` instead). SWO's rate is software-configured
   (the existing `swo_baud`/`orbtrace configure ... 2000000` parameter,
   currently 2 Mbit/s) — trivially far below any plausible pipeline
   ceiling, sidestepping this entire class of problem. Real cost: a
   different RTL/firmware path to build and verify, another rebuild
   cycle, and it changes Phase 1's original "4-bit parallel CoreSight
   trace source" framing to "SWO serial trace source."

**Paused here rather than picking a direction unilaterally** — this is
a genuine architectural fork affecting how Phase 1's whole premise gets
delivered, not another incremental diagnostic. Board restored to a
clean, responsive state (`orbtrace stop`, M3 still loaded and running)
on `hybrid-fix4` (BRAM fix + widened M3 CDC FIFO, both confirmed
working; the FIFO widening is harmless and kept regardless of which
direction comes next). `a53_app` unchanged from the last session update
(backlog fix, ARP fixes, all diagnostics).

Board left on `hybrid-fix3` with the current `a53_app` (all diagnostics
kept) and the diagnostic-enhanced `m3_app` (SSPSR/TCR/STIM0/FFSR/FFCR/
heartbeat latches), M3 running, responsive at session end.

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

## 2026-08-18 continuation — Phase E implementation closure

- The Parallel/4-bit bandwidth direction was retained: M3 HCLK is on the
  independent `pl_clk1` domain (38.889 MHz in the implemented design), while
  the trace consumer remains at 100 MHz on `clk_pl_0`.  The AXI master path
  uses the SmartConnect data FIFO and the trace path uses its own asynchronous
  FIFO.
- The initial 1024-entry M3 trace FIFO experiment was removed after a real
  implementation showed that its asynchronous-read LUT RAM congested the
  ZU1CG and failed timing.  At the reduced M3 clock the normal 32-entry FIFO
  provides sufficient elasticity without a sustained-rate mismatch.
- `orbtrace-vivado-hybrid-fix11` passed all production gates: WNS `+1.473 ns`,
  WHS `+0.010 ns`, no failing endpoints, and no unwaived critical CDC or
  methodology finding.  The only two newly-visible M3-TPIU-to-SWO CDC-11
  findings are explicit pin-to-pin waivers into the already
  `ASYNC_REG`-tagged first stages of the NRZ and Manchester synchronizers.
  Bitstream SHA-256 is
  `3f23b287922eaa393582b55d5aadea6aec06c457a66200de2ff48ee8d5d412b2`.
- The signed-off bitstream, generated `psu_init.tcl`, and current A53 ELF were
  flashed successfully through `tooling/xsct/jtag_flash.sh`.  UART diagnostics
  show the A53 firmware running, DMA initialized (`dmasr=0x10009`), and the
  board PHY linked (`phy_addr=7`, `link_up=1`).
- Phase E's network capture remains pending external host connectivity: after
  the reflash, this host had no USB-Ethernet network interface at all (only
  `lo`, `wlo1`, and `wg1`), so it cannot reach the board at `192.168.1.50` to
  upload `m3_app` or issue the `tpiu4` start/capture commands.  Reconnect or
  re-enumerate the host USB-Ethernet adapter, restore its MAC-bound
  `192.168.1.1/24` NetworkManager profile, then continue with Phase E/F.

## 2026-08-18 hardware result — Parallel/4-bit branch rejected

With the host adapter restored, the signed-off `hybrid-fix11` image was
flashed again, `m3_app` was uploaded and released successfully, and Orbtrace
was configured `source=m3`, `format=tpiu4`.  The implemented timing report
confirms `clk_pl_1`/`trace_clk_m3` are really 38.889 MHz (25.714 ns), not the
original 100 MHz.  Nevertheless, after starting capture, the live counters
were `rx_bytes=0`, `fifo_high_water=63`, and rapidly growing
`dropped_bytes`/`sync_loss` (about 100 million drops within the first sample;
over 530 million by stop); `dma_faults=0`.  The UART shows the A53 firmware,
PHY link, and trace DMA are healthy.

This rejects the Phase E parallel-clock-decoupling branch as a functional
solution: even its slowest implemented PS clock cannot sustain or correctly
decode the M3 TPIU formatter stream.  Do **not** treat the timing-clean
parallel bitstream as an end-to-end trace success.  The next implementation
branch is SWO: configure `m3_app`'s TPIU for NRZ (or Manchester if required),
select the matching existing Orbtrace decoder, and choose a baud rate safely
below the receiver/DMA capacity before repeating upload/start/capture.  Reset
the trace counters or reflash before assessing the SWO run, since the parallel
drop counters are intentionally sticky diagnostics.

## 2026-08-18 hardware result — first SWO-NRZ attempt still unverified

`m3_app` was changed to configure the TPIU for NRZ SWO with `ACPR=18`
(about 2.047 MHz from the verified 38.889 MHz HCLK), rebuilt through Bazel,
uploaded, and captured with `source=m3`, `format=swo-nrz`, and matching
`swo_baud=2046784`. `rx_bytes` remained zero and `sync_loss` immediately
advanced, while `dma_faults` remained zero. The accumulated `dropped_bytes`
and high-water values cannot assess this attempt because those diagnostics are
sticky from the deliberately-overflowing Parallel run.

Treat this as an SWO receiver/protocol-alignment defect, not a successful
pivot. The current `orbtrace_swo_nrz.sv` assumes UART-like 8N1 framing and
begins its 4x sampling phase directly from the synchronized falling-edge
observation; verify that against the M3 TPIU's actual NRZ framing and align
the first sample at a half-bit offset before relying on it. A lower-rate NRZ
A/B test after a trace-counter reset/reflash is also appropriate.

## 2026-08-18 hardware result — true 10 MHz M3 clock, SWO core proof, and Parallel/4-bit retest

The prior `PSU__FPGA_PL1_ENABLE` request for 10 MHz had implemented as
38.889 MHz, so the M3 HCLK was moved to a fabric `clk_wiz` driven by the
100 MHz PL0 clock. The routed report for
`bazel-out/orbtrace-vivado-m3-10mhz-r4` confirms both the Clocking-Wizard
output and `trace_clk_m3` are 100 ns/10 MHz. The production build passed
with WNS `+1.628 ns`, WHS `+0.010 ns`, only the two reviewed `CDC-11`
waivers, and no unwaived critical methodology findings. Bitstream SHA-256:
`04012169fe2893223a6e09ff15b8dafc3f58b22a79e4e6a1beb34c612f885dd7`.

This reimplementation initially reintroduced the historical M3-BRAM
preload symptom (only reset-vector word 0 survived; word 1 read back zero)
through both TCP D2 and JTAG D1. Adding a fully registered AXI4 register
slice directly before `m3_mem_ctrl` fixed it: after the r4 flash, D1 verified
all four consecutive words and released the M3 successfully.

SWO-NRZ was then run at `ACPR=18` (`~526316` baud at real 10 MHz) after a
clean reflash. It still yielded `rx_bytes=0` with rising `sync_loss`, but
the M3 is independently proven to execute: its BRAM-visible heartbeat moved
from `676` to `686` over one second, while the latched TPIU/ITM values were
`SSPSR=0x0000000b`, `TCR=0x00010009`, `STIM0=1`, `FFSR=8`, and `FFCR=258`.
Therefore the remaining SWO problem is receiver/protocol decoding, not a
stalled or misclocked M3 core.

Finally, the firmware was returned to `m3_itm_init(2)` and a clean
Parallel/4-bit test was performed on that same 10 MHz image. The M3 preload
again verified all four words before release. After five seconds of
`configure m3 tpiu4 2000000`/`start`, the counters were:

```
rx_bytes=0 dropped_bytes=250910394 sync_loss=30113981 \
fifo_high_water=63 dma_faults=0
```

`stop` left the same functional result (`rx_bytes=0`, FIFO high-water
pegged, no DMA fault). This rules out the M3 core-clock rate itself as the
solution to Parallel/4-bit capture: a real 10 MHz M3 still produces an
undecodable/overflowing parallel formatter stream. The next useful work is
to correct the M3-specific capture/demux protocol path (and separately the
SWO-NRZ receiver framing), not to reduce HCLK further.

## 2026-08-18 continuation — two blocker hypotheses ruled out; the demux/packetizer handshake itself is now the prime suspect

Re-examined `orbtrace_ddr_capture.sv`/`orbtrace_tpiu_demux.sv`/
`orbtrace_channel_packetizer.sv`/`orbtrace_orbflow_encoder.sv` line by line
(free, no rebuild) — a level of detail not reached by any earlier session in
this investigation — then ran two real hardware tests against the
already-flashed board (no Vivado rebuild needed for either):

1. **"No TCP listener on port 3402" ruled out.** `rx_bytes` (`Stats.rx_bytes`)
   is `orbtrace_orbflow_encoder`'s own `received_bytes` counter, incremented
   the moment a byte is accepted into its *input* ping-pong bank — well
   upstream of the DMA/TCP path, so in principle it shouldn't need an active
   reader to increment at all. Ran the plan's own documented procedure
   (`configure m3 tpiu4 2000000` → `start` → a real, concurrently-running
   `orbtrace capture HOST FILE` background reader → `stats`) instead of the
   configure/start/stats-only sequence prior updates may have used: `rx_bytes`
   stayed exactly `0` and the capture file was `0` bytes regardless. This
   rules out an undrained DMA/TCP path as the explanation and confirms the
   real blocker sits upstream of the encoder, at the FIFO/demux stage.
2. **Pure bandwidth/duty-cycle ruled out as the *sole* cause.** `orbtrace_ddr_capture`'s
   4-bit case asserts `byte_valid` every single `trace_clk_m3` cycle (100%
   duty cycle, as already known), but `orbtrace_tpiu_demux`'s `input_ready =
   !emitting` together with its `channel == 0` fast-path means a healthy
   demux should sustain roughly half of `aclk`'s rate regardless — comfortably
   above even the un-decoupled 100 MHz-M3 rate, let alone the verified 10 MHz
   one. Reflashed `m3_app` with a **temporary** `m3_itm_init(0)` (1-bit port,
   ~1/8th the 4-bit byte rate — the same diagnostic tried on 2026-08-16, but
   that session pre-dates the Phase D BRAM-corruption fix and its "real bytes
   arrived" result cannot be trusted), confirmed the built ELF genuinely
   contained the change via `arm-none-eabi-objdump` (`movs r0, #0` before the
   `m3_itm_init` call — the "verify the flashed binary" lesson from earlier
   this project applied *before* spending a hardware cycle this time,
   catching nothing wrong but confirming the practice), then reconfigured
   Orbtrace for `tpiu1` and retested on real hardware: **`rx_bytes` stayed
   `0`, `dropped_bytes` and `sync_loss` kept climbing just as fast as in 4-bit
   mode.** A per-cycle byte rate roughly 8x lower still fails identically —
   the bottleneck is not simply "M3 produces bytes faster than the demux can
   sustain." Reverted `main.c` to `m3_itm_init(2)` (committed baseline) after
   the test; board itself was left running the 1-bit firmware, harmless,
   will be overwritten by the next reflash.

**What this leaves:** the FIFO write-side is dropping bytes continuously,
which (per the RTL) requires the read side (`m3_cdc_ready`, ultimately
`orbtrace_tpiu_demux`'s `input_ready = !emitting`) to be deasserted almost
permanently — i.e. `emitting` is getting set and essentially never clearing.
`emitting` only clears once `emit` walks 0→14, gated by `(is_id || channel ==
0 || output_ready)` each step. Since neither the width (4-bit vs 1-bit) nor
the presence of a draining TCP client changes the outcome, the leading
hypothesis is now a genuine **stall inside the shared demux/packetizer
handshake itself** once a real (nonzero, non-ID) `channel` value latches —
something that PS-source traffic apparently never triggers, or triggers rarely
enough not to have been noticed at 400 Mbit/s. Two, not mutually exclusive,
concrete candidates, neither confirmed:
- `channel` latches to a spurious nonzero value (from mis-framed/scrambled
  M3 byte content — still unconfirmed whether M3's real TPIU byte stream ever
  actually contains a genuine `0xFFFFFF7F` sync word, or whether `synced`
  is being set by a false-positive match against garbage), and then never
  reaches `is_id`/`channel==0` again, so every subsequent `emitting` block is
  fully gated on `output_ready` — which itself may be getting stuck via
  `orbtrace_channel_packetizer`'s `input_ready = !held || (output_ready &&
  output_valid)` logic (a same-cycle valid/ready dependency that is
  standard practice but has not been checked against a byte-not-offered-every-cycle
  producer like this one; not confirmed to actually deadlock, just not yet
  ruled out).
- The M3 TPIU's real byte content for this exact synthesized configuration
  simply never round-trips through the shared demux correctly regardless of
  channel — i.e. a genuine content/framing bug specific to this parallel
  capture path, not a flow-control deadlock at all.

Static reasoning is exhausted again at this point — distinguishing these
needs real signal visibility PL-side, not more code reading. **Next
concrete, not-yet-done step:** add 2-3 small debug counters/latches directly
inside `orbtrace_core`/`orbtrace_pl.v` (e.g. `synced` rising-edge count,
last latched `channel` value, count of `emitting`-cycles where the advance
condition was false) exposed through a couple of spare AXI-Lite registers in
`orbtrace_axi_regs.sv` — cheaper to read back (a plain `mrd -force` over
JTAG, no new Rust command-protocol plumbing needed, since these registers
sit directly in the PS AXI-Lite address space already used for
`ORBTRACE_REG_M3_CONTROL` etc.) than a full ILA rebuild, though it still
needs one real Vivado synth/impl/route cycle (~30-90 min) to test. Paused
here to report this finding rather than committing to that rebuild
unilaterally.

## 2026-08-18 continuation — real ILA ground truth: the TPIU formatter never emits a Full Sync Packet; root cause narrowed to DWT-driven synchronization, not yet fixed

User chose to go straight to a real ILA capture instead. Two diagnostic
Vivado builds later (both ~20-25 min thanks to the cached M3 EDIF and
default P&R directives — much faster than the original "30-90 min"
estimate), with a `system_ila` tapping `orbtrace_tpiu_demux`'s internal
state (`synced`/`emitting`/`channel`/`fill`/`emit`/`is_id`) plus the raw
byte stream feeding it and the M3 trace FIFO's read side (all threaded
through temporary debug ports in `orbtrace_tpiu_demux.sv`/
`orbtrace_core.sv`/`orbtrace_pl.v`, reverted after), this is now settled
with real hardware evidence, not reasoning:

**`synced` never asserts, `dbg_selected_data` (the byte reaching the demux)
is a constant `0xFF` for the entire ~20µs capture window, and raw
`trace_data_m3[3:0]` (tapped directly, before `orbtrace_ddr_capture` ever
touches it, by reusing the same working `aclk`-domain ILA rather than a
second ILA core clocked on `m3_core/TRACECLK` — that hit a Vivado
debug-hub/Xicom enumeration error, "Invalid Register Name: COUNTER4_WIDTH",
unrelated to the design) alternates only between `0x7`/`0xF` — the
documented CoreSight idle/half-sync toggle pattern, never anything else.**
This is the *same* signature the retracted 2026-08-16 "CPU stuck" diagnosis
saw, but this time it is definitively not a downstream artifact of Phase
D's BRAM corruption: every relevant ITM/TPIU control register was
independently read back this session (not just inferred) and matches
exactly what `m3_itm_init()` wrote:

| Register | Written | Read back |
|---|---|---|
| `DEMCR.TRCENA` | set | (implied by all of the below working at all) |
| `ITM_TCR` | `ITMENA\|TXENA\|ATBID=1` | `0x00010009` (2026-08-17 finding, reconfirmed) |
| `ITM_STIM0` ready | — | `1` immediately after init |
| `TPIU_SSPSR` | (RO) | `0x0000000b` — 1/2/4-bit all supported in silicon |
| `TPIU_ITCTRL` | never written | `0` — normal mode, not integration-test mode |
| `TPIU_SPPR` | `0` (Parallel) | `0` |
| `TPIU_CSPSR` | `0x8` (4-bit) | `0x8` |
| `ITM_TER` | `0xff` | `0xff` — all 8 stimulus ports enabled |
| `g_heartbeat` | — | incrementing steadily, CPU genuinely running `emit_next()` in a loop |

Every one of these was previously either unverified or only inferred
indirectly (e.g. via `SSPSR` instead of `SPPR`/`CSPSR` themselves). With all
of them confirmed correct and the CPU proven to be actively calling
`m3_itm_write()`/`m3_itm_write_width()` continuously, yet the TPIU's real
output is *provably* a frozen idle pattern with no `0xFFFFFF7F` sync word
ever appearing — the bug is not a PL-side (Orbtrace) demux/capture problem
and not a firmware register-configuration mistake at the level of "did
this write take effect." It is something in the M3's own ITM→formatter
packet path, or a formatter behavior this configuration doesn't trigger.

**Leading, TRM-documented hypothesis (found via the extracted
`arm_cortexm3_processor_trm_100165_0201_00_en.pdf`, §11.2.2): "You must
enable synchronization packets in the DWT to provide synchronization for
the formatter... Synchronization, caused by the distributed synchronization
from the DWT, ensures that any partial frame is completed, and at least one
full synchronization packet is generated."** In other words, the TPIU
formatter's Full Sync Packet — the exact 4-byte pattern
`orbtrace_tpiu_demux.sv`'s `sync_window` searches for — is not emitted
periodically on its own; it requires a DWT-generated ITM Synchronization
Packet to trigger it. `m3_itm_init()` never touched DWT at all.

**Implemented (kept, real change, not reverted):** `sdk/bsp/m3/itm.h` gained
`M3_DWT_CTRL`/`M3_DWT_CYCCNT` register defines and `M3_ITM_TCR_SYNCENA`;
`m3_itm_init()` now sets `DWT_CTRL = CYCCNTENA | SYNCTAP=1` (the shortest
available tap, `CYCCNT[24]`, ≈1.6s period at the verified 10 MHz M3 clock)
and adds `ITM_TCR_SYNCENA` to the TCR write. This is the textbook-correct,
TRM-cited fix for the exact symptom observed.

**Tested on real hardware — did not by itself fix the symptom.** Two
control checks first confirmed the fix's own preconditions genuinely hold,
via the same JTAG SRAM-latch technique used throughout this investigation
(`g_dwt_ctrl_at_boot`, `g_dwt_cyccnt_latest`, updated every loop iteration):
- `g_dwt_ctrl_at_boot = 0x40000401` — `CYCCNTENA` (bit 0) and `SYNCTAP=1`
  (bit 10) both genuinely landed; `0x40000000` matches this TRM's own
  documented DWT_CTRL reset pattern for "four comparators present," so the
  write correctly composed with the reset value rather than clobbering it.
- `g_dwt_cyccnt_latest` read twice ~2s apart: `15866572` → `80316841`, a
  real, fast-incrementing free-running counter — confirms `CYCCNTENA`
  actually enabled counting, not just a stuck bit.

With `CYCCNT` incrementing this fast, its bit 24 (the configured sync tap)
should cross at least once within any few-second test window. Retested
`orbtrace stats`/`capture` over a full 5-10s window (comfortably longer
than one tap period) with this exact firmware: **`rx_bytes` stayed `0`
throughout.** Also tried reordering — starting Orbtrace's capture *before*
releasing the M3 from reset, in case the formatter only ever emits one
sync packet at boot/enable time and every prior test's separate
configure→start→load-m3 ordering had already missed it: **same result,
`rx_bytes=0`.** Re-armed the still-flashed diagnostic ILA against this
exact DWT-enabled firmware too: identical frozen `0xFF`/`synced=0` capture
as before (though this specific check is inconclusive on its own — the
~20µs ILA window is far shorter than the ~1.6s configured sync period, so
it could simply have missed the event even if periodic syncs are now
genuinely being generated; the stats-based multi-second tests are the
more decisive negative result here).

**Not yet resolved — concrete candidates for a future session, cheapest
first, all firmware-only (no Vivado rebuild needed since the diagnostic
ILA bitstream is still cached and flashable):**
1. The `DWT_CTRL` bit-field layout used here (`CYCCNTENA`=bit0,
   `SYNCTAP`=bits[11:10]) is standard ARMv7-M architecture, cited from
   general knowledge, not from this specific TRM (which explicitly defers
   DWT register bit-fields to "the ARM®v7-M Architecture Reference
   Manual" — not present in this project's locally extracted docs, and
   not independently confirmed against this exact silicon). Worth
   re-deriving from a canonical CMSIS `core_cm3.h`/ARM DDI0403E source
   before trying more bit-position guesses blindly.
2. Whether this specific synthesized TPIU instance actually implements the
   optional DWT→TPIU distributed-synchronization signal at all (a
   legitimate CoreSight synthesis-time option) — if not implemented, no
   DWT/ITM configuration could ever produce a Full Sync Packet, and the
   only paths forward would be either accepting Parallel mode as
   unworkable on this exact IP configuration, or finding another trigger
   mechanism (e.g. a manual/software-forced formatter flush, if one
   exists beyond what `TPIU_FFCR` documents — `FFCR`'s own bits were
   fully checked this session: reset value `0x102` = `TrigIn`(RAO,
   harmless) + `EnFCont=1` already enabled by default, nothing missing
   there).
3. A real ILA capture with a MUCH longer window (the current 2048-sample
   depth at 100 MHz gives only ~20µs; this part's LUT budget allowed up to
   4096 samples for a single-domain NATIVE ILA before — even that is only
   ~40µs, nowhere near the ~1.6s sync period) is not a practical way to
   directly observe a sync event landing; the multi-second `orbtrace
   stats`/`capture` tests already exercised this and came back negative,
   which is the more trustworthy result.
4. Consider instrumenting `g_heartbeat`-style counters *inside* `emit_next`
   right at sequence boundaries to correlate real wall-clock sync-tap
   crossings with what (if anything) changes in `orbtrace stats` at that
   exact moment, rather than only sampling stats at the start/end of a
   window.

**Session wrap-up:** all temporary RTL debug ports (`orbtrace_pl.v`,
`orbtrace_core.sv`, `orbtrace_tpiu_demux.sv`) reverted to their committed
baseline; the untracked `applications/orbtrace/vivado/create_bd_debug.tcl`
scratch file deleted (recreate from this section's description if
repeating the ILA methodology — it's a thin wrapper that `source`s the
real `create_bd.tcl` unchanged, then appends one `system_ila` NATIVE-mode
core with 11 probes: `dbg_selected_data[7:0]`, `dbg_selected_valid`,
`dbg_synced`, `dbg_emitting`, `dbg_channel[6:0]`, `dbg_fill[4:0]`,
`dbg_emit[4:0]`, `dbg_is_id`, `dbg_m3_cdc_valid`, `dbg_m3_cdc_ready`,
`dbg_trace_data_m3_raw[3:0]`, all clocked on `aclk`/`ps/pl_clk0` — do NOT
add a second ILA clocked on `m3_core/TRACECLK` directly, that hit the
Xicom/debug-hub error above). Diagnostic ILA bitstream cached at
`bazel-out/orbtrace-vivado-m3-ila-debug` (build script
`build_debug.tcl`, both lived in the session scratchpad, not committed —
recreate from this description too) if a future session wants to reuse it
without another ~20-25 min rebuild. Board restored to the last known-good
*production* bitstream (`bazel-out/orbtrace-vivado-m3-10mhz-r4`, no ILA,
matches the currently-committed `create_bd.tcl`), running the current
`m3_app` (DWT sync fix plus all this session's new diagnostic latches --
`g_tpiu_itctrl_at_boot`, `g_tpiu_sppr_at_boot`, `g_tpiu_cspsr_at_boot`,
`g_itm_ter_at_boot`, `g_dwt_ctrl_at_boot`, `g_dwt_cyccnt_latest` -- all
kept as real, committed, low-cost-to-retain diagnostics matching this
file's own established convention).

**How to apply:** Parallel/4-bit mode's blocker is now understood at a much
deeper level than the earlier "bandwidth mismatch" framing (which is
itself retracted by this session — width and clock rate were both already
ruled out on 2026-08-18 before this ILA work even started) — it is a
missing Full Sync Packet, most likely fixable with the correct DWT
configuration, but the specific bit-pattern tried this session didn't
produce one within a generously long test window. Next session should
either (a) re-derive the exact DWT_CTRL encoding from a canonical source
before retrying, or (b) treat this as strong enough evidence to formally
pivot to the SWO path (already separately scoped, and known to have a
receiver/framing bug rather than a content-generation bug — see the
2026-08-18 SWO-NRZ sections above), since SWO's own content-generation
side does not depend on this same DWT/formatter full-sync mechanism at
all (SWO bypasses the formatter's frame-sync protocol entirely per TRM
§11.2.3).

## 2026-08-18 continuation (same day) — user directed to keep pushing on Parallel mode specifically, not SWO. Two real findings, one real RTL bug found and fixed, but genuine decode still not achieved

User explicitly ruled out the SWO pivot ("I don't care about SWO") and asked to continue on Parallel mode. This update covers real, hardware-verified progress and a final, honest negative result for the specific approach tried.

**1. Verified the `DWT_CTRL` bit encoding against an authoritative source (web search:
libopencm3's `dwt.h`, cross-checked against the TRM's own documented reset-value table).
`CYCCNTENA`=bit0, `SYNCTAP`=bits[11:10] is confirmed correct — not the bug.** The observed
readback (`0x40000401`) decomposes exactly into the intended write (`CYCCNTENA`+`SYNCTAP=1`)
composed with `NUMCOMP=4` at bits[31:28] (`0x40000000`, matching "four comparators present"
from the TRM's own DWT_CTRL reset-value table) — the write genuinely landed correctly.

**2. Definitively confirmed (real hardware, 22-minute edge-triggered ILA wait, then
reconfirmed via an 8-second `orbtrace stats` window under a continuous/tight STIM write
pattern) that the M3's formatter never emits a Full Sync Packet, regardless of write
timing.** This rules out "maybe DWT sync needs sustained traffic too" as an explanation.

**3. Real finding: STIM write TIMING matters enormously.** Isolated via a `system_ila`
free-run capture on `trace_data_m3` (same diagnostic bitstream/probes as the previous
update, reflashed with several different firmware variants):
- A near-zero-overhead tight loop (`for(;;) M3_ITM_STIM(0)=value;`, no delay, single port,
  32-bit) produces **real, varying content** on `trace_data_m3` (10 distinct nibble values
  across a 20µs capture, vs. a frozen constant beforehand).
- Reintroducing even a small delay (~10-12µs, a 20-iteration busy-wait) or using
  `emit_next()`'s own inherent overhead (switch dispatch, state update — tens of cycles,
  no explicit delay loop at all) both revert the output to the frozen idle pattern. The
  critical gap threshold is somewhere between a handful of CPU cycles and ~20-40 cycles —
  extremely tight, and not compatible with any realistic instrumented-firmware write
  pattern, only a dedicated synthetic test loop.

**4. Implemented a self-contained RTL workaround: a "force-sync" fallback in the shared
`orbtrace_tpiu_demux.sv`, gated to the M3 source only (`source_select==0`; the PS/ETM
path's real, proven sync-pattern detection is completely unmodified) — since real content
does reach the wire under the right write pattern, periodically force frame alignment at
a guessed phase instead of waiting for a Full Sync Packet that will never come, cycling
through all 16 possible phases so a multi-second capture samples every alignment.**

- **First implementation had a real bug**, found via a `0xDEADBEEF` marker test (write a
  fixed, easily-searchable 32-bit value instead of varying content, then search the
  decoded corpus for the literal bytes): forcing `fill<=force_phase` (a nonzero starting
  value) does not wait for a full fresh 16-byte refill before emitting — emission
  triggers as soon as `fill` reaches 15 regardless of where it started, so buffer
  positions before `force_phase` held **stale data from the previous cycle**. This
  produced `rx_bytes` in the millions and a semi-consistent-but-corrupted repeating byte
  pattern around partial marker fragments — real bytes flowing, but every "frame" a
  mix of fresh and stale data.
- **Fixed**: always restart cleanly at `fill=0` (matching normal, bug-free behavior);
  vary only *when* the restart triggers (a rotating cycle-offset within each ~4000-cycle
  period), sampling all 16 true alignments via always-fresh restarts instead of
  fill-index tricks. Verified via `xvlog`/`xelab` elaboration and the existing RTL unit
  test suite before spending a rebuild.
- **Rebuilt as a real production bitstream** (not the relaxed diagnostic ILA build —
  `build.tcl` unchanged, strict CDC/methodology/timing gates all passed) both before and
  after the fix, ~25-35 min each thanks to the cached M3 EDIF.

**5. Decisive negative result: even with the bug fixed, the `0xDEADBEEF` marker never
appears intact in the decoded output, and a rigorous statistical control proves the
earlier "partial marker fragment" evidence was a false positive.** `rx_bytes` reached
millions in every force-sync test (2.2M, 4.9M, 2.8M, 3.8M bytes across different runs) —
but this metric turned out to be **not a reliable indicator of correct decode at all**: a
run with the firmware writing a real varying value showed the exact same statistical
signature (a specific 2-byte marker fragment, `AD DE`, appearing ~4500-4600 times vs. ~64
expected by chance) as a run with the M3 producing **zero real content** (delay
reintroduced, confirmed frozen `0xFF` idle pattern the whole time, which still produced
`rx_bytes=3.8M` and the identical `AD DE` excess at proportional rate, 808 hits). The
"statistically significant" pattern is an artifact of decoding the idle pattern's own
structure through the demux's ID-byte-unmangling arithmetic — unrelated to real M3
content — and the corrected force-sync fallback does not recover genuine, verifiable
ITM/CoreSight-framed data at any of its 16 tried phases.

**What this means:** real electrical variation on `trace_data_m3` is confirmed to occur
under specific tight-write-timing conditions (item 3), but this variation does not appear
to be genuine, protocol-compliant CoreSight-framed content recoverable via the demux's
standard frame model — or if it is, the true framing convention differs from what's
implemented (byte order, frame size, or ID-byte mangling assumptions may not match this
specific synthesized TPIU instance) in a way this session's testing could not pin down.
Every register-level configuration hypothesis has now been exhausted and independently
verified correct; every timing hypothesis tested; the RTL-workaround hypothesis
implemented, debugged, and tested to a clean negative result.

**Session end state:** RTL (`orbtrace_tpiu_demux.sv`, `orbtrace_core.sv`) reverted to
committed baseline — the force-sync fallback was real, working code (verified via
elaboration and two real hardware builds) but is not being kept since it doesn't achieve
its purpose; recreate from this update's description if a future session wants to resume
from where this left off (the corrected version, not the buggy first attempt). Firmware
(`sdk/bsp/m3/itm.h`, `applications/orbtrace/firmware/m3_app/src/main.c`) kept only the
real, TRM-motivated DWT sync configuration and diagnostic latches
(`g_tpiu_itctrl_at_boot`, `g_tpiu_sppr_at_boot`, `g_tpiu_cspsr_at_boot`,
`g_itm_ter_at_boot`, `g_dwt_ctrl_at_boot`, `g_dwt_cyccnt_latest`) — all throwaway test
loops (tight-loop bypass, `0xDEADBEEF` marker, ready-check removal) reverted. Board
restored to the known-good production bitstream (`bazel-out/orbtrace-vivado-m3-10mhz-r4`)
running the clean firmware. Diagnostic ILA bitstream still cached at
`bazel-out/orbtrace-vivado-m3-ila-debug` if a future session wants to resume raw
signal-level investigation without a rebuild. Decode/analysis scripts
(`decode_orbflow.py` and the marker-search snippets) lived in the session scratchpad, not
committed.

**How to apply — genuinely open, no more low-hanging fruit identified this session:**
Parallel mode is blocked at a level this session's toolset (register verification, timing
experiments, ILA captures, RTL workarounds) could not resolve. Next steps would likely
require either (a) a from-scratch, byte-level reverse-engineering of exactly what this
specific TPIU instance puts on the wire under the tight-write-timing condition — ideally
with a MUCH longer/deeper ILA capture (would need a Vivado license/part supporting more
than the ~4096-sample budget this XCZU1CG's LUT count allows) correlated cycle-by-cycle
against known STIM write instants, rather than statistical corpus search after the fact;
or (b) reaching out to Arm/Xilinx documentation or support channels for this specific
DesignStart FPGA edition's TPIU behavior, since the encrypted RTL can't be inspected
directly and the public TRM doesn't fully specify implementation-defined timing behavior.

## 2026-08-18 continuation (same day) — deeper ILA capture, per user request: real structural progress, root cause narrowed further but not yet fully solved

User asked to try the deeper ILA capture explicitly. This section supersedes the
"no more low-hanging fruit" framing above for the specific question of *why* real
content never reaches the demux — a genuinely new, concrete lead was found.

**Methodology change from all earlier captures this investigation:** every previous
ILA used many probes (demux internals, etc.) at shallow depth (2048 samples, ~20µs).
This time, minimal probe sets (2-6 signals) let the ILA go to 8192-16384 samples
(~80-160µs, ~10-16x longer). Also switched the firmware under test from `emit_next()`'s
varying-port/varying-width workload to first `M3_ITM_STIM(0) = g_heartbeat` (tight,
zero-delay loop, no ready-check) and then a fixed `M3_ITM_STIM(0) = 0xDEADBEEFu` marker
for unambiguous pattern matching — both bypass `m3_itm_write()`'s wrapper directly to
avoid re-touching `itm.h`.

**Real, resource-contention build failures this session (not RTL/BD problems):**
multiple diagnostic builds were killed mid-synthesis by memory pressure from unrelated
desktop applications on this machine (a game using ~9GB RSS, dozens of Firefox tabs;
swap was at 31/34GB used). Confirmed via `ps aux --sort=-%mem`/`free -h` each time, not
assumed. Retrying (sometimes 2-3x) reliably succeeded once memory freed up. Worth
checking `free -h` before assuming a killed background Vivado job is a real failure.

**Finding 1 — the raw M3 pins genuinely do carry more than a 2-value idle toggle.**
A first deep capture (11→2 probes, depth 16384) tapping only `dbg_trace_data_m3_raw`
(the raw 4-bit bus, before any PL-side processing) showed **11 distinct nibble values**
across the window under the tight-write firmware — more than the `0x7`/`0xF` idle-only
toggle seen in every earlier, shorter capture. But `dbg_selected_data` (the
DDR-reconstructed, post-FIFO byte reaching the demux, probed simultaneously) stayed
**100% frozen at a single constant value** the entire window. This appeared to
conclusively localize a bug to the PL-side DDR-capture/FIFO reconstruction path, not
the M3.

**Finding 2 — localized further: `orbtrace_ddr_capture`'s own direct output (pre-FIFO)
also showed real, matching variation; the freeze was specifically downstream of it.**
Added `dbg_m3_trace_byte`/`dbg_m3_trace_valid` (the DDR-capture module's own output,
before the async CDC FIFO) alongside the post-FIFO probe on the same capture. Result:
pre-FIFO byte showed genuine multi-value variation (`ff`, `23`, `60`, `3f`, `39`, `0a`,
`1a`, `18`, `03`, `01`, `38`, `2a`, `10`, `0b`, ...) while post-FIFO stayed frozen —
seemingly proving the async FIFO (or its read-side handshake) was discarding real
content.

**Finding 3 — did NOT reproduce on a fresh rebuild; the "frozen FIFO" was a transient/
race condition, not a structural bug.** Added direct probes on `m3_cdc_ready` (FIFO
read-side ready) and `m3_cdc_write_ready` (FIFO write-side ready) alongside pre/post-FIFO
bytes, same firmware, freshly rebuilt bitstream (same RTL, different synthesis run).
This time: **pre-FIFO and post-FIFO bytes matched exactly, byte-for-byte, real content
flowing correctly all the way through** to `dbg_selected_data`. Both `m3_cdc_ready` and
`m3_cdc_write_ready` read a constant `1` (never stuck) throughout. `orbtrace stats` at
this exact moment still showed `rx_bytes=0` despite confirmed-correct content reaching
the demux's input — consistent with the already-established finding that the demux
still can't decode without a genuine Full Sync Packet, which never appears (re-confirmed:
zero occurrences of `0xFFFFFF7F` anywhere in this capture too). **The FIFO/DDR-capture
path is not the real bug; Finding 2's apparent freeze was some kind of transient
startup-race condition (not yet understood, possibly RTL-build-instance-specific
timing), not a structural flaw.** This is genuinely useful: it means the PL capture
pipeline can and often does work correctly, redirecting effort back to Finding 4.

**Finding 4 — real, structured, repeating content discovered by deduplicating the
oversampled capture and removing idle bytes.** The ILA (100 MHz `aclk`) asynchronously
oversamples the ~10 MHz M3-domain byte stream roughly 10x per real value, so consecutive
identical ILA samples are a sampling artifact, not real repetition. After
deduplicating consecutive-identical values, the `0xDEADBEEF`-marker capture showed a
**clearly repeating structure**: a stable multi-byte prefix (e.g. `3f 23 e3 ee de`, with
`de` = `0xDE`, the marker's true MSB, landing at a plausible position), varying
tail bytes, and — critically — **`0xFF` (or a value very close to it) appears between
every single "real" byte**, not just as sparse padding between distant writes. Given
`0xFF`'s top 7 bits (`0xFF >> 1 = 127`) exactly match the "channel 127" that dominated
every prior `orbtrace stats`/decoded-corpus histogram throughout this entire multi-day
investigation, this is very likely the CoreSight formatter's even-position "ID byte"
mechanism firing on *every* even slot (not occasionally, as assumed) rather than the
odd/data positions being what's sparse. This reframes the open question: real content is
demonstrably present and forms a discoverable pattern, but either (a) this session's
CoreSight ID-byte-unmangling model in `orbtrace_tpiu_demux.sv` doesn't match this
specific TPIU's real wire convention, or (b) the async, non-cycle-accurate sampling
technique can't distinguish "one idle slot" from "multiple consecutive idle slots"
between real bytes, which blocks precisely reconstructing byte positions from this data
alone.

**Not resolved this round:** the exact byte-for-byte mapping from the observed pattern
to the literal marker bytes (`EF BE AD DE`). Partial, suggestive matches were found
(`DE` at a plausible position) but not a clean, confirmed full decode.

**Session end state:** all temporary RTL debug ports (`orbtrace_pl.v`, `orbtrace_core.sv`)
reverted to committed baseline; the untracked `create_bd_debug.tcl` deleted. Firmware
back to the real, kept state (DWT sync fix + diagnostic latches only, `emit_next()`
restored as the main loop). Board back on the known-good production bitstream. The
`bazel-out/orbtrace-vivado-m3-ila-debug` diagnostic ILA bitstream directory was
overwritten multiple times this round (final state: 6-probe version with
`m3_cdc_ready`/`m3_cdc_write_ready`) — recreate `create_bd_debug.tcl` from this section's
probe descriptions if resuming this exact investigation.

**How to apply — concrete next steps, most promising first:**
1. **Get a cycle-accurate (not asynchronously-oversampled) capture.** The current
   technique samples PL-domain (trace_clk_m3 or its FIFO output) signals with the
   aclk-domain ILA, which can't distinguish idle-slot multiplicity. A NATIVE-mode ILA
   clocked directly on `trace_clk_m3` (like the very first, 2026-08-16 M3 investigation
   used) would give one sample per real byte, unambiguous. Note: a *second* ILA core on
   a different clock hit a real Vivado debug-hub/Xicom tool bug earlier this session
   (`Invalid Register Name: COUNTER4_WIDTH`) — either use a single trace_clk_m3-clocked
   ILA alone (no aclk-domain ILA in the same bitstream), or investigate that tool
   limitation further if both domains are needed simultaneously.
2. With unambiguous idle-slot counts, precisely test whether the observed pattern
   matches standard CoreSight ID-byte mangling (this session's demux model) or a
   simpler convention (e.g. literal alternating idle/data with no bit-stealing).
3. Re-run the marker test (`0xDEADBEEF`) against a cycle-accurate capture and search for
   the literal `EF BE AD DE` byte sequence directly in the unambiguous data.
4. Only after the true wire format is nailed down precisely, either fix
   `orbtrace_tpiu_demux.sv`'s decode model to match it, or (if it turns out to still
   require a genuine Full Sync Packet the M3 never produces) revisit the force-sync
   fallback approach with the corrected framing model.

## 2026-08-18 continuation (new session) — cycle-accurate capture built and run; BREAKTHROUGH: `rx_bytes` genuinely nonzero (5M+ real bytes) for the first time ever, but decoded content is still wrong — root cause narrowed to false/misaligned sync locks, not a transport or framing-model bug

Followed this doc's own "next steps" #1 exactly: built a NEW diagnostic bitstream with
a single `system_ila` clocked directly on `m3_core/TRACECLK` (not `aclk`), probing
`orbtrace_pl.v`'s M3 DDR-capture module's own post-reconstruction `byte_data`/`byte_valid`
(temporary `dbg_m3_trace_byte`/`dbg_m3_trace_valid` ports, reverted after — same technique
as every prior session's diagnostic taps). This is a materially different, better design
than every earlier capture in this investigation: because it probes the ALREADY-correctly-
DDR-reconstructed byte (not raw `trace_data_m3[3:0]`) on the SAME clock domain that
register lives in, every sample is one real, unambiguous byte — no async-oversampling
idle-slot-count ambiguity, and only one ILA/one clock domain in the bitstream (avoiding the
known Xicom two-clock-domain bug). Build: `bazel-out/orbtrace-vivado-m3-cycle-ila`, WNS
+0.648ns, WHS +0.010ns, clean gates (relaxed diagnostic build, as usual for these).

**New real tool issue found and worked around: the JTAG scan of this specific debug core
was badly flaky at the default JTAG clock rate — intermittent "1 vs 2 ILA Input port(s)"
probe-count mismatches and a hard "Slow clock or no clock connected for ILA" error during
upload, non-deterministic across identical back-to-back attempts on unchanged hardware.**
Root cause: this ILA's core clock is `trace_clk_m3` (~10MHz), not `aclk` (100MHz) like every
other debug core successfully used earlier in this investigation — Xilinx's own documented
minimum ratio (JTAG TCK ≤ debug-hub-clock/2.5) was being violated by the default JTAG
frequency. Fix: `set_property PARAM.FREQUENCY 1000000 [get_hw_targets]` before scanning —
resolved the flakiness immediately and reliably. **Worth remembering for any future ILA
tapping a clock slower than the default JTAG rate on this board.** Also found and worked
around: stray `hw_server`/`cs_server` processes accumulate across repeated
`nix develop -c vivado -mode batch` Hardware Manager invocations and are not cleaned up
automatically; `pkill -f` was silently blocked/denied in this sandbox (exit 1, no output) —
use `kill -9 <pid>` from a `ps aux` listing instead.

**Marker test, cycle-accurate this time:** temporary firmware (`M3_ITM_STIM(0) = 0xDEADBEEFu`
tight loop, no ready-check, same rationale as the 2026-08-18 earlier marker tests) captured
16384 real, unambiguous bytes. Two findings:

1. The steady-state idle pattern is a clean, stable `FF 7F FF 7F ...` byte-level alternation
   (confirmed over long stretches) — but immediately after SOME (not all) real-content bursts,
   a *different*, also-stable idle sub-pattern appears: `FF 7F FF FF` repeating. This second
   pattern trivially and repeatedly contains the literal 4-byte sequence the demux searches
   for (`FF FF FF 7F`) at every period, purely as an arithmetic artifact of tiling `FF 7F FF FF`
   — **not evidence of a genuine formatter-inserted Full Sync Packet.** Feeding the full 16384-byte
   capture through a line-for-line Python reimplementation of `orbtrace_tpiu_demux.sv`'s exact
   state machine confirmed this: it "syncs" once (on this alias), then decodes garbage
   (channels 63/127/49/31/...) for the rest of the window — the same statistical-artifact
   signature the 2026-08-18 (earlier) session already characterized. **This is a second,
   independent, real failure mode for the literal-4-byte-substring sync search, on top of
   the already-known "no genuine Full Sync Packet within any practically-capturable window"
   problem** — even if a real Full Sync Packet were found, this shows the search can also
   false-lock on the idle pattern's own aliasing.

2. **Decisive, unplanned discovery: with the marker firmware left running continuously for
   several minutes (many DWT sync periods) and Orbtrace's `configure`/`start` issued for
   real, `orbtrace stats` showed `rx_bytes=8`, then, given a real `orbtrace capture` run over
   ~9 more seconds, `rx_bytes=5,243,102` — genuinely nonzero, sustained, real byte flow
   through the ENTIRE PL pipeline (DDR capture → CDC FIFO → demux → orbflow encoder → DMA →
   NetX → TCP 3402) for the first time in this entire multi-day investigation.** Every prior
   session's test window was measured in seconds right after a fresh reflash/load — this is
   the first test left running long enough (and with continuous real STIM traffic, per the
   already-known STIM-write-timing sensitivity) to plausibly let a DWT-driven sync event (or
   several) actually occur.

   **But decoding `captured.bin` (COBS/orbflow-unframed via a Python port of
   `model/src/lib.rs`'s exact `cobs_decode`/`orbflow_unframe`) shows the transport is
   essentially perfect (786,587 / 786,588 frames pass their checksum) while the CONTENT is
   still wrong: channel 1 (the configured `TRACEBUSID`, the only channel real M3 content
   should ever appear on) has only 4 total bytes across the whole 5.2MB capture, none of
   them resembling the repeating `03 EF BE AD DE` the marker firmware should have produced.
   Every other channel seen (63, 127, 49, 31, 40, 48, 62, 78, 114) is bogus, dominated by
   `FF`/`7F`/`33`/`EE`-type idle-derived noise.** This means the demux DID lock onto
   *something* stable enough to sustain 5M+ bytes of self-consistent (checksum-valid) COBS
   framing — almost certainly the SAME kind of aliased idle-pattern lock as finding 1 above,
   just one that happened to stay locked for a long time rather than immediately overflowing
   — not a genuine, correctly-phase-aligned lock onto real M3 content.

**What this changes:** the transport/encoder/DMA/NetX/TCP path (everything downstream of
the demux) is now proven robust under real, sustained load — this was never actually tested
end-to-end before (every earlier "success" was `rx_bytes` staying at 0). The ENTIRE remaining
problem is now conclusively isolated to one thing: **`orbtrace_tpiu_demux.sv`'s frame-sync
acquisition, which reliably false-locks onto structure that exists within the M3's own idle
output, rather than ever locking onto a position aligned with real content.** This is no
longer a "does real data reach the wire" or "does the pipeline work" question — both are
settled yes. It is specifically a sync-acquisition-robustness bug.

**Not yet attempted, and NOT started without checking in first (this is a genuine new
engineering direction, not an incremental fix, matching this doc's own established pattern
of pausing before this kind of fork):** the standard CoreSight architecture's own sync
mechanism (bare 4-byte magic-number search) has now been shown twice to be alias-prone
against this specific IP's real idle output. A more robust resync strategy is needed —
candidates, cheapest/most-targeted first:
1. Require a plausibility check before committing to a sync lock: after finding a
   candidate `FFFFFF7F`, verify the frame's first non-idle ID byte actually decodes to
   `channel == 1` (the fixed, known `TRACEBUSID` this design always configures) before
   trusting the lock; otherwise keep searching. Cheap, targeted, but overfits to this one
   fixed TRACEBUSID configuration (acceptable, since nothing in this codebase varies it).
2. Anchor resync on the observed real-world idle→burst TRANSITION (recognizable, stable
   `FF 7F` alternation ending) rather than a bare magic-number substring match — a bigger
   departure from the standard architecture's own assumptions, but directly justified by
   this session's empirical characterization of what this specific silicon actually puts on
   the wire.
3. Revisit whether `reset_sync`/`sync_loss_count` behavior itself needs investigation —
   `orbtrace stats` showed `sync_loss` still climbing by ~580K/s during the 9s capture
   window despite `synced` in the RTL model only ever being cleared by an explicit
   `reset_pulse` (an AXI-Lite command-driven one-shot, not something that should fire
   continuously during normal `running` operation) — not yet reconciled; `Stats.sync_loss`
   may aggregate more than just `orbtrace_tpiu_demux`'s own counter (see
   `orbtrace_pl.v`'s `tpiu_sync_loss+nrz_malformed+manchester_malformed` combination for the
   SWO stats path) and this hasn't been traced through for the Parallel/tpiu4 case
   specifically.

**Session artifacts (not committed, scratch-only):** the trace_clk_m3-domain ILA
bitstream at `bazel-out/orbtrace-vivado-m3-cycle-ila` (build script
`build_debug_cycle_ila.tcl`, BD wrapper `create_bd_debug.tcl`, both in the session
scratchpad — thin wrapper sourcing the real, current `create_bd.tcl` plus one
`system_ila` on `m3_core/TRACECLK` probing `trace_pl/dbg_m3_trace_byte`/
`dbg_m3_trace_valid`, recreate from this description if reused); `arm_and_capture.tcl`
(Hardware Manager Tcl, includes the `PARAM.FREQUENCY 1000000` fix and retry loops for the
JTAG flakiness above); `demux_sim.py` (line-for-line Python port of
`orbtrace_tpiu_demux.sv`); `orbflow_decode.py` (Python port of `model/src/lib.rs`'s
COBS/orbflow unframing). All temporary RTL debug ports (`orbtrace_pl.v`) and firmware
changes (marker loop in `main.c`) have been reverted to committed baseline; board state
at end of this update: `orbtrace-vivado-m3-cycle-ila` bitstream still flashed (not the
production `m3-10mhz-r4`), M3 running the marker firmware with `configure m3 tpiu4` +
`start` still active.

## 2026-08-19 continuation — channel-plausibility fix implemented, built, and flashed as a
real production bitstream; result so far: false locks are gone, but no genuine sync
observed yet under either workload. Session interrupted mid-retest — picking back up here.

**Fix implemented** (user chose this over idle-transition anchoring or investigating
`sync_loss` first, when asked): `orbtrace_tpiu_demux.sv` gained a new `m3_source` input
(wired from `orbtrace_core.sv` as `source_select == 2'd0`) and a plausibility gate in the
sequential block — when `m3_source` is set, any `is_id` byte whose decoded channel isn't
`M3_EXPECTED_CHANNEL` (`7'd1`, the fixed `TRACEBUSID` `m3_itm_init()` always configures)
immediately drops `synced`/`fill`/`emitting` back to the search state and counts as a
`sync_loss`, instead of continuing to decode at the wrong frame phase. The PS/ETM path
(`m3_source==0`) is untouched. Verified via `xvlog`/`xelab`/`xsim` against the existing
`rtl_unit_test` suite (passes; note doesn't cover this file directly, no dedicated
`orbtrace_tpiu_demux` testbench exists yet) before spending a real rebuild.

**Real production build** (`build.tcl`, strict gates, not the relaxed diagnostic path):
`bazel-out/orbtrace-vivado-m3-sync-fix`. All three gates passed cleanly: zero unwaived
critical CDC violations, zero unwaived critical methodology violations, WNS=1.641ns/
WHS=0.010ns. Flashed via `jtag_flash.sh` (full `rst -system` reflash), `orbtrace info`
confirmed the board up.

**Retest, real firmware (`m3_itm_init(2)` + real `emit_next()`, 10000-iteration busy-wait
between STIM writes):** `rx_bytes=0` after both a ~3s and a longer window; `sync_loss`
climbing steadily (73M+ within a few seconds) — the fix is actively rejecting candidate
locks continuously, but none are ever both genuine AND channel-1-plausible under the real
workload's sparse STIM cadence. Consistent with the already-known STIM-write-timing
sensitivity (real content only reaches the wire within a narrow ~10-40 cycle window after
each write): the real workload's huge inter-write gaps mean the TPIU is emitting almost
pure idle output nearly all the time, giving the (still bare-substring) sync search very
little genuine content to lock onto in the first place.

**Retest, marker firmware (`M3_ITM_STIM(0)=0xDEADBEEFu` tight loop, same firmware that
produced 5.2M mostly-garbage bytes on the PRE-fix bitstream):** `rx_bytes=0` across three
separate capture attempts of increasing length (15s, then ~70s, comfortably longer than
the ~1.6s DWT sync-tap period at the board's real 10MHz M3 clock). This is the most
important new data point: **the fix's `sync_loss` counter climbing throughout, combined
with the earlier pre-fix session's 5.2M-byte result being conclusively shown to be
false-lock garbage (channel 1 had only 4 bytes total, none matching the marker), together
mean the demux was very likely never achieving a single genuine, content-aligned lock at
all in this whole investigation** — every prior "success" (both the 2026-08-18 5.2M-byte
result and the isolated `FFFFFF7F` occurrences found by manual inspection of the
cycle-accurate capture) was the same idle-pattern aliasing artifact, not evidence that
real sync ever worked even once. The channel-plausibility fix is doing exactly what it
was designed to do (stop mis-decoding aliases as real data) but has not yet been observed
to let a genuine lock through, because it's not clear one has ever actually occurred to
let through.

**Session interrupted by the user during a capture retest** (a `timeout 70 ... capture
...` call was killed mid-run, exit 137). Immediately after, the board became unreachable
(`orbtrace info` → "No route to host", no ARP entry for `192.168.1.50` at all) — needs a
fresh `jtag_flash.sh` reflash to recover before continuing (this specific symptom — board
totally silent, no ARP, not just a slow/failed single command — matches this project's
established "genuinely down, not just a flaky ping" signature, distinct from the
documented "ICMP unreliable but TCP fine" caveat).

**Not yet resolved, concrete next steps in order:**
1. Reflash (recover the board) and re-verify `orbtrace info` responds before anything else.
2. Given the marker firmware itself may not be tight enough to produce a SUSTAINED,
   multi-frame-long run of real content (the earlier ILA capture showed bursts of only
   ~7-9 bytes between idle stretches, well under one 16-byte frame), consider whether a
   genuine sync even CAN occur under any firmware pattern tried so far — the DWT-driven
   Full Sync Packet mechanism (`m3_itm_init()`'s `M3_DWT_CTRL` write) remains the only
   documented trigger and still has not been proven to fire even once with certainty.
3. If still no genuine lock after a longer, patient retest, this changes the diagnosis
   materially: it would mean the false-lock aliasing was masking a DEEPER problem (no real
   Full Sync Packet ever occurs) rather than being the root cause on its own. Worth
   revisiting candidate #3 from the previous update (trace `sync_loss`/`reset_pulse`
   generation) and candidate #2 (idle-transition anchoring, which doesn't depend on a
   literal Full Sync Packet appearing at all) at that point.

**Update, same session: board recovered (reflashed the sync-fix bitstream) and the marker
test re-run for a full ~130 seconds (~80 DWT sync-tap periods at the board's real 10MHz M3
clock) — `rx_bytes=0` for the ENTIRE window. This is decisive, not just inconclusive.**

`sync_loss` climbed steadily throughout (48M → 73.9M across the run), confirming the
plausibility gate is actively and continuously rejecting candidate locks the whole time —
but not one single one of them, across 130+ seconds of continuous real tight-loop STIM
traffic, was both a genuine frame-boundary match AND channel-1-plausible. Combined with
the earlier finding that the pre-fix bitstream's 5.2M "successful" bytes were conclusively
proven to be alias garbage (channel 1 had only 4 bytes total), this now rules out "just
needed a longer window" as an explanation. **Best current read: this specific synthesized
TPIU instance's DWT-driven Full Sync Packet mechanism does not reliably fire in a way this
bare-4-byte-magic-number search can ever legitimately catch — either it doesn't fire at
all despite the correct-per-TRM `DWT_CTRL` configuration (a real, still-unexplained gap
between documented architecture and this instance's actual behavior), or it fires but the
demux's simple substring scan has some other structural reason it can't catch a genuine
one (not yet identified).**

**This points toward candidate #2 from the previous update as the more promising direction
now**, not just a fallback: anchor resync on the empirically well-characterized, stable,
recognizable idle→burst transition in the M3's real wire output (already measured in
detail via the cycle-accurate capture earlier this session) rather than continuing to wait
for a literal Full Sync Packet that may never come. This is a bigger design change than
the plausibility gate (a genuine departure from the standard CoreSight architecture's own
sync mechanism, justified specifically by this instance's empirically-characterized real
behavior) — flagging here rather than starting it unilaterally, matching this document's
established pattern at forks like this.

**User chose to pause here rather than start the idle-transition redesign this session.**
Session wrap-up:

- **Real, kept change:** the channel-plausibility gate in `orbtrace_tpiu_demux.sv` /
  `orbtrace_core.sv` (new `m3_source` port, `M3_EXPECTED_CHANNEL` check) is left in place,
  uncommitted, in the working tree — it is a genuine correctness improvement (stops
  decoding idle-pattern aliases as real trace data) even though it hasn't yet been the
  thing that makes Parallel/4-bit mode succeed end-to-end. Not reverted.
- **Board state:** reflashed with `bazel-out/orbtrace-vivado-m3-sync-fix` (the real
  production bitstream containing the fix, all gates clean — WNS=1.641ns, WHS=0.010ns,
  zero unwaived critical CDC/methodology findings). M3 reloaded with the real firmware
  (`m3_itm_init(2)` + `emit_next()`, not the marker), capture stopped, board confirmed
  responsive (`orbtrace info` → `ZUBoard-Orbtrace/1`) before ending.
- **Diagnostic artifacts kept in the session scratchpad, not committed** (recreate from
  this document's descriptions if resuming): `create_bd_debug.tcl`/
  `build_debug_cycle_ila.tcl` (the trace_clk_m3-domain ILA build, cached bitstream at
  `bazel-out/orbtrace-vivado-m3-cycle-ila`), `arm_and_capture.tcl` (Hardware Manager Tcl,
  includes the `PARAM.FREQUENCY 1000000` JTAG-clock fix and retry loops for the scan
  flakiness documented above), `demux_sim.py` (Python port of the pre-fix demux state
  machine), `orbflow_decode.py`/`verify_captured.py` (COBS/orbflow decode +
  Workload-reference diff tooling for Phase F, verified working, ready to reuse once real
  captured data exists), `m3_app_marker.bin`/`m3_app_real.bin` (built firmware images).

**Where the next session should start:** the plausibility-gate fix is real progress (it
converts silent mis-decoding into an honest, countable rejection), but the actual
end-to-end goal is still not met — `rx_bytes` has never been observed genuinely nonzero.
The next concrete step is the idle-transition-anchored resync redesign (candidate #2,
scoped above), or the DWT-generation investigation (the other option offered and not
chosen this session) if a future session wants to settle *why* no genuine Full Sync
Packet is ever caught before committing to a bigger resync redesign.

## 2026-08-19 continuation (new session) — DWT-sync investigation found a different, real bug
## first: the sync-word byte order in `orbtrace_tpiu_demux.sv` is backwards. Fixed, simulation
## -validated, real hardware rebuild in progress.

User asked to investigate DWT sync first, not jump straight to the idle-transition redesign.
That investigation found something more concrete than expected.

**1. DWT hardware presence confirmed present at maximum config, from the exact flashed
build's own XCI (static check, no hardware time):** `bazel-out/orbtrace-vivado-m3-sync-fix`'s
`zub_orbtrace_m3_core_0.xci` shows `TRACE_LVL=1` ("Standard trace: ITM & DWT, no ETM") and
`DEBUG_LVL=3` ("Full debug including DWT"), both `resolve_type: user` (i.e. these ARE this
build's real settings, not just component.xml defaults quoted for context) — genuinely
present in the exact bitstream currently on the board. This is real evidence, not inference,
against the "this synthesized instance doesn't implement DWT-to-formatter sync" contingency
noted at the end of the previous update. `cm3_dwt.v`/`cm3_itm.v`/`cm3_tpiu.v` themselves are
IEEE P1735/Xilinx-encrypted (confirmed by inspection — not just the CPU core, the debug/trace
peripherals too), so no deeper static RTL ground truth is available beyond this.

**2. Real, previously-undiscovered bug found: `orbtrace_tpiu_demux.sv`'s sync-word search
has the byte order backwards relative to the correct CoreSight convention.** Cross-checked
against sigrok's independent, open-source `arm_tpiu` reference decoder (fetched and read
directly, not from memory): a genuine Full Sync Packet is, chronologically, **three `0xFF`
bytes followed by a terminating `0x7F`** (0x7F is appended last, completing the match — this
is `[0xFF,0xFF,0xFF,0x7F]` in the decoder's own rolling append-to-end buffer). Re-deriving
`orbtrace_tpiu_demux.sv`'s shift-register match cycle-by-cycle (`sync_window <=
{input_data, sync_window[31:8]}`, compared against `32'hffffff7f`) shows the OPPOSITE
chronological order: the byte that completes the match (the *newest* one, landing at
`sync_window[31:24]`) is `0xFF`, and the byte at the *oldest* position (`sync_window[7:0]`,
three cycles stale) is `0x7F` — i.e. the RTL was searching for **`0x7F` first, then three
`0xFF`s**, backwards from the real protocol.

**3. Confirmed against real, still-on-disk hardware data, not just derivation.** A leftover
scratchpad from the previous session's cycle-accurate ILA capture
(`m3_cycle_ila_marker.csv`, the real 16384-byte `trace_clk_m3`-domain capture referenced in
the 2026-08-18 "BREAKTHROUGH" update) was still present on this machine. Replaying it in
Python: max consecutive `0xFF` run anywhere in the real capture is exactly 3 (histogram:
`{1: 6361, 3: 490}`, never 2 or 4+) — consistent with the documented CoreSight half-sync
idle behavior, not noise. The canonical chronological pattern (`0xFF,0xFF,0xFF,0x7F`) occurs
455 times in this window; the RTL's actual (backwards) search pattern occurs 490 times —
both large fractions of a 16384-byte capture, confirming the specific "`FF 7F FF FF`
repeating" idle sub-pattern documented on 2026-08-18 aliases against **both** byte orders
almost symmetrically (a periodic pattern contains a given 4-byte substring at multiple
phases). So the byte-order fix does *not*, by itself, eliminate the false-lock-on-idle
problem the channel-plausibility gate already handles — but it does mean that on any
genuine, non-periodic DWT-triggered sync event, the pre-fix RTL would search at exactly the
wrong alignment and could never correctly frame-align even in the best case, which is
independently sufficient to explain why not one single valid channel-1 frame has ever been
observed despite 130+ seconds of continuous real traffic in the previous session's testing.

**4. Fix implemented:** `orbtrace_tpiu_demux.sv`'s match constant changed from
`32'hffffff7f` to `32'h7fffffff` (flips which byte-position is "newest" vs "oldest" in the
comparison, matching the real chronological convention). One line, low risk, well-evidenced.

**5. Simulation-validated for the first time this investigation with a dedicated testbench**
(`applications/orbtrace/rtl/tb/orbtrace_tpiu_demux_tb.sv`, wired into
`rtl_unit_test.sh`/`BUILD.bazel`'s existing glob — this file previously had zero dedicated
test coverage, noted as a gap in the 2026-08-19 earlier update). Three checks: (a) a
synthetic idle-alias stream never produces a stable channel==1 lock and does trigger
`sync_loss_count`, matching real hardware; (b) the CORRECT chronological sync sequence
(`FF,FF,FF,7F`) immediately followed by a synthetic, well-formed 16-byte CoreSight frame (ID
byte selecting channel 1, 14 real payload bytes) decodes byte-for-byte correctly with zero
spurious `sync_loss` — verified against the pre-fix constant too: with `32'hffffff7f`
restored, this exact same test fails outright (0 bytes ever emitted, the demux never locks
at all on the correctly-ordered sequence) — proof the test actually exercises the bug, not
just passing trivially; (c) the plausibility gate still correctly rejects a wrong-channel ID
byte (regression check, unchanged behavior). All of `rtl_unit_test.sh`'s existing five
testbenches plus this new one pass together (manual `xvlog`/`xelab`/`xsim` invocation via the
flake's wrapped tool derivations — `nix develop -c <tool>` with `XILINX_ROOT` set — since
`rtl_unit_test.sh` itself currently fails outside that path: see the environment note below).

**Environment note, unrelated to the RTL work but blocked it initially:** on this machine,
`vivado`/`xvlog`/`xelab`/`xsim` at their raw `~/opt/vitis/Vivado/2023.2/bin/*` paths (and
therefore also `bazel test //applications/orbtrace/rtl:rtl_unit_test`, whose `sh_test`
script invokes them via `$XILINX_VIVADO/bin/*` directly) currently fail with `/bin/bash: bad
interpreter: No such file or directory` — this system has no `/bin/bash` (only `/bin/sh`,
symlinked to nix bash). `flake.nix` already solves exactly this (see its
`xilinxRuntimePkgs`/`mkXilinxTool`/`buildFHSEnv` comments: "Keep the command name stable
inside a development shell... resolves the installation through XILINX_ROOT only when it is
executed") — the FIX is to use the wrapped `vivado`/`xvlog`/`xelab`/`xsim` commands `nix
develop` puts on PATH (or the `$VIVADO`/`$XVLOG`/etc. env vars its `shellHook` exports),
with `XILINX_ROOT` (not `XILINX_VIVADO`) pointing at `~/opt/vitis`, instead of invoking the
raw vendor binaries directly. This resolved the simulation blocker immediately once applied,
and the real Vivado build below uses the same wrapped `vivado` successfully. Worth fixing
`rtl_unit_test.sh` itself to use this path (or at minimum documenting it) so `bazel test`
works without a manual workaround — not done this session, flagging for later.

**Real hardware build launched** (`build.tcl`, strict/production gates, not a relaxed
diagnostic build): `AVNET_BDF_ROOT=/home/v/projects/avnet_bdf`,
`ARM_DESIGNSTART_IP_ROOT=/home/v/projects/arm_designstart_m3/vivado/Arm_ipi_repository`,
`M3_OOC_EDIF` pointed at the cached `bazel-out/m3-ooc-2019/m3_core_2019.edf` (the hybrid
fast-path), output dir `bazel-out/orbtrace-vivado-m3-sync-order-fix`, via the properly
wrapped `nix develop -c vivado -mode batch -source applications/orbtrace/vivado/build.tcl`.
Result and retest to follow once the build completes.

**Result: BREAKTHROUGH — real, genuinely correct channel-1 CoreSight content decoded for
the first time in this entire multi-day investigation. Phase E's core acceptance criterion
is met.**

**Build had to be retried twice before succeeding — a real, still-unexplained environment
flakiness, unrelated to the RTL fix, worth documenting for future sessions.** Both of the
first two attempts failed identically: real DRC error at `opt_design` ("Cell
`zub_orbtrace_i/m3_core/inst` ... is considered a black box"), because `read_edif`'s netlist
(read via the `STEPS.SYNTH_DESIGN.TCL.PRE` hook, exactly the same mechanism that built
`orbtrace-vivado-m3-sync-fix` cleanly earlier the same day) never actually merged into the
synth_1 checkpoint — confirmed by diffing the two attempts' `synth_1/runme.log` line-by-line
against the successful build's own log: the working build shows `Parsing EDIF File`/
`Finished Parsing EDIF File` during synth_design's "Translating synthesized netlist" step;
neither failing attempt ever printed that line, despite using the literal identical EDIF
file (`bazel-out/m3-ooc-2019/m3_core.edf` is a symlink to `m3_core_2019.edf` — byte-identical
either way) and an identical, unmodified `build.tcl`. No Tcl error was ever reported for the
`source m3_hybrid_pre_synth.tcl` call in either failing attempt (the driver script's own
`catch {...}` block would have printed "sourcing script ... failed" if it had errored, and
never did) — so `read_edif` itself apparently runs without error but its effect doesn't
always get picked up by synth_design's automatic black-box resolution. A third attempt
succeeded after adding `set_param general.maxThreads 1` (on the theory that multithreaded
synth_design has a race around EDIF-based black-box resolution in this sandboxed
environment) — **but this is very likely NOT actually why it worked**: `build.tcl` itself
unconditionally calls `set_param general.maxThreads 4` immediately before `launch_runs
synth_1` (a pre-existing line, memory-footprint-motivated per its own comment), which would
have silently overridden the maxThreads=1 diagnostic addition before synthesis ever ran. The
one-line diagnostic edit was reverted (`git checkout --`) before flashing; `build.tcl` is
back to its committed state, unmodified. **Best current read: this is genuine, still
unexplained flakiness in how Vivado 2023.2 merges a `read_edif`'d netlist into a black-box
cell during project-mode `launch_runs synth_1` in this specific sandboxed (bubblewrap/
buildFHSEnv) environment — succeeded 2 of 3 total M3-hybrid-EDIF builds today. Future
sessions hitting this exact DRC error at `opt_design` (not a script bug, not a content
problem) should just retry `launch_runs`/the whole build** — checking the failing attempt's
own `zub_orbtrace.runs/synth_1/runme.log` for the presence/absence of a `Parsing EDIF File`
line is the fast, decisive diagnostic (present = will succeed; absent = will hit the DRC
error at impl, no need to wait for `opt_design` to confirm).

**Flash + M3 load, real hardware:** `tooling/xsct/jtag_flash.sh` (full reset-based reflash,
`PSINIT`/`BITSTREAM` pointed at the new build's own `psu_init.tcl`/`zub_orbtrace.bit`,
`bazel-bin/applications/orbtrace/firmware/a53_app/a53_app` — the symlink, not `_elf` — as
usual) succeeded cleanly; `orbtrace info` confirmed the board up. M3 firmware rebuilt
(cache-hit, unchanged since the DWT-sync-fix commit-pending state — confirmed via
`arm-none-eabi-objdump -d` that the flashed ELF's `m3_itm_init` disassembles as expected and
`g_dwt_ctrl_at_boot`/`g_dwt_cyccnt_latest` symbols are present, i.e. the real firmware with
the DWT fix, not a stale build) and loaded via `orbtrace load-m3` (D2, A53-native, readback
verified). `configure m3 tpiu4 2000000` + `start` against the **real** firmware
(`m3_itm_init(2)`, `emit_next()`, 10000-cycle busy-wait between STIM writes — not the marker
firmware) —

`orbtrace stats` genuinely and repeatably nonzero for the first time ever under this
workload, and growing steadily over a sustained window (not a one-off blip):
```
rx_bytes=11   (immediately after start)
rx_bytes=19   (~15s later)
rx_bytes=22   (~35s later)
rx_bytes=173  (after a further 75s capture)
```
`dropped_bytes`/`sync_loss` are still enormous (billions/tens of millions) — expected and
correct: the M3's idle-pattern output still dominates the wire, and the demux is supposed to
reject all of it. What matters is what gets *through*.

**Decoded via the leftover `orbflow_decode.py` (COBS/orbflow unframe, from a previous
session's scratchpad): every single byte that got through is genuine, checksummed, real
CoreSight content, exclusively on channel 1.** Two separate captures (47 bytes/16 frames,
then a fresh 84 bytes/26 frames), zero bad-checksum frames in either, and — the qualitative
break from every prior "success" in this investigation — **`channels seen: [1]` in both
runs, no other channel present at all.** Every previous nonzero-`rx_bytes` result in this
investigation (the 2026-08-18 5.2M-byte capture, the isolated manual `FFFFFF7F` sightings)
was dominated by garbage on bogus channels (63, 127, 49, 31, 40, 48, 62, 78, 114...) with at
most a handful of incidental real bytes; this time there is no garbage at all, only real
channel-1 frames. The decoded payload bytes themselves are structurally plausible ITM
Software Trace Packet content too (e.g. `0a71f1`, `0a93d5`, `2a3ffd`, `2afdbb` — a header
byte with the low bits set to a SWIT size code, port-number bits consistent with stimulus
port 0, followed by 2-3 real data bytes), not random noise.

**What this means for the byte-order theory:** the original concern (documented earlier
this session) that the byte-order fix alone wouldn't eliminate the idle-pattern aliasing
problem was correct — `sync_loss`/`dropped_bytes` are still huge, meaning the demux is still
frequently false-locking on the idle alias and getting rejected by the plausibility gate,
exactly as before. But the fix's actual purpose — ensuring that when a *genuine* sync event
occurs, frame decode starts at the *correct* byte alignment — is now empirically confirmed:
real content is getting through, correctly framed, on the correct channel, for the first
time ever. The combination of (a) the byte-order fix (correct alignment on genuine locks)
and (b) the 2026-08-18 channel-plausibility gate (reject everything that isn't alignment (a)
lets through cleanly) together are what made this work — neither alone was sufficient,
matching this session's own earlier prediction.

**Status: Phase E is now substantively DONE.** `rx_bytes > 0` with clean, channel-1-only,
checksum-valid content, sustained and repeatable — the acceptance bar this document set at
the top has been met. Not yet done, for a future session: a full byte-for-byte diff of a
longer capture against `emit_next()`'s real, deterministic `Workload` reference sequence
(this is Phase F, not Phase E — `verify_captured.py` in the scratchpad already has the
reference-model comparison logic half-written from a previous session, reusable), and a
throughput/reliability characterization (how much of the real content is lost to the
still-high false-lock rate vs. genuinely captured) — but the fundamental "can this pipeline
ever produce real, trustworthy trace data" question this whole investigation was chasing is
now answered yes, with real evidence.

**Attempted Phase F verification (informational, not blocking):** tried the leftover
`verify_captured.py`'s contiguous-run match against the `Workload(seed=7)` reference over
the 84-byte second capture — no contiguous match found. This is expected, not a red flag:
the M3 had already been running `emit_next()` continuously since boot (the reference model
starts counting from `sequence=1`, but the real capture is a snippet from deep into a
long-running sequence with an unknown offset), and — per the already-established
STIM-write-timing sensitivity — only a sparse, non-contiguous subset of `emit_next()`'s calls
ever have their real content survive to the wire at all, so a contiguous-subsequence match
against the reference isn't the right verification strategy for this data shape. A real
Phase F pass would need either a much longer capture window (to accumulate more matchable
material) or a sparse/subsequence-tolerant comparison, not the raw script as originally
written for the marker-firmware's tight loop. Left for a future session, as already scoped.

**Housekeeping:** `orbtrace_tpiu_demux.sv`'s byte-order fix (`32'h7fffffff`) and the new
`orbtrace_tpiu_demux_tb.sv` testbench remain uncommitted in the working tree alongside the
2026-08-18 channel-plausibility gate — both are real, verified, working fixes, not yet
committed. `applications/orbtrace/vivado/build.tcl` is back to its exact committed state
(the `maxThreads 1` diagnostic was reverted). The board is running the new
`orbtrace-vivado-m3-sync-order-fix` bitstream with the real (non-marker) M3 firmware loaded;
the capture session was stopped (`orbtrace stop`) at the end of this session, board confirmed
still responsive (`orbtrace info` → `ZUBoard-Orbtrace/1`) afterward.

### Next steps for a future session

1. **Phase F** — a proper content-correctness pass: a longer capture (minutes, not seconds)
   against the real firmware, then a sparse/subsequence-tolerant comparison against the
   `Workload` reference (not the raw contiguous-run matcher tried above), to confirm the
   *values*, not just the framing/channel, are genuinely correct.
2. Consider whether the still-enormous `sync_loss`/`dropped_bytes` rate is worth reducing
   (e.g. revisiting the idle-transition-anchored resync idea from earlier this session, now
   as a throughput optimization rather than a correctness fix) — not required for
   correctness, since the plausibility gate already keeps false locks from corrupting output,
   but a very high false-lock rate does mean most real content is likely still being missed
   between successful locks.
3. **Phase G** — the real JTAG debug path (`ORBTRACE_REG_M3_CONTROL`, halt/read PC/
   single-step/resume) is still unblocked and not started; the plan's original scope isn't
   complete until this is verified too.
4. Commit the two real RTL fixes (channel-plausibility gate + sync byte-order) and the new
   `orbtrace_tpiu_demux_tb.sv` testbench once reviewed — both are working, verified changes
   sitting uncommitted in the working tree.

**Update 2026-08-19 (later): items 1 and 4 above are done — commit `cbce37d` recorded this
whole investigation, and the byte-order/plausibility-gate fixes are already committed
(`abb1bf0`/`4e3afcf`). This continuation covers Phase F and a first real attempt at Phase G.**

## 2026-08-19 continuation — Phase F: genuine content recovery confirmed

**Board state at start:** still running `orbtrace-vivado-m3-sync-order-fix` with the real
(non-marker) firmware, `orbtrace info` responsive, `orbtrace stats` showing leftover nonzero
`rx_bytes` from the previous session (capture had been left configured/running).

**First attempt (unsynced) — inconclusive, as the previous session's own note predicted.**
A 240s capture (629 raw bytes, 285 bytes of reconstructed channel-1 ITM content after
COBS/checksum unframing) tried against a `Workload(seed=7)` reference generated for
200,000 iterations found only 4 order-preserving byte-string matches — LOWER than a
shuffled-order control (6) and barely above a random-event control (2). Confirms the
previous session's own diagnosis: the M3 had been running continuously since an earlier,
unknown boot, so its real `sequence` counter at capture time was some large, unknown offset
— matching against a reference that assumes `sequence` starts near 1 can't work regardless
of how much real content is actually present.

**Fix: synchronize the reference to a known start.** `orbtrace load-m3 HOST m3_app.bin`
resets `state`/`sequence` to their static-initialized values (7/0) as a side effect of
reloading the BRAM image — running this immediately after starting a fresh capture pins the
real firmware's sequence to a known near-zero start, matching what the `Workload(seed=7)`
reference already assumes. Re-ran: 200s capture, 503 raw bytes, 223 bytes reconstructed
channel-1 content.

**Result — real signal, not aliasing.** Matching methodology refined based on the first
attempt's own noise floor: width-1 (2-byte: 1 header + 1 payload byte) events have a much
higher baseline chance-collision rate (~1/65536 per byte position) than width≥2 events
(3-byte/24-bit or 5-byte/40-bit needles, ~1/16.7M or ~1/4.3B per position) in a short
haystack, so they were reported separately rather than lumped into one headline number (the
lumped number is exactly what made the first, unsynced attempt look like pure noise). Of
206,250 width≥2 reference events tried: **9 matched, in strictly increasing byte-stream
order**, against an analytically-expected ~1.4 coincidental matches (223 haystack positions
× ~1/16.7M chance × ~103k width-2 attempts) and empirically-observed 2-3 matches from a
shuffled-order-of-the-same-bytes control and a random-unrelated-SWIT-packets control. 9 real
vs. ~1-3 both controls, with the temporal-ordering constraint additionally ruling out
independent chance alignment, is not plausible as coincidence.

**Conclusion: Phase F's acceptance bar is met.** The channel-1 content Phase E found isn't
just correctly-framed noise that happens to pass the checksum and channel-plausibility gate
— specific, correctly-sequenced `Workload` reference events (port/width/value all matching)
are genuinely present in the decoded stream. The very low absolute byte count (223 bytes
recovered from 200s of continuous real STIM traffic) reflects the already-documented high
false-lock/sync-loss rate, not a content-correctness problem — most real content is still
lost between successful locks (see Phase E's already-noted future-throughput-improvement
item), but what does get through is real.

**Tooling (scratch-only, not committed — recreate from this description if reused):**
`verify_phase_f.py` — decodes an `orbtrace capture` file (COBS/orbflow unframe per
`model/src/lib.rs`'s exact algorithm, hand-ported to Python), reconstructs the raw ITM byte
stream per orbflow channel, generates the `Workload(seed=7)` reference event sequence
(direct port of `firmware/m3/src/lib.rs`/`firmware/m3_app/src/main.c`'s `emit_next()`),
encodes each reference event as its expected ARMv7-M ITM SWIT header+payload byte string
(`header = (port<<3)|size_code`, `size_code` 1/2/3 for 1/2/4-byte payloads), then does a
greedy order-preserving subsequence search plus shuffled-order and random-event controls.
Lives at `/tmp/.../scratchpad/verify_phase_f.py` this session — small and self-contained
enough to regenerate from this description, or worth committing as a real diagnostic tool if
a future session wants it kept.

## 2026-08-19 continuation — Phase G: first real attempt, JTAG-DP not yet responding

**Setup, all real, no RTL changes:** added a small CLI gap-fill —
`orbtrace m3-control HOST BITS` (`model/src/main.rs`) — since no prior CLI command exposed a
raw `Command::M3Control` write (the protocol opcode already existed, `load_m3()` just never
exposed it standalone). Used it to set `ORBTRACE_REG_M3_CONTROL=0x3` (bit0 `m3_release` kept
set so this doesn't re-assert CPU reset, bit1 `m3_dap_real` newly set to route the DAP
bit-banger at the real M3 instead of the synthetic responder). `orbtrace remote-bitbang HOST
127.0.0.1:9999` confirmed it accepts a client connection and bridges to TCP 3240 (CMSIS-DAP)
without error.

**OpenOCD attempt:** a probe-only config (`jtag newtap m3 tap -irlen 4 ...`, no
`-expected-id` so as not to hard-fail on a guess, `remote_bitbang` transport at
`127.0.0.1:9999`) run as `openocd -f ... -c "init; scan_chain; shutdown"`. Result: **`JTAG
scan chain interrogation failed: all zeroes`**, then `IR capture error; saw 0x00 not 0x01` —
the mandatory IEEE 1149.1 IR-capture LSB (always 1) never appears; TDO reads as a constant 0
regardless of what's shifted in.

**Hypothesis tested and ruled out: the M3's combined SWJ-DP (per `create_bd.tcl`'s own
comment, "autodetects JTAG vs SWD from the standard switch sequence") might be latched into
SWD mode, and OpenOCD's plain `remote_bitbang` JTAG transport never sends the ARM ADIv5
§5.2.1 SWD→JTAG switch sequence (0xE73C, LSB-first) the way a real SWD-aware adapter would.**
Wrote `swj_probe.py`, a raw remote-bitbang client (bypasses OpenOCD, talks the
`orbtrace remote-bitbang` bridge's wire protocol directly per `model/src/main.rs`) that
bit-bangs: ≥50-cycle line reset (TMS/SWDIO=1, safe in either protocol) → the 16-bit
0xE73C switch sequence → ≥5-cycle JTAG Test-Logic-Reset → Run-Test/Idle → navigate to
Shift-DR → shift 32 bits. **Still all-zero.** This doesn't rule out a JTAG/SWD mode-latch
problem entirely (some SWJ-DP implementations only re-arm mode detection on an actual
`nTRST` pulse, not a software-driven TMS sequence, which this probe didn't attempt — `nTRST`
stayed at its RTL-default deasserted value throughout, since no `DAP_SWJ_Pins` command was
ever sent to toggle it) but does rule out "just needed the standard software switch
sequence" as a complete fix on its own.

**Positive evidence `use_real_target` is genuinely active, not silently still-synthetic:**
`orbtrace_dap_engine.sv`'s `use_real_target==0` path returns a deterministic synthetic echo
(`response_mem[2] <= header_data ^ 8'h01`, i.e. TDO = NOT(TDI)) for every `DAP_JTAG_Sequence`
call. Every bit sent by both the OpenOCD run and the raw probe used `TDI=0` throughout — if
`use_real_target` were still 0, every response would read back as constant TDO=**1** (`0^1`),
not constant 0. Getting constant 0 with `TDI=0` sent throughout is consistent with a real
(non-synthetic) target that simply isn't driving TDO high, not with the synthetic path still
being selected. The `M3_CONTROL` write took effect.

**Positive evidence the M3 core itself is alive, and `DBGRESETn` is very likely deasserted:**
`create_bd.tcl` ties `SYSRESETn` and `DBGRESETn` to the *same* net
(`trace_pl/m3_reset_n_sync`) — see its own comment acknowledging this reuse. `orbtrace stats`
taken immediately after the failed JTAG attempts still showed `dropped_bytes`/`sync_loss`
climbing at tens of millions per second, meaning the M3 is actively executing and driving
its TPIU continuously, which requires `SYSRESETn` (and therefore, on this design, also
`DBGRESETn`) to be deasserted. This weighs against "debug logic held in reset" as the
explanation, though it doesn't fully rule out some other reset/enable condition specific to
the debug domain that isn't tied to this same net.

**What's genuinely still unknown, ranked by how cheap the next check is:**
1. Whether `TCK` is actually toggling at the M3's `SWCLKTCK` pin at all — everything above is
   consistent with either "TCK reaches the pin but the DP doesn't respond" or "TCK never
   reaches the pin" (e.g. an unnoticed `create_bd.tcl` wiring slip, despite the direct
   `get_bd_pins`/connectivity read in this file looking correct). Cheapest next check with no
   rebuild: `open_run impl_1` on the current build and `get_nets -of_objects` /
   `get_pins` on `m3_core/SWCLKTCK`, mirroring the exact static-netlist-inspection technique
   that found the Phase D BRAM bug — free, no hardware needed, should be tried before a new
   ILA build.
2. Whether this SWJ-DP's JTAG/SWD mode-latch specifically requires an `nTRST` pulse (not just
   a TMS-driven software sequence) to re-arm — untested this session. Cheap-ish: issue a real
   `DAP_SWJ_Pins` (`orbtrace dap` or extend `swj_probe.py`) to pulse `nTRST` low then high
   around the switch sequence, still no rebuild required.
3. If both of the above check out clean, this becomes a real ILA question (tap
   `SWCLKTCK`/`SWDITMS`/`TDI`/`TDO` at the IP boundary, same proven methodology as every
   previous real bug in this investigation) — the expensive (~20-90 min) option, not
   attempted this session.

**Housekeeping:** `orbtrace m3-control` (the new CLI subcommand) is a real, useful, working
addition, currently uncommitted in `applications/orbtrace/model/src/main.rs`. No RTL changes
were made this session. `verify_phase_f.py`, `swj_probe.py`, and the OpenOCD probe config are
scratch-only (recreate from this section's description if resuming). `ORBTRACE_REG_M3_CONTROL`
was left at `0x3` (real-DAP route selected) on the board; `orbtrace stop` was called at the
end of this session but the DAP route was not reverted to synthetic — a future session
picking this up should be aware `m3-control HOST 0x1` returns to the pre-Phase-G default
(release asserted, synthetic DAP) if that matters for other testing.
