# Plan: R5/A53 (PS-side CoreSight) as debug/trace targets

Goal: turn the *already-partially-built* `Source::R5`/`Source::A53` path in
this repo's `orbtrace` application into a genuinely working trace pipeline
for the ZynqMP PS's own hardened Cortex-R5 and Cortex-A53 cores — a
different, and structurally more capable, target than the Cortex-M3 soft
core `M3_TRACE_VERIFICATION_PLAN.md` and `M3_PERFETTO_VISUALIZATION_PLAN.md`
cover. This document owns everything specific to the R5/A53 targets;
anything about the M3 stays in those two documents (see section 0, item 5).

## 0. How to hand off sessions on this document

Same protocol as `M3_TRACE_VERIFICATION_PLAN.md` section 0 (that document
is the origin of this convention — read it if this is unfamiliar). In
short:

1. The status table (section 1) is live state — edit rows in place, never
   leave a stale "DONE" standing.
2. Everything below the table is an append-only log — new dated `###
   Update` sections, mark superseded content in place, never delete.
3. "DONE" requires a real, checkable artifact (passing host test, or a
   real hardware readback/capture) — not "should work."
4. End every session with: committed vs. scratch state, exact hardware
   state left behind, and a **Next steps** list, cheapest first.
5. Cross-reference, don't duplicate: this document owns R5/A53 hardware
   bring-up and PS-side CoreSight specifics. `M3_TRACE_VERIFICATION_PLAN.md`
   owns the M3 soft core and its Phase H ETM feasibility analysis (worth
   reading first — the bandwidth-ceiling reasoning there is the same shape
   of analysis this document will eventually need for R5/A53, just against
   different, likely more favorable numbers). `M3_PERFETTO_VISUALIZATION_PLAN.md`
   owns the ITM-to-JSON pipeline this document's eventual decoder should
   reuse/extend rather than duplicate (frame reconstruction, JSON writer).

## 1. Status

| Phase | What | Status |
|---|---|---|
| 1 | Confirm what already exists and what's actually reachable from real firmware | **DONE, 2026-08-19 — analysis only, no hardware touched.** See section 2 |
| 2 | Wire `coresight::select()` into `a53_app`'s real `configure` command handler | **DONE, 2026-08-19 — host unit tests + real aarch64 cross-build only, no hardware touched.** See section 9 |
| 3 | Confirm RPU (R5 subsystem) is actually usable on this board/PS config | **DONE, 2026-08-19 — real hardware, R5-0 booted and ran real ThreadX firmware.** See section 10 |
| 4 | Build real target firmware for R5 and/or a second A53 core to actually trace | **DONE, 2026-08-19 — real hardware, R5-0 booted the new deterministic workload.** See section 11 |
| 5 | Boot/load path for that target firmware | **DONE, 2026-08-19 — reused Phase 3's existing R5 JTAG boot flow unchanged.** See section 11 |
| 6 | First real hardware capture on the R5/A53 path, verify PS/ETM sync (mirrors M3 Phase E/F) | **STARTED, 2026-08-19 — two real address/offset bugs found+fixed with real UG1085/DDI0500J documentation; ETM confirmed genuinely tracing; still 0 bytes reach the PL, TPIU formatter suspected next.** See section 13 |
| 7 | ETMv4 host-side decoder | NOT STARTED |
| 8 | Perfetto integration (reuse `M3_PERFETTO_VISUALIZATION_PLAN.md`'s pipeline/JSON writer, extend for ETMv4 call-stack bars per that document's Phase-I-style discussion) | NOT STARTED |

## 2. What already exists (confirmed 2026-08-19, by reading the real code — not assumed)

This path was evidently scoped early in the project (the `Source` enum
predates the M3-specific work) and left partially built. The pieces that
exist are real, non-trivial, and already tested at the unit level:

- **Wire protocol**: `Source::R5` / `Source::A53` are real values in
  `model/src/lib.rs`'s `Configure` command, alongside `CortexM3`/`Test`,
  already accepted by the CLI's `configure` subcommand (`main.rs`).
- **The RTL mux is real, wired, and source-agnostic** —
  `orbtrace_core.sv` → `orbtrace_source_mux.sv`: `source_select` chooses
  between `m3_data`, `coresight_data` (the PS's real trace output), and
  `test_data`. **R5 and A53 both route through the same `coresight_data`
  input** — the PL doesn't need to know which target it is, only whether
  it's "M3" or "the PS's own CoreSight system." Which real core feeds
  `coresight_data` is decided entirely upstream, in firmware (see below).
- **The M3-only plausibility gate is already correctly scoped out** for
  this path: `orbtrace_tpiu_demux`'s `m3_source` input
  (`source_select == 2'd0`) means R5/A53 traffic gets the demux's
  original, unmodified sync-detection logic — the M3-specific heuristic
  built later (`M3_TRACE_VERIFICATION_PLAN.md`'s 2026-08-18 gate) never
  applies here, by design, per that module's own comment ("Leaves the
  PS/ETM path (m3_source==0) bit-for-bit").
- **Real PS-side CoreSight firmware exists**: `firmware/a53/src/lib.rs`'s
  `coresight` module. Genuine ZynqMP CoreSight system addresses, sourced
  from **UG1085 Figure 39-8** (the module's own comment cites this):
  `R5_0_ETM = 0xfe3f_c000`, `A53_1_ETM = 0xfe54_0000`,
  `FUNNEL_RPU = 0xfe11_0000`, `FUNNEL_APU = 0xfe12_0000`,
  `FUNNEL_SYSTEM = 0xfe13_0000`, `TPIU = 0xfe18_0000`. `coresight::select(mmio,
  a53_1: bool)` unlocks the chosen target's ETM plus every funnel on its
  path (`CORESIGHT_UNLOCK` to each `LAR`) and routes it through to the
  system funnel with the correct per-target funnel-select values. A real,
  passing unit test (`coresight_routes_r5_and_a53_1_through_distinct_funnels`)
  confirms the exact register-write sequence for both targets.
- **`create_bd.tcl` already enables and wires the PS's own trace
  peripheral** into the same `trace_pl` block the M3 path shares:
  `CONFIG.PSU__TRACE__PERIPHERAL__ENABLE {1}`,
  `CONFIG.PSU__TRACE__PERIPHERAL__IO {EMIO}` (routed through PL fabric,
  **not physical board pins** — a materially different situation from the
  M3 DesignStart IP's hard-pinned `TRACEDATA[3:0]`),
  `CONFIG.PSU__TRACE__WIDTH {4Bit}` with
  `CONFIG.PSU__TRACE__INTERNAL_WIDTH {32}` (today's 4-bit is a
  *configuration* choice against a 32-bit-wide internal path, not a
  macrocell ceiling), and a PS-generated
  `CONFIG.PSU__CRF_APB__DBG_TRACE_CTRL__FREQMHZ {100}` trace clock — an
  **independent 100 MHz clock domain**, not tied 1:1 to any core's own
  execution clock the way the M3 DesignStart macrocell's `TRACECLK` is.
  `ps/ps_pl_tracedata` → `trace_pl/trace_data` and `ps/trace_clk_out` →
  `trace_pl/trace_clk` are both already connected.

**The one real gap in the wiring, confirmed by grep, not assumed:**
`coresight::select()` is called **only from its own unit test**.
`firmware/a53_app/src/main.c` — the actual C firmware that handles the
real `configure` TCP command — never calls it at all. So today, selecting
`Source::R5`/`Source::A53` over the wire protocol sets `source_select` in
the PL (real, working) but never actually routes any real core's ETM
through the CoreSight funnels to feed it (not wired). This is a small,
concrete, well-scoped fix — see Phase 2.

## 3. Why R5/A53 differs structurally from the M3 path — real advantages, not yet verified on hardware

Cross-referencing `M3_TRACE_VERIFICATION_PLAN.md` Phase H's bandwidth
analysis for the M3 soft core, several of that analysis's hard IP
ceilings plausibly don't apply here, though none of this is verified on
real hardware yet:

1. **Trace clock isn't tied 1:1 to core clock.** The PS's
   `DBG_TRACE_CTRL` is its own independent clock domain (100 MHz today,
   separately configurable) — Phase H's "raising HCLK can't help, the
   ratio is fixed" argument was specific to the M3 DesignStart macrocell
   having no `TRACECLKIN` input at all; the PS block has a real,
   independent trace clock.
2. **Not pin-constrained.** EMIO routing plus a 32-bit internal path
   narrowed to 4-bit by configuration (not by a hard-declared `[3:0]`
   vector the way the M3 IP's `component.xml` was) — widening this is at
   least plausible, unlike the M3 path where `TRACEDATA` literally has no
   width parameter to change.
3. **ETMv4, not ETMv3.5** — denser encoding, same protocol family as
   Cortex-A/R-class generally, on both R5 and A53.
4. **Real hardened-core debug.** R5/A53 already go through the standard
   Xilinx JTAG/xsct debug flow this project already relies on for
   `psu_init`/A53 ELF loading — the `DBGEN`-tied-low situation blocking M3
   halting (`M3_TRACE_VERIFICATION_PLAN.md` Phase G) is unlikely to recur
   here, though not yet confirmed for this specific board.
5. **Likely richer ETM resource sets.** The M3's ETM-M3 config is the
   most stripped-down possible (0 address comparators, 0 data
   comparators, no sequencer — `M3_TRACE_VERIFICATION_PLAN.md` Phase H).
   Production Cortex-A53/R5 cores typically implement fuller ETMv4
   configurations, plausibly including real address/data comparators and
   possibly an ETR (trace-to-system-memory) sink — which would sidestep
   the entire pin-bandwidth question Phase H spent real effort on for M3
   entirely. **Not confirmed for this exact ZU1CG part** — checking the
   real UG1085/Cortex-A53 TRM CoreSight configuration for this die is
   real, valuable Phase-3-adjacent work, not assumed here.

## 4. Phase 2 — Wire `coresight::select()` into the real `configure` handler

`firmware/a53_app/src/main.c` needs to call
`coresight::select(mmio, a53_1)` when the incoming `Configure` command's
`source` is `Source::R5` (`a53_1=false`) or `Source::A53` (`a53_1=true`),
before the PL's own `source_select` register write that `orbtrace_core.sv`
already consumes. Small, mechanical, but genuinely untested — the
existing unit test only proves the register-write *sequence* is correct
in isolation, not that invoking it at the right point in real firmware
against real MMIO produces a working end-to-end trace.

## 5. Phase 3 — Confirm RPU availability

No explicit RPU enable/configuration was found in `create_bd.tcl` or
`sdk/boards/zub_1cg/board_preset.tcl` (checked by grep, not found). The
RPU is presumably always physically present on ZU+ silicon (unlike GP AXI
ports, which do need explicit `PSU__USE__*` enables), but this needs a
real confirmation before Phase 4 invests in R5 firmware — e.g. via `xsct`
target enumeration, or a trivial hand-written R5 stub that just toggles a
GPIO/writes a known SRAM location, loaded and readback-verified the same
way Phase D verified the M3 BRAM load.

## 6. Phase 4 — Target firmware

Unlike the M3, which already had `firmware/m3_app`, nothing currently
runs on the R5 or on a second A53 core (the one A53 core in active use
runs `a53_app`, the Orbtrace control agent itself — it cannot be its own
trace target while also serving the control TCP sockets). This phase
needs a new, real workload — could start as simple as `m3_app`'s own
deterministic `Workload` pattern (reusable design, already proven for
verifying decode correctness end-to-end in `M3_TRACE_VERIFICATION_PLAN.md`
Phase F) ported to run natively on R5 or the second A53 core, before
attempting anything application-specific.

## 7. Phase 5 — Boot/load path

R5 and a second A53 core don't have an equivalent of the M3's
`orbtrace load-m3` BRAM-streaming trick. This would use the standard
Xilinx multi-core boot flow (FSBL configuration for R5, or `xsct`'s own
`dow`/`con` for a second A53 core) — well-trodden ground already exercised
for this project's own PS bring-up, likely simpler than the M3's
from-scratch load path was.

## 8. Phases 6-8 — Capture verification, ETMv4 decoder, Perfetto integration

Mirror `M3_TRACE_VERIFICATION_PLAN.md`'s Phase E/F (get a real capture,
confirm genuine content recovery against a known reference workload
before trusting anything downstream) and
`M3_PERFETTO_VISUALIZATION_PLAN.md`'s pipeline shape (frame
reconstruction → protocol decode → Perfetto JSON), but against ETMv4
instead of ITM SWIT/ETMv3.5. Not detailed further here until Phases 2-5
produce a real capture to decode — writing a protocol decoder against
packet-format assumptions instead of real captured bytes has already
bitten this project once (`M3_TRACE_VERIFICATION_PLAN.md`'s own early
Phase E false-starts), worth not repeating.

## 9. Phase 2 — real implementation (2026-08-19)

Wired into `firmware/a53/src/lib.rs`'s `Controller::command`, not
`a53_app/src/main.c` as originally scoped in section 4 — the `Configure`
command is already fully handled in Rust (`main.c` only pumps bytes
through `orbtrace_control_feed`), so the actual hook point is the opcode-2
match arm in `Controller::command`, one level below where section 4
guessed the C file would need to change.

- Added `RegisterIo::select_coresight_source(&mut self, a53_1: bool)`,
  default no-op (same pattern as `write_m3_bram`/`read_m3_bram`), so
  existing register-level test mocks are unaffected unless they opt in.
- `Controller::command`'s `Configure` handler (opcode 2) now matches
  `request[3]` (the wire `Source` byte: `R5 = 1`, `A53 = 2`) and calls
  `select_coresight_source(false)`/`(true)` **before** the existing
  `REG_SOURCE_FORMAT`/`REG_SWO_BAUD` writes, so a real core is already
  routed to `coresight_data` by the time the PL's `source_select` picks
  it up. `Source::CortexM3`/`Source::Test` are untouched, matching the
  RTL mux boundary described in section 2.
- Real hardware path: `firmware/a53/src/ffi.rs`'s `Mmio` now also
  implements `select_coresight_source` via a new `CoresightMmio` struct
  (implements `coresight::Mmio`, raw `write_volatile` at the absolute
  CoreSight physical addresses — deliberately a separate address space
  from `RegisterIo`'s `ORBTRACE_AXI_BASE`-relative offsets).
- Three new host unit tests in `firmware/a53/src/lib.rs`
  (`configure_r5_selects_coresight_before_source_format`,
  `configure_a53_selects_coresight_before_source_format`,
  `configure_m3_does_not_touch_coresight`) confirm the dispatch and
  ordering via a recording `RegisterIo` mock.
- Verified: `bazel test //applications/orbtrace/firmware/a53:control_firmware_test`
  passes (all tests, including the three new ones), and
  `bazel build --config=apu //applications/orbtrace/firmware/a53:control_firmware_a53
  //applications/orbtrace/firmware/a53_app:all` succeeds — the real
  aarch64-none-elf firmware compiles with the new CoreSight write path.
  **Not yet verified on real hardware** — no capture attempted, no
  confirmation that the funnel-select writes actually produce ETM traffic
  on `coresight_data` for either target. That's Phase 3 onward.

Committed vs. scratch: all changes are in tracked source files (`lib.rs`,
`ffi.rs`) plus this document; nothing left in a scratch/uncommitted state.
Hardware state: untouched this session — no board interaction happened.

## 10. Phase 3 — RPU availability confirmed (2026-08-19)

Section 5's proposed checks (grep `create_bd.tcl`/`board_preset.tcl`, xsct
target enumeration, or a hand-written R5 stub) turned out to already be
answered by existing, unrelated project infrastructure this document's
authors hadn't cross-referenced: `applications/rpu/hello_world` (a real
ThreadX firmware target) plus `tests/rpu_hello_world_test.sh`, which
drives `tooling/zub_ctl`'s `watch-r5` — OpenOCD (`tooling/openocd/aes_zub.cfg`
+ `psu_init_run.tcl` + `load_r5.tcl`) boots R5-0 directly via JTAG/CoreSight
(module reset release, OCM ELF load, PC/CPSR redirect), no FSBL, no BOOT.BIN,
independent of the PL bitstream entirely — confirming the RPU needs no
`create_bd.tcl`/Vivado enable at all (it's PS hardened silicon, not a
PL-instantiated block, unlike the M3 DesignStart soft core).

**Ran for real** (`bazel test --config=host --config=onboard
--test_env=ZUB1CG_PSINIT //tests:rpu_hello_world_test`, `ZUB1CG_PSINIT`
pointed at `sdk/boards/zub_1cg/generated/psu_init.tcl`): R5-0 released from
reset, ran real ThreadX, and printed over UART0
(`/dev/ttyUSB1`) — `--- ThreadX Hello World (AES-ZUB R5F) ---`,
`[TEST BEGIN] hello_world`, `Hello, World!`, `[TEST PASS] hello_world`.
Test **PASSED**. RPU0 is confirmed real, usable, and already has a working
boot flow — Phase 4/5 (R5 target firmware + boot path) can build directly
on `applications/rpu/hello_world` and `tooling/openocd/load_r5.tcl` instead
of inventing a new path from scratch, materially shrinking those phases'
scope from what section 7 originally assumed.

**Real gotcha, worth remembering:** `psu_init_run.tcl` (chained by
`watch-r5` before `load_r5.tcl`) runs the **full** Vitis-generated
`psu_init` — not an RPU-scoped subset — which reprograms system-wide PS
PLLs/clock muxes. Running this JTAG sequence while the A53 Orbtrace
service (this session's own freshly-flashed `a53_app`) was live and
network-reachable **silently killed its network reachability**
(`orbtrace info`/ARP both went dead immediately after, host-side USB link
itself stayed up — ruled out re-enumeration) — almost certainly GEM2's
clock tree glitching from the shared PLL reprogram, not anything specific
to R5 or to this document's own firmware changes. `aes_zub.cfg` is
carefully scoped to avoid examining A53 CoreSight (so the A53 core itself
plausibly kept executing throughout — not confirmed either way), but
**`psu_init` itself is not scoped to be side-effect-free against a
concurrently-running A53** regardless. Recovered with the standard
`tooling/xsct/jtag_flash.sh` reflash (bitstream
`bazel-out/orbtrace-vivado-m3-10mhz-r4/zub_orbtrace.bit` — the last
documented known-good production build per [[orbtrace_m3_integration]],
paired with a freshly rebuilt `a53_app` carrying this session's Phase 2
changes); `orbtrace info 192.168.1.50` confirmed responsive again
afterward. **Do not run any R5/RPU JTAG flow (`watch-r5`,
`rpu_hello_world_test`, or anything sourcing `psu_init_run.tcl`) while a
network-dependent A53 test is in progress or being relied upon** — treat
them as mutually exclusive board sessions, plan on a reflash after
switching from RPU work back to A53 work.

## 11. Phases 4 and 5 — R5 target firmware + boot path (2026-08-19)

Built the "new, real workload" section 6 called for, deliberately scoped
small per this document's own "before attempting anything
application-specific" guidance:

- **`applications/orbtrace/firmware/rpu`** (new Rust crate, host-testable):
  a `Workload` producing a deterministic, reproducible sequence of
  `Action`s (`Marker`/`Spin`/`Branch`/`Fault`/`Call(0..=6)`) from a
  seeded xorshift PRNG. Deliberately **not** a port of the M3 `Workload`'s
  ITM-stimulus-event model — ETMv4 traces instruction/branch flow
  directly, there's no "emit" call to model. What this instead captures is
  *which distinguishable, noinline branch target* a real R5 workload
  visits next, so a future ETM decoder can cross-check recovered branch
  addresses against a known sequence — the same verify-against-a-known-
  sequence methodology `M3_TRACE_VERIFICATION_PLAN.md` Phase F used for
  ITM content. Two host unit tests (`reproducible`, matching M3's own;
  `visits_every_call_target`, confirming the PRNG's distribution actually
  exercises all 7 call targets within a bounded window).
- **`applications/rpu/orbtrace_workload`** (new C firmware, ThreadX,
  mirrors `applications/rpu/hello_world`'s structure): hand-ports the same
  xorshift/dispatch sequence, calling real `__attribute__((noinline))`
  functions per action so the branch targets stay genuinely distinct
  addresses in the compiled ELF (not inlined/merged away) — `action_call0`
  through `action_call6`, `action_branch_true`/`_false`, `action_fault`,
  `action_marker`, plus a bounded busy-loop for `Spin`. Each does a real,
  cheap side effect (`g_heartbeat` update) so the compiler can't dead-code
  them either. `TEST_BEGIN`/`TEST_PASS` markers follow the same
  `zub_ctl`/`test_proto.h` convention as every other RPU app in this repo.
- Test scaffolding added to `tests/BUILD.bazel`/`tests/rpu_orbtrace_workload_test.sh`,
  mirroring `rpu_hello_world_*` exactly (`firmware_elf_test`,
  `firmware_size_test`, `onboard_firmware_test`).

**Phase 5 needed no new work at all** — section 7's assumption that R5
needs its own boot/load mechanism was wrong (superseded by section 10's
finding): the exact same `tooling/openocd/load_r5.tcl` +
`psu_init_run.tcl` + `zub_ctl watch-r5` flow Phase 3 already proved on
`hello_world` booted this new firmware unchanged, just pointed at a
different ELF.

**Verified for real, not just built:** `bazel test --config=host
//tests:rpu_orbtrace_workload_elf_test //tests:rpu_orbtrace_workload_size_test`
pass, and `bazel test --config=host --config=onboard --test_env=ZUB1CG_PSINIT
//tests:rpu_orbtrace_workload_test` passed against real hardware — R5-0
booted, printed `--- ThreadX ETM Workload (AES-ZUB R5F) ---`, and reached
`[TEST PASS] orbtrace_workload` (confirming the thread reached its main
loop, not that any ETM capture was attempted — that's still Phase 6).
As expected per section 10's gotcha, this again killed the A53 Orbtrace
service's network reachability; recovered with the same `jtag_flash.sh`
reflash as before (`orbtrace-vivado-m3-10mhz-r4` + freshly rebuilt
`a53_app`), confirmed via `orbtrace info` afterward.

**Not yet done:** actually enabling ETM tracing on R5-0 while this
workload runs (`coresight::select()`'s current implementation only
unlocks components and wires funnels — it does not program the ETM's own
`TRCPRGCTLR`/config/comparator registers to start tracing), and attempting
any capture at all. That's Phase 6.

Committed vs. scratch: all changes are in tracked source files (new
`firmware/rpu` crate, new `applications/rpu/orbtrace_workload` app,
`tests/BUILD.bazel`, new `tests/rpu_orbtrace_workload_test.sh`) plus this
document; nothing left uncommitted. Hardware state: board left running the
just-reflashed A53 Orbtrace service (confirmed responsive via
`orbtrace info`), same production bitstream as every other recent session.

## 12. Phase 6 — first real capture attempt (2026-08-19)

**Resolved the section 11 open question first, and it changed the
workflow for the better:** the A53-network-killing effect from sections
10/11 is specifically caused by `psu_init_run.tcl`'s *full* Vitis
`psu_init` re-run, not by an R5 JTAG session in general. Confirmed on
real hardware: `openocd -f tooling/openocd/aes_zub.cfg -f
tooling/openocd/load_r5.tcl` (booting `orbtrace_workload` on R5-0,
*omitting* `psu_init_run.tcl` entirely — `load_r5.tcl` already has its
own fallback R5-clock-enable path for exactly this case) left the A53
Orbtrace service's network fully responsive throughout and after. **R5
JTAG work and a live A53 network session are NOT mutually exclusive after
all — only `psu_init_run.tcl` specifically is the thing to avoid
alongside a live A53.** One real, minor side effect: R5-0 and the A53
share the same physical UART0/`/dev/ttyUSB1` console (`load_r5.tcl`'s own
`setup_uart` reprograms UART0 registers directly), so running both
concurrently interleaves/occasionally garbles printed diagnostic text on
that shared serial line — cosmetic only, not a functional problem, not
worth fixing for a bring-up workload.

**Implemented the ETM-enable step `coresight::select()` was always
missing:** `firmware/a53/src/lib.rs`'s `coresight` module gained
`enable_trace()`, alongside a full ETMv4 register-offset map
(`TRCPRGCTLR`, `TRCCONFIGR`, `TRCSYNCPR`, `TRCTRACEIDR`, `TRCVICTLR`,
`TRCOSLAR`, etc.) and a minimal "trace everything unconditionally, no
branch broadcast/cycle-count/data-trace/address-filtering" configuration,
with a distinct nonzero trace ID per target (R5-0 = `0x10`, A53-1 =
`0x20`). **Explicitly best-effort**: these offsets/values are recalled
from public ETMv4 reference material (the shape Linux's coresight-etm4x
driver programs), not cross-checked against a local copy of the real
Cortex-R5/A53 TRM — `internal/reference_docs/` intentionally doesn't
carry those PDFs in this repo/sandbox. Wired into `RegisterIo` (new
`enable_coresight_trace` method, same default-no-op pattern as
`select_coresight_source`) and called right after `select_coresight_source`
in the `Configure` opcode-2 handler. Two new host unit tests
(`enable_trace_starts_the_selected_etm_with_a_unique_trace_id`, and the
existing `configure_*` tests extended to check call *ordering* via a new
`CoresightCall` enum). Verified: host tests pass, real aarch64 cross-build
succeeds.

**Real hardware capture attempt, real result: zero bytes, but a genuine,
specific root cause found — not "trace doesn't work."** Sequence: reflashed
A53 with the new firmware; booted R5-0 with `orbtrace_workload` via the
now-known-safe JTAG-only flow above; ran `orbtrace configure 192.168.1.50
r5 tpiu4 2000000` (real TCP, reaches the new `enable_trace()` code) then
`orbtrace start` then `orbtrace capture ... ` for several seconds.
**Result: 0 bytes captured.** `orbtrace stats` showed `rx_bytes=0
dropped_bytes=0 fifo_high_water=0` but `sync_loss` climbing into the
hundreds of millions — the demux/FIFO pipeline is running (searching for
sync continuously) but never sees any real toggling data at all on
`coresight_data`, a materially different signature from M3's earlier
"real data but can't sync" symptom.

**Root-caused via direct JTAG readback** (all these CoreSight system
addresses are AXI-visible from the PS, confirmed already in section 2 —
no new tooling needed, just ad hoc `openocd` + `read_memory`/`mww`, same
technique this project has used throughout):
- **R5-0's ETM registers are real and correctly programmable.** Read back
  exactly what firmware wrote: `TRCPRGCTLR=0x1`, `TRCCONFIGR=0x0`,
  `TRCSYNCPR=0x8`, `TRCTRACEIDR=0x10`, `TRCVICTLR=0x1`. This is strong,
  direct evidence `R5_0_ETM = 0xfe3f_c000` (from Phase 1's original
  analysis) is correct and that `enable_trace()`'s offsets are at least
  self-consistent with a real, responsive register block at that address.
- **`FUNNEL_RPU` (`0xfe11_0000`, from the same Phase 1 analysis) does
  not behave like a real CoreSight component at all.** A raw JTAG write of
  `1` to its control register (offset `0x000`), after unlocking its LAR,
  read back `0` both immediately and 500ms later — the write never stuck.
  Worse: its Peripheral/Component ID registers (offsets `0xfcc`-`0xfec`,
  which are hardwired ROM values on any *real* CoreSight component,
  architecturally never all-zero) read back `0x00000000` across the board.
  `A53_1_ETM` (`0xfe54_0000`) was worse still — reading its own ID
  registers hit a hard `JTAG-DP STICKY ERROR`, not just silent zeros.
  **This means `coresight::select()`'s funnel-routing writes have likely
  been going to wrong/unmapped addresses since Phase 1 — a real,
  previously-uncaught bug in the addresses that analysis phase cited from
  "UG1085 Figure 39-8"** (not independently verified against the real TRM
  at the time, since it wasn't available in this sandbox then either).
  This directly explains the zero-byte capture: even with the ETM itself
  correctly configured and enabled, its output has nowhere real to go.
- Checked whether the licensed Xilinx toolchain (`~/opt/vitis`) had any
  authoritative header/XML with these addresses (a real, cheap thing to
  try before concluding "no source available") — it doesn't; the bundled
  SDK data is legacy Zynq-7000-era (2019.1), no ZynqMP-specific CoreSight
  system map present.
- Stopped probing further blind physical addresses at this point —
  diminishing returns, and one probe already tripped a JTAG-DP STICKYERR
  (harmless, self-recovering via `clear_stickyerr`, confirmed A53's TCP
  session was unaffected afterward, but a real signal to stop guessing).

**Session end state (superseded by section 13 below — kept for history,
do not act on the "next steps" that followed it, which are replaced):**
all code changes committed. Board left with A53 Orbtrace service
confirmed responsive (`orbtrace info`); R5-0 left running
`orbtrace_workload` (harmless, JTAG-only boot, no `psu_init` side
effects). No destructive hardware state left behind.

## 13. Phase 6 continued — two real bugs found and fixed using real documentation (2026-08-19, same day)

Section 12 identified the funnel/TPIU addresses as the likely blocker but
had no way to confirm the real values (Xilinx toolchain's bundled SDK data
turned out to be legacy Zynq-7000-era, no ZynqMP CoreSight map). The user
pointed at a private reference-document archive
(`projects/zub_1cg_documentation_private`, sibling to this repo, gitignored
from `zub_1cg` itself — see that archive's own `README.md`/`NOTICE.md` for
provenance and handling terms) that turned out to already have exactly
what was needed: the real `ug1085-zynq-ultrascale-trm.pdf` and
`DDI0500J_cortex_a53_trm.pdf`, pre-extracted to searchable Markdown at
`internal/documentation/pdf/<name>/document.md` inside that archive.

**Bug 1, confirmed and fixed: `coresight::select()`'s funnel/TPIU/ETM base
addresses were missing the CoreSight region's own base address.** UG1085's
real Figure 39-8 ("CoreSight System Debug Address Map", source page 1196)
gives offsets *relative to a 0xFE80_0000 base* (its own text: "CoreSight
components are allocated 8 MB of address space from FE80_0000 to
FEFF_FFFF"), e.g. Funnel 0 = offset `0011_0000`. The addresses in this
codebase since Phase 1 had taken that raw offset and prefixed it with `FE`
directly (`0011_0000` -> `0xfe11_0000`) instead of adding it to the real
base (`0011_0000` -> `0xfe91_0000`) — a transcription bug, not a
documentation error. Fixed in `firmware/a53/src/lib.rs`'s `coresight`
module: added a `CORESIGHT_BASE = 0xfe80_0000` constant, every address now
derived as `CORESIGHT_BASE + <Figure 39-8 offset>`.
**Confirmed fixed via real JTAG readback**, not just plausible-looking
math: with the corrected addresses, `FUNNEL_RPU`'s control register now
holds exactly what `select()` wrote (`0x1`), and — the more decisive
proof — its Peripheral/Component ID registers (architecturally hardwired,
never all-zero on a real component) now read genuine CoreSight signatures:
`CIDR0 = 0x0000000d` (the standard CoreSight preamble byte, part of the
well-known `0xB105_100D` pattern), `PIDR0 = 0x00000008`, `PIDR4 =
0x00000004`. Same confirmation for `FUNNEL_SYSTEM` and `TPIU`. Compare to
section 12's old-address readback: all-zero IDs, or a hard JTAG-DP
STICKYERR for `A53_1_ETM`.

**Bug 2, confirmed and fixed: every ETMv4 register offset in
`enable_trace()` except `TRCOSLAR` was wrong.** DDI0500J (Arm Cortex-A53
MPCore TRM) Chapter 13 "Embedded Trace Macrocell", section 13.8, states
each register's real offset explicitly (e.g. "The TRCPRGCTLR can be
accessed through the external debug interface, offset 0x004"). The
previous session's offsets (recalled from memory of generic ETMv4
reference material, not a real TRM) were consistently wrong:

| Register | Was | Real (DDI0500J 13.8) |
|---|---|---|
| TRCPRGCTLR | 0x000 | 0x004 |
| TRCSTATR | 0x008 | 0x00C |
| TRCCONFIGR | 0x00C | 0x010 |
| TRCEVENTCTL0R | 0x018 | 0x020 |
| TRCEVENTCTL1R | 0x01C | 0x024 |
| TRCSTALLCTLR | 0x020 | 0x02C |
| TRCTSCTLR | 0x024 | 0x030 |
| TRCSYNCPR | 0x028 | 0x034 |
| TRCCCCTLR | 0x02C | 0x038 |
| TRCBBCTLR | 0x030 | 0x03C |
| TRCTRACEIDR | 0x034 | 0x040 |
| TRCVICTLR | 0x040 | 0x080 |
| TRCVIIECTLR | 0x044 | 0x084 |
| TRCVISSCTLR | 0x048 | 0x088 |
| TRCOSLAR | 0x300 | 0x300 ✓ (only one already correct) |

This explains section 12's misleading "confirmation" — writing to offset
`0x000` (real: Reserved) and reading back exactly what was written wasn't
proof of a working `TRCPRGCTLR`, just a Reserved register faithfully
echoing whatever it's told (`TRCPRGCTLR.EN`, the real trace-enable bit at
the real offset `0x004`, was never touched at all). Also dropped
`TRCVIPCSSCTLR` (not a real register in DDI0500J's Table 13-3 — likely
confused with a later ETMv4.x extension not present on this core) and
added the real `TRCAUXCTLR` (offset `0x018`) to the write sequence in its
place, matching the register summary table's actual layout. These are the
ETMv4-architected registers (not Cortex-A53 IMPLEMENTATION DEFINED ones),
so the same offsets apply to the R5-0 ETM too.

**Confirmed fixed via real JTAG readback + register semantics, not just
matching writes:** with corrected offsets, `TRCSTATR` (real offset
`0x00C`) reads `0x00000000` — per DDI0500J Table 13-5, bit[0] `IDLE`: "0 =
The ETM trace unit is not idle" — **meaning the ETM is now genuinely,
actively tracing.** (The other "static config" registers read back
values that don't match what firmware wrote, e.g. `TRCCONFIGR=0x2` for a
written `0`; this is expected and documented, not a bug: `TRCSTATR` bit[1]
`PMSTABLE` also read `0`, and DDI0500J's own text says the "programmers
model is not stable" while running, i.e. these registers legitimately
don't reflect clean static content while `EN=1`.)

**Real hardware capture retried with both fixes: still 0 bytes.**
`rx_bytes=0 dropped_bytes=0 fifo_high_water=0`, `sync_loss` still climbing
into the hundreds of millions. So the ETM is genuinely tracing and the
CoreSight fabric is genuinely routing (both confirmed above), but
something further downstream still isn't delivering usable bytes to the
PL's `coresight_data` input.

**Third real lead investigated, inconclusive:** read the TPIU's own
register state directly (`0xfe98_0000`, offsets reused from
`sdk/bsp/m3/itm.h`'s already-hardware-proven M3 TPIU layout — `SSPSR
0x000, CSPSR 0x004, SPPR 0x0F0, FFSR 0x300, FFCR 0x304, ITCTRL 0xF00`,
the standard ARM CoreSight TPIU macrocell layout, plausible to carry over
since it's the same component family). Findings: `CSPSR=0x9` (bit3 set =
4-bit port, matching `create_bd.tcl`'s `PSU__TRACE__WIDTH {4Bit}` — looks
already correctly configured, likely by hardware default), `SPPR=0x0`
(Parallel mode, correct), `ITCTRL=0x0` (not in integration-test mode,
good) -- but **`FFCR=0x0`** (Formatter and Flush Control, all zero — the
formatter's continuous-output enable is off). Tried a live JTAG write of
`FFCR=0x2` (`EnFCont`, a commonly-cited stable TPIU convention, though
**not** confirmed against a real TPIU-specific TRM the way the two fixes
above were) — **the write stuck, but a re-capture attempt still showed 0
bytes.** Inconclusive: either `FFCR`'s `EnFCont` bit isn't actually at
bit 1 for this specific SoC-400 TPIU instance, or the real blocker is
elsewhere in the chain (a Funnel/Replicator stage between "Funnel 2 /
SYSTEM" and TPIU that Figure 39-8 lists but this code never touches, or a
genuine PL-side issue).

**What's still missing:** UG1085's own citation table (source page 1195)
names the authoritative document for Funnel/TPIU/DAP/Timestamp/CTI
register-level behavior as the **"Arm CoreSight SoC-400 Technical
Reference Manual" [Ref 39]** — a *different* document from both UG1085 and
DDI0500J, and **not present** in the private reference-document archive
checked this session. This is the single most valuable next document to
obtain: it would give the real `TPIU_FFCR`/`FFSR` bit definitions (and
confirm/deny whether a Replicator stage needs separate enabling) instead
of the current best-effort carryover from the M3 macrocell's TRM-verified
layout.

**Session end state:** all code changes (both real fixes) committed. The
one FFCR write this session was a live JTAG probe only, never added to
firmware — nothing to revert. Board left with A53 Orbtrace service
confirmed responsive (`orbtrace info`); R5-0 left running
`orbtrace_workload`. No destructive hardware state left behind.

## Next steps for a future session

1. **Cheapest, do first:** get the real **Arm CoreSight SoC-400 Technical
   Reference Manual** (UG1085's own Ref 39 citation for Funnel/TPIU/DAP/
   Timestamp/CTI) — not currently in the private reference-document
   archive; check there first (same place UG1085/DDI0500J were found this
   session) before searching further afield. This should resolve the
   `TPIU_FFCR` bit-definition gap directly and confirm whether a
   Funnel-2-to-TPIU Replicator stage (visible in Figure 39-8's map but
   never touched by this code) needs its own explicit enable.
2. If that document remains unavailable, the fallback is a real PL-side
   ILA capture on `coresight_data`/`trace_data_m3`-equivalent signals at
   the `orbtrace_pl`/`orbtrace_tpiu_demux` boundary — the same proven
   methodology `M3_TRACE_VERIFICATION_PLAN.md` used repeatedly to
   distinguish "PS-side CoreSight isn't producing anything real" from "PL
   isn't receiving/decoding what's genuinely being sent." This session's
   findings (ETM genuinely tracing, funnels genuinely routing) make "PS
   config is still incomplete" the leading hypothesis over a PL/wiring
   bug, but an ILA capture would settle it definitively either way.
3. Once bytes start flowing, mirror `M3_TRACE_VERIFICATION_PLAN.md`'s
   Phase E/F methodology exactly: confirm real content recovery against
   `applications/orbtrace/firmware/rpu`'s known-reproducible `Workload`
   sequence before trusting anything downstream (this project has been
   burned before by declaring victory on `rx_bytes > 0` alone — see that
   document's own Phase E cautionary history).
4. Before investing further in Phase 6-8's ambition level, it's worth
   doing the real UG1085/Cortex-A53 TRM check flagged in section 3, point
   5 — if this silicon's ETM genuinely has address/data comparators or an
   ETR path, that changes how ambitious those phases are worth being from
   the start. (Now easy to combine with item 1 — both documents are
   findable via the same private archive this session used.)
5. Everything from Phase 6 onward is new engineering, not just wiring —
   size it accordingly before starting, the same way
   `M3_TRACE_VERIFICATION_PLAN.md` Phase H sized the ETM-on-M3 path
   before recommending against starting it without a clear bandwidth case.
