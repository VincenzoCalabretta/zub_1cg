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
| 6 | First real hardware capture on the R5/A53 path, verify PS/ETM sync (mirrors M3 Phase E/F) | **STARTED, 2026-08-20 (continued yet further still) — R5-0 genuinely boots and runs ThreadX (real, repeated UART evidence). A real power cycle ruled out "stale JTAG debug state." A clean disable A/B test proved TTC0 *is* causally necessary for the IRQ exceptions; `GICD_ICFGR` confirmed its SPI is level-sensitive, ruling out edge-triggered-narrow-pulse. A follow-up A/B test (IER enabled, counter statically disabled) proved IER alone is insufficient — the counter must genuinely be running. `TTC_ISR`/`TTC_COUNT_VAL` now read from *inside* `IRQHandler()` itself (earliest possible vantage point, not slow external JTAG): `TTC_ISR` still reads `0` every time, `TTC_COUNT_VAL` reads an identical small constant (13) every time. A wall-clock-bracketed measurement confirms the entry rate is genuinely ~105 Hz, not a sampling artifact. Four specific, TRM-motivated hypotheses for the persistent CoreSight-examine `JTAG-DP STICKY ERROR` (JTAG clock speed, LPD debug clock gating, RPU debug-logic reset, AP index/type) have all been tried and ruled out; that failure remains unexplained. Net position: a real, counter-driven hardware event reproducibly and immediately fires a genuine ARM IRQ into `IRQHandler()`, yet every status register this investigation can reach — including an in-ISR `TTC_ISR` read — agrees, every time, that nothing is outstanding. CoreSight/funnel/TPIU wiring itself is still believed fine — the idle-pattern captures are still best explained by some stalled/spinning CPU state once this tick bug stalls a thread.** See sections 20-26 |
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

**Session end state (superseded by section 14 below — a materially bigger
finding landed the same day; kept for history):** all code changes (both
real fixes) committed. The one FFCR write this session was a live JTAG
probe only, never added to firmware — nothing to revert. Board left with
A53 Orbtrace service confirmed responsive (`orbtrace info`); R5-0 left
running `orbtrace_workload`. No destructive hardware state left behind.

## 14. Phase 6 continued — real bytes reach the PL for the first time (2026-08-19, same day)

Section 13's fixes (real funnel/ETM addresses, real ETMv4 offsets)
confirmed the ETM genuinely tracing and the funnels genuinely routing, but
a real capture retry still showed 0 bytes. This section chases that down
to its actual root cause — a **third, structurally different kind of bug**
from the first two: not a wrong constant in this repo's code, but **this
entire investigation (this session and, per [[board_bitstream_state]],
likely every prior Orbtrace hardware session) has been flashing the board
with the wrong `psu_init.tcl`.**

**The `sync_loss` metric was a red herring, worth flagging clearly so a
future session doesn't repeat the mistake:** `orbtrace_pl.v` wires the
`stats` command's `sync_loss` field to `tpiu_sync_loss + nrz_malformed +
manchester_malformed` — the SWO NRZ/Manchester decoders run
*unconditionally*, regardless of the configured trace format, continuously
sampling `trace_data[0]`. In Parallel/TPIU4 mode (what this whole
investigation uses), those decoders are decoding meaningless noise the
whole time and dominate the reported number. The huge, climbing
`sync_loss` values seen throughout sections 12-13 were **not** evidence of
real PS-side activity — the actually-informative signals are `rx_bytes`
and `fifo_high_water`, both of which stayed at exactly `0` through every
test in sections 12-13.

**Investigated three more hypotheses in order, ruling out two and
confirming the third:**
1. **FPD debug power domain gating — ruled out.** Table 39-12 (UG1085)
   places all trace-related components (Funnels 1/2, TMC, TPIU) in the
   FPD, requiring a `CSYSPWRUPREQ`/`CSYSPWRUPACK` handshake through the
   JTAG-DP's own `CTRL/STAT` register before FPD debug logic is genuinely
   powered. Read `CTRL/STAT` directly via `openocd`'s built-in `dap dpreg
   0x4` command (much safer than hand-rolling raw ADIv5 DPACC shifts,
   which this session tried once and got nonsense back): `0xf0000001` —
   `CSYSPWRUPACK` (bit31) and `CDBGPWRUPACK` (bit29) both already `1`.
   Already satisfied by `aes_zub.cfg`'s existing setup event; not the
   blocker.
2. **TPIU formatter config (`CSPSR`/`SPPR`/`FFCR`) — tried, not
   sufficient alone, later found to be a red herring for a different
   reason (see below).** Read TPIU's real register state
   (`0xfe98_0000`, offsets reused from `sdk/bsp/m3/itm.h`'s
   hardware-proven M3 TPIU layout — same ARM CoreSight TPIU component
   family): `SPPR=0` (parallel, correct), `CSPSR=0x9` (bits 0 and 3 both
   set — plausibly wrong for a one-hot selector), `FFCR=0` (formatter
   continuous-output disabled). Live JTAG writes of `CSPSR=0x8` (pure
   4-bit) and `FFCR=0x2` (`EnFCont`) both stuck but didn't unblock
   anything by themselves.
3. **The PS trace debug clock was never enabled at all — confirmed real,
   the actual first domino.** UG1085 Table 37-7 documents
   `CRF_APB.DBG_TRACE_CTRL`'s reset value as `0000_2500h` with
   `[CLKACT] = Clock stop`. Read it live: **`0x00002500`** — exactly the
   documented reset value. Compare `DBG_FPD_CTRL` (used for CoreSight
   *register* access): `0x01000200`, `CLKACT` bit set — explaining why
   every earlier register read/write against Funnel/TPIU/ETM "worked" at
   the APB level while the TPIU's actual DDR output serializer (which
   specifically needs `DBG_TRACE_CLK`, a separate clock) never ran.
   Confirmed the generic board-level
   `sdk/boards/zub_1cg/generated/psu_init.tcl` this whole investigation
   has used (a known gap already flagged in [[board_bitstream_state]] for
   AXI HPM/fabric-width registers — trace clock is the same class of gap)
   has **zero** references to `DBG_TRACE_CTRL` — `grep -c` on the literal
   string returns `0`. `create_bd.tcl` sets `PSU__TRACE__PERIPHERAL__ENABLE
   {1}` / `PSU__TRACE__PERIPHERAL__IO {EMIO}` / `PSU__TRACE__WIDTH {4Bit}`
   directly on the PS block — real PS-configuration properties the generic
   board-level design never had reason to set.

**The real fix, and it's not a hand-patch:** `applications/orbtrace/vivado/build.tcl`
already exports Orbtrace's *own* `psu_init.tcl` alongside every bitstream
build, via `write_hw_platform` + `export_psu_init.tcl` (see that file's
own tail). It was sitting unused, right next to the bitstream, in every
cached build this whole investigation has flashed from
(`bazel-out/orbtrace-vivado-m3-10mhz-r4/psu_init.tcl` for the specific
build used this session) — this repo's own documented hardware workflow
(`AGENTS.md`) always pairs the Orbtrace bitstream with the *generic*
board-level `psu_init.tcl` instead, a choice whose own justifying comment
in [[board_bitstream_state]] only ever audited it for DDR/AXI-HPM
correctness, not trace. Confirmed the real, Orbtrace-specific
`psu_init.tcl` genuinely programs `DBG_TRACE_CTRL` correctly:
`mask_write 0XFD1A0064 0x01003F07 0x01000500` — `CLKACT=1` **and**
`DIVISOR0=5`, a materially different (and presumably correctly-tuned for
the real 100 MHz target) value from the `DIVISOR0=37` reset default this
session's own hand-patch had left unchanged (only setting `CLKACT`, not
the divisor) — explaining why the hand-patch alone hadn't been enough.

**Reflashed A53 using this real `psu_init.tcl` for the first time.** No
DDR hang (the historical reason this was avoided is fixed as of the
2026-08-09 `zub1cg_apply_ps_preset` fix, already baked into the current
`create_bd.tcl`/this build). `orbtrace info` confirmed responsive
immediately after. Bonus confirmation this was the right document: `TPIU
CSPSR` now reads `0x8` (pure 4-bit) **on its own**, without this session's
earlier hand-patch — the real `psu_init.tcl` already configures it
correctly, unprompted.

**Result: real bytes reach the PL capture FIFO for the first time in this
entire investigation.** Re-ran the exact same `configure r5 tpiu4` →
`start` → check sequence: `fifo_high_water=63` (pegged at its max depth —
the same "real overflow, not idle" signature
`M3_TRACE_VERIFICATION_PLAN.md`'s own Phase E first hit early in that
investigation). `rx_bytes` still `0` and stayed at `0` through a real
`orbtrace capture` attempt (the TCP connection itself worked fine this
time — no hang) — `fifo_high_water` staying pegged rather than draining
even with a live client connected points at the **TPIU demux never
achieving `synced`**, not a downstream TCP/DMA issue. Given
`orbtrace_tpiu_demux.sv`'s sync search looks for a generic CoreSight TPIU
Full Sync Packet (source-agnostic framing, should apply to ETMv4 content
the same as it did to M3's ITM content), and `M3_TRACE_VERIFICATION_PLAN.md`
independently root-caused an extremely similar-shaped M3 symptom to "the
TPIU formatter's Full Sync Packet requires an explicit trigger, it's not
periodic/automatic" (TRM §11.2.2, for the M3's own local TPIU macrocell) —
the leading hypothesis is that the **system TPIU (SoC-400 macrocell) needs
some additional trigger/flush beyond `FFCR.EnFCont` to actually emit a
Full Sync Packet**, and the still-missing Arm CoreSight SoC-400 TRM (see
section 13) is very likely exactly where that's documented.

**Session end state:** no new firmware/code changes this section — every
fix here was either a live JTAG register probe (CSPSR/FFCR, harmless,
firmware still doesn't set these) or a **flash-time tooling choice**
(which `psu_init.tcl` to pass to `jtag_flash.sh`), not a code change to
commit. Board left flashed with the real Orbtrace `psu_init.tcl` +
current `a53_app`; R5-0 running `orbtrace_workload`; `orbtrace info`
confirmed responsive. **This flash-time choice does not survive a future
`jtag_flash.sh` call that reverts to the generic board-level
`psu_init.tcl`** — a future session must deliberately keep using the
Orbtrace-specific one (see next steps below) or this exact regression
will silently reappear.

## 15. Phase 6 continued — `TRCAUXCTLR.SYNCDELAY` fix applied, JTAG readback hits real reliability limits (2026-08-19, same day)

With section 14's `psu_init.tcl` fix landing real bytes in the PL FIFO
(`fifo_high_water=63`, pegged at max) but `rx_bytes` staying `0`, the
question became: why does `orbtrace_tpiu_demux.sv` never find a Full Sync
Packet in a byte stream that's evidently real and flowing?

**Found a genuinely strong, TRM-confirmed candidate: `TRCAUXCTLR` bit[3]
`SYNCDELAY`.** DDI0500J Table 13-8: "Delay periodic synchronization if
FIFO is more than half-full... `0` = SYNC packets are inserted into FIFO
only when trace activity is LOW... `1` = SYNC packets are inserted into
FIFO irrespective of trace activity." This project's own deterministic
workloads (`applications/orbtrace/firmware/rpu`'s `Workload`, and
`m3_app`'s equivalent) are designed to run continuously, essentially never
idling at the timescale this bit cares about — under the default
(`SYNCDELAY=0`), the ETM's `TRCSYNCPR`-driven periodic sync packet (the
exact byte-level construct `orbtrace_tpiu_demux.sv` searches for) could be
deferred forever, which would exactly explain "real trace bytes flowing,
FIFO backed up, never a sync lock."

**Implemented in firmware, not just tried ad hoc:** added
`coresight::AUXCTLR_SYNCDELAY = 1 << 3` and wired it into `enable_trace()`
(`mmio.write32(etm + TRCAUXCTLR, AUXCTLR_SYNCDELAY)`, in the existing
disable→configure→enable sequence — `TRCAUXCTLR`, like `TRCCONFIGR`, "only
accepts writes when the trace unit is disabled" per DDI0500J, and
`enable_trace()` already writes `TRCPRGCTLR=0` before any static config
and `TRCPRGCTLR=EN` last, so the ordering is correct by construction).
Extended the existing `enable_trace_starts_the_selected_etm_with_a_unique_trace_id`
host test to assert this write. Host tests pass, real aarch64 cross-build
succeeds. Reflashed A53 (using the Orbtrace-specific `psu_init.tcl` from
section 14, not the generic one) and re-ran R5-0's boot + `configure`/
`start`/`capture` sequence.

**Result: still `fifo_high_water=63`, `rx_bytes=0`.** Not conclusively
disproven, though — see the reliability caveat below.

**A real methodological problem surfaced while trying to verify this via
JTAG readback, worth recording so a future session doesn't repeat the
mistake:** `TRCSTATR` bit[1] `PMSTABLE` ("indicates whether the ETM trace
unit registers are stable and can be read... 0 = The programmers model is
not stable") reads `0` throughout active tracing (`TRCSTATR=0x0`,
`IDLE=0` too — genuinely tracing). This means **every JTAG readback of
`TRCCONFIGR`/`TRCAUXCTLR`/`TRCVICTLR`/`TRCTRACEIDR`/`TRCPRGCTLR` taken
while the ETM is actively running is architecturally unreliable** — this
session's own attempt to verify the `SYNCDELAY` write via a live re-read
got back `TRCAUXCTLR=0x00800000` (not the `0x8` firmware wrote) and
`TRCPRGCTLR=0x8d014024` (the exact same value seen in an *earlier*,
unrelated readback in section 14 — too consistent to be live noise, more
likely some other JTAG/APB artifact not yet understood). Notably, section
13/14's own "confirmed via readback" claims for `TRCTRACEIDR`/`TRCVICTLR`
earlier in this document were ALSO taken while actively tracing and got
clean-looking values that time — the inconsistency between "clean
readback" and "garbage readback" across sessions is itself unexplained.
**Net effect: JTAG readback can no longer be trusted as a verification
tool for ETM static config while tracing is active — a real limitation of
this session's own methodology, not a new hardware finding.** The
firmware-level fix should be trusted based on correct write ordering
(host-tested) rather than a post-hoc external read.

**Session end state (superseded by section 16 below — kept for history):**
`AUXCTLR_SYNCDELAY` change committed (real, TRM-justified, worth keeping
regardless of whether it alone is sufficient). Board left flashed with
this firmware + the Orbtrace-specific `psu_init.tcl`; R5-0 running
`orbtrace_workload`; `orbtrace info` confirmed responsive.

## 16. Phase 6 continued — real ILA capture, decisive: no genuine ETM content ever leaves the PS (2026-08-20)

Section 15 recommended a real PL-side ILA capture over further JTAG
register probing, since `TRCSTATR.PMSTABLE=0` makes live APB reads of ETM
config registers architecturally unreliable while tracing. Did exactly
that.

**Built a diagnostic bitstream** (`bazel-out/orbtrace-vivado-ps-etm-ila-debug`,
~17 minutes with the cached M3 EDIF — see below for tooling): a
`system_ila` in NATIVE mode, clocked directly on `trace_clk` (i.e.
`ps/trace_clk_out`, the PS trace port's own real clock, not `aclk` — one
sample per real byte, no async-oversampling ambiguity, mirroring
`M3_TRACE_VERIFICATION_PLAN.md`'s own proven preference for native-clock
capture over its own earlier aclk-domain attempts). Four probes, all
temporary combinational passthroughs added to `orbtrace_pl.v` (reverted
after use, `git diff` clean): `dbg_trace_data_raw` (raw `trace_data[3:0]`
pins, sampled once per `trace_clk` rising edge — inherently only sees half
of what feeds the DDR reconstruction, which samples both edges, but still
useful as a coarse check), `dbg_trace_byte`/`dbg_trace_valid` (the
`orbtrace_ddr_capture` instance's own reconstructed byte output, the real
signal of interest), `dbg_cdc_write_ready` (whether the CDC FIFO's write
side is backpressured).

**Tooling notes for next time (session scratch, not committed — recreate
from this description, per `M3_TRACE_VERIFICATION_PLAN.md`'s own
established convention):**
- `create_bd_debug.tcl` = `source` the real `create_bd.tcl` verbatim, then
  append one `system_ila` (`xilinx.com:ip:system_ila:1.1`, `C_MON_TYPE
  {NATIVE}`, 4 probes matching the widths above, `C_DATA_DEPTH {4096}`),
  wired via plain `connect_bd_net` calls (`debug_ila/clk` ←
  `ps/trace_clk_out`; each probe ← the matching `trace_pl/dbg_*` pin). A
  cheap `validate_bd_design`/`generate_target` sanity pass (~1 minute, no
  synthesis) caught nothing wrong — the real build didn't need a second
  attempt.
- `build_debug.tcl` = `build.tcl` with three changes: source
  `create_bd_debug.tcl` instead of `create_bd.tcl`; default P&R directives
  instead of `AggressiveExplore` (this throwaway bitstream doesn't need
  timing-closure effort); the CDC/methodology/timing `error` gates
  downgraded to non-fatal `puts` (this build isn't meant to be a
  production artifact) — same relaxed-gate pattern
  `M3_TRACE_VERIFICATION_PLAN.md` used for its own diagnostic builds.
  `M3_OOC_EDIF=bazel-out/m3-ooc-2019/m3_core.edf` (the cached pre-2019.1
  M3 IP netlist, found via `find ~/.cache/bazel -iname "*.edf"` — real,
  present, contrary to section 12's earlier "none found" note, which
  evidently didn't search broadly enough) cut the build to ~17 minutes.
  Result came back genuinely clean anyway: positive setup/hold slack
  (1.805ns / 0.010ns), only the same known M3-related methodology
  warnings this design always has.
- Flashed via the normal `jtag_flash.sh` flow (never a raw
  `program_hw_devices` reprogram), paired with *this build's own* exported
  `psu_init.tcl` (same section-14 lesson: always use the build-specific
  one).
- Arming/capture via Vivado Hardware Manager Tcl batch mode:
  `open_hw_manager`/`connect_hw_server`, **`set_property PARAM.FREQUENCY
  1000000 [get_hw_targets]` before `open_hw_target`** (the
  `M3_TRACE_VERIFICATION_PLAN.md`-documented fix for slow-clock JTAG scan
  flakiness — default TCK violates the debug hub's `TCK ≤
  clock/2.5` minimum ratio), then `PROBES.FILE` set on the `hw_device`
  *before* `refresh_hw_device` (order matters). Triggered on
  `dbg_trace_valid==1`; triggered essentially instantly, since the signal
  had already been continuously asserted (matches `fifo_high_water`
  staying pegged). `upload_hw_ila_data` + `write_hw_ila_data -csv_file`
  for offline analysis — much easier to `awk`/`sort`/`uniq -c` a CSV than
  to read a GUI waveform.

**The capture is decisive, and overturns the section 14/15 framing.**
4096 real samples, exactly **two** distinct byte values in
`dbg_trace_byte`: `0xFF` (3072 samples, 75%) and `0xDF` (1024 samples,
25%), in a perfectly regular period-4 pattern (`FF FF FF DF` repeating,
without exception, for the entire ~40µs window).
`dbg_trace_data_raw` read a constant `0xF` throughout (consistent with
this being only a single-edge sample of a signal whose real variation
happens to fall on the un-sampled edge — not itself informative beyond
"nothing is obviously stuck at 0"). `dbg_cdc_write_ready` stayed `1`
throughout this window.

**This is a clean idle-pattern signature, not genuine ETM trace content.**
Real instruction/branch trace, even for a small deterministic workload,
would show far higher byte-value entropy than two values in a fixed
period-4 cycle. **Conclusion: the PS trace port's physical output is
alive and toggling (explaining `fifo_high_water>0` — the PL correctly
captures *something* continuously), but no genuine ETM ATB traffic is
reaching it at all** — despite `TRCSTATR` showing `IDLE=0` (the ETM
believes it is not idle) in every JTAG readback taken this investigation.
This makes section 14/15's entire "does the TPIU ever emit a Full Sync
Packet" framing **premature** — there is no real trace content for it to
frame *from* in the first place. `TRCAUXCTLR.SYNCDELAY` (section 15) may
still be correct/worth keeping, but it was answering the wrong question.

**Real, live power-domain check ruled out one candidate cause:** verified
via `dap dpreg 0x4` (already done in section 15) that `CSYSPWRUPACK`/
`CDBGPWRUPACK` are asserted — the FPD debug power domain is genuinely up,
so power gating on the funnel/TPIU/ETM's *register* access isn't the
explanation (consistent with every register read/write "working" all
along). Checked `TRCPDCR`/`TRCPDSR` (DDI0500J 13.8.42/13.8.43, offsets
`0x310`/`0x314`) — `TRCPDSR.POWER` must already be `1` for any of this
investigation's register reads to have succeeded at all (`POWER=0` means
"registers not accessible, error response"), so the ETM's own trace-unit
power state isn't gating this either. Neither of these register-level
theories explains a "believes it's tracing, produces nothing" ETM.

**What remains unexplained, and is now the real open question:** why does
genuine ATB traffic never leave the ETM (or never survive to the funnel/
TPIU) despite every register this investigation has checked reading back
consistent with "should be tracing"? Two live hypotheses, neither
confirmed:
1. A CoreSight Cross-Trigger Interface (CTI) "start trace" event is
   required at the system level, distinct from any single component's own
   enable bits — UG1085's Table 39-12 lists CTI 0/1/2 as real,
   present components in this exact SoC, never touched by this
   investigation at all. Many ARM reference CoreSight designs use CTI
   channels to synchronize a trace-start pulse across multiple components;
   if this SoC's funnel/ETM path structurally depends on one, no amount of
   individual-register configuration would substitute for it.
2. Some other funnel- or ATB-handshake-level configuration this
   investigation hasn't found yet — the Arm CoreSight SoC-400 TRM (still
   missing, see section 13) is the authoritative reference for exactly
   this class of question (funnel behavior beyond the simple port-enable
   bitmask already confirmed working via real Peripheral ID readback).

**Session end state:** all temporary RTL debug ports reverted (`git diff`
clean on `orbtrace_pl.v`). Board reflashed back to the last known-good
*production* bitstream (`bazel-out/orbtrace-vivado-m3-10mhz-r4`) + its own
correct `psu_init.tcl` + current `a53_app`; `orbtrace info` confirmed
responsive. Diagnostic ILA bitstream still cached at
`bazel-out/orbtrace-vivado-ps-etm-ila-debug` (bitstream + `.ltx` probes
file) for a future session to re-arm/re-capture without another ~17-minute
rebuild, if useful (e.g. probing CTI-adjacent signals instead, or the
funnel's own ATB input/output directly, once temporary debug ports for
those are threaded through). No destructive hardware state left behind.

## 17. Phase 6 continued — CTI hypothesis tested live on hardware, ruled out (2026-08-20)

Picked up section 16's item 1 (cheapest next step: read the CTI
components' state via JTAG). Board had been left powered off/idle since
section 16; reflashed with the same known-good production bitstream
(`bazel-out/orbtrace-vivado-m3-10mhz-r4`) + its own paired `psu_init.tcl`
(both still cached, no rebuild needed) — `orbtrace info` confirmed
responsive immediately after, same recovery flow as every prior session.

**Read the real UG1085 CTI sections closely first (Chapter 39, "Embedded
Cross Trigger" + Table 39-8 "CTI Connections"), not previously done in
this investigation.** Two concrete, useful findings not in section 16's
framing:

1. **Table 39-8 explicitly wires system CTI 0 (`CORESIGHT_SOC_CTI_0`,
   `CORESIGHT_BASE + 0x0019_0000`) to the TPIU's `FLUSHIN` (trigger output
   6) and `TRIGIN` (trigger output 7)** — directly relevant to section
   14's still-open "does the TPIU need an explicit trigger beyond
   `FFCR.EnFCont`" question. CTI 0's *inputs* are ETF/ETR `FULL`/`ACQCOMP`
   signals — components this Orbtrace design never configures (it bypasses
   ETF/ETR/TMC entirely, straight from Funnel 2 to TPIU), so CTI 0's
   *inputs* being unconfigured is expected, not a bug.
2. **The per-core R5-0/R5-1 CTI (`CORESIGHT_BASE + 0x003F_8000`) is a
   structurally different component** — its Table 39-8 connections are
   `DBGTRIGGER`/`PMUIRQ`/`ETM EXTOUT[0:1]`/`COMMRX`/`COMMTX`/`ETM
   TRIGGER`/`EDBGRQ`/`ETM EXTIN[0:1]`/`DBGRESTART` — nothing about
   starting/flushing trace output. The system CTI 0, not the per-core CTI,
   is the one architecturally tied to the TPIU.

**Read CTI 0/1/2's real register state via JTAG** (`uscale.axi` mem_ap,
same ad hoc `read_memory`/`mww` technique as every prior register probe
this investigation has used — pure read-only, no R5/A53 CoreSight
examine, confirmed safe to run against a live A53 network session):
all three are genuine, correctly-addressed CoreSight components (real
Peripheral/Component ID signatures, `DEVTYPE=0x14` — the standard
CoreSight "Trigger, Cross Trigger" encoding, corroborating the address
math independent of the ID-register check already used for funnel/TPIU
in section 13). But **`CTICONTROL=0` (globally disabled) and every
`CTIINEN`/`CTIOUTEN` register reads `0`** on all three — no channel
routing has ever been configured, by any part of this system (not this
firmware, not `psu_init`, not any Xilinx default). `CTIGATE=0xF` (all 4
channels ungated — an unremarkable reset default, not itself evidence of
anything).

**Live-tested the hypothesis directly, not just inferred it from the
register dump.** Booted R5-0 with `orbtrace_workload` via the known-safe
JTAG-only flow (`load_r5.tcl`, omitting `psu_init_run.tcl` — confirmed
again this session not to disturb the A53 network). Ran `configure r5
tpiu4 2000000` → `start`, confirmed the section 14/15 baseline reproduces
exactly (`fifo_high_water=63` pegged, `rx_bytes=0`). Then, via a second,
separate JTAG connection (the first `load_r5.tcl` invocation exits after
boot, freeing the bus): unlocked CTI 0 (`LAR`), set `CTIOUTEN6=1` and
`CTIOUTEN7=1` (channel 0 → both `FLUSHIN` and `TRIGIN`), set
`CTICONTROL=1` (global enable), then pulsed channel 0 via `CTIAPPPULSE`
— both a single pulse and, in case a one-shot pulse arriving after the
FIFO was already backed up wasn't enough, a 50-pulse burst spaced ~50
JTAG clocks apart. All writes genuinely stuck (read back what was
written, confirming CTI 0 is real and controllable). **Result: no change
whatsoever.** `rx_bytes` stayed exactly `0` and `fifo_high_water` stayed
pegged at exactly `63` before, between, and after every pulse attempt —
confirmed via a real TCP round-trip each time (`orbtrace stats`'s
`sync_loss`/`rx_bytes`/etc. are decoded live from each response, not
client-cached — checked the model's own wire-decode code to be sure
before trusting a negative result).

**Conclusion: the CTI 0 → TPIU FLUSHIN/TRIGIN trigger, at least with this
channel/output mapping, is not the missing piece.** This doesn't prove
CTI involvement is impossible (a different channel, a different pulse
timing relative to ETM start, or a requirement this investigation hasn't
found without the SoC-400 TRM could still be real), but a direct,
repeated, real-hardware test of the most natural reading of Table 39-8's
own wiring diagram produced a clean negative — worth not re-trying the
same shape of fix again without new information.

**One unexplained anomaly noticed, not chased further this session:**
`sync_loss` read back bit-for-bit identical (`562425449`) across every
`stats` call this session, including across a `stop`/`start` cycle —
contradicting section 12/14's description of it "climbing into the
hundreds of millions" continuously. Confirmed this isn't a CLI-side
caching artifact (real TCP round-trip, decoded fresh each time). Could be
a saturated/latched sub-counter within the `tpiu_sync_loss +
nrz_malformed + manchester_malformed` sum, or something genuinely
different about this session's exact state — not resolved, flagged for
whoever picks this up next. Doesn't undermine the CTI test's negative
result: `rx_bytes` and `fifo_high_water` (the two metrics section 14
already established as the trustworthy ones, specifically because
`sync_loss` is noisy/misleading in TPIU4 mode) were unambiguous and
consistent throughout.

**Session end state:** no firmware/code changes — every action this
session was either a flash-time tooling choice (same production
bitstream + its own `psu_init.tcl`, both cached) or a live JTAG register
probe (CTI 0 unlock/configure/pulse — read-only intent, no persistent
hardware state; a CTI's enable bits don't survive a power cycle or
reflash regardless). Board left flashed with the production Orbtrace
bitstream + `a53_app`; R5-0 left running `orbtrace_workload` harmlessly
(same JTAG-only boot as every prior session, no `psu_init_run.tcl` side
effects); `orbtrace info` confirmed responsive. No destructive hardware
state left behind.

## 18. Phase 6 continued — ILA re-arm + CTI pulse attempted, blocked on tooling, not completed (2026-08-20)

Attempted section 17's own recommended next step (item 2): re-arm the
cached diagnostic ILA bitstream (`bazel-out/orbtrace-vivado-ps-etm-ila-debug`)
while live-pulsing CTI 0, to see whether the raw `dbg_trace_byte` pattern
changes at all. **Did not get as far as a capture — hit a real tooling
conflict before touching the board, worth recording so the next attempt
doesn't repeat it.**

**The problem:** section 17's CTI pulse used OpenOCD (`tooling/openocd/aes_zub.cfg`),
which opens the board's FTDI FT2232H JTAG adapter directly via its own
`ftdi` driver (libusb, exclusive access) — this is *separate* from, and
incompatible with, Vivado Hardware Manager's route to the same physical
adapter (`open_hw_manager`/`connect_hw_server`/`open_hw_target`, the
mechanism section 16's ILA capture used). Both want exclusive ownership
of the one USB JTAG cable; they cannot run concurrently. Pulsing CTI 0
*during* a live ILA capture — the whole point of this check — needs both
at once.

**The fix, not yet executed:** don't use OpenOCD for the CTI pulse in
this combined test. `jtag_flash.sh` and `psu_init_run.tcl` already prove
`xsct` can do raw PS memory reads/writes (`mwr`/`mrd`, via the same ARM
DAP, targeting `APU*`/`PSU` in xsct's `targets -set` selection) through
Xilinx's own `hw_server` — and `hw_server` is designed for exactly this,
multiple simultaneous TCP clients (xsct *and* Vivado Hardware Manager)
against one JTAG session. So the real sequence for next time: launch one
persistent `hw_server`, connect Vivado Hardware Manager to it for the ILA
arm/capture, and connect `xsct` to the *same* `hw_server` for the CTI 0
`mwr` pulse sequence (translating section 17's register writes to `mwr`
calls) — never OpenOCD in this specific combined test.

**Where this attempt actually stopped:** tried to launch `hw_server`
directly (`~/opt/vitis/Vivado/2023.2/bin/hw_server`) to have a persistent
instance both clients could reach — it isn't directly executable outside
the FHS-wrapped environment `nix develop` provides for `xsct`/`vivado`
(same reason raw `vivado`/`xsct` binaries aren't directly executable
either — see [[hardware-test-environment]]). `flake.nix`'s `mkXilinxTool`
wraps `xsct`/`vivado`/`openocd`/`picocom` for the devShell but not
`hw_server` as its own top-level tool. Didn't chase a workaround this
session (e.g. `nix develop -c env XILINX_ROOT=... vivado -mode tcl -source
<script that just does connect_hw_server>` to get a wrapped `hw_server`
spawned and left running, then pointing `xsct`'s own `connect` at that
same port instead of autostarting its own) — that's the concrete next
thing to try.

**Session end state: no hardware touched at all this session** — the
board is in exactly the state section 17 left it (production Orbtrace
bitstream + `a53_app`, R5-0 running `orbtrace_workload`, last confirmed
responsive via `orbtrace info` at the end of section 17). No code
changes. This section is scratch/tooling notes only.

## 19. Phase 6 continued — combined ILA + CTI test finally run, but R5's own boot health now in doubt (2026-08-20)

Picked up section 18's blocker directly: get a persistent `hw_server`
that both Vivado Hardware Manager and `xsct` can connect to at once, so
the CTI-0-pulse-while-ILA-armed test section 17 recommended could
actually run.

**Fixed the tooling gap for good, not just worked around it this
session.** Section 18 found `flake.nix`'s `mkXilinxTool` wraps
`vivado`/`xsct`/`xelab`/`xsim`/`xvlog` for the FHS sandbox but not
`hw_server`, and that raw `~/opt/vitis/.../hw_server` isn't directly
executable outside that sandbox (same FHS-runtime-library reason
`vivado`/`xsct` themselves aren't). Added `hw_server = mkTool "hw_server"`
to both the `devShells.default` tool list and its `packages` list in
`mkDevShell` (`flake.nix`, two one-line additions, same pattern as every
other wrapped tool) — `nix develop -c hw_server -s tcp::3121` now just
works. Confirmed the FHS sandbox (`buildFHSEnv`/bubblewrap) does **not**
isolate the network namespace: a `hw_server` launched this way binds
`127.0.0.1:3121` and is reachable from plain host-namespace processes
(`bash -c '(exec 3<>/dev/tcp/127.0.0.1/3121)'` succeeded) — this is what
makes the whole multi-client scheme work at all.

**Proved Vivado Hardware Manager and `xsct` can genuinely share one
`hw_server` instance concurrently, not just sequentially.** Ran a
persistent `hw_server -s tcp::3121` in the background; pointed a separate
`xsct` process at `-url tcp:127.0.0.1:3121` and confirmed real target
enumeration (`PS TAP`/`PMU`/`PL`/`PSU`/`RPU` with `Cortex-R5 #0
(Running)`/`Cortex-R5 #1 (Reset)`, `APU` with `Cortex-A53 #0
(Running)`/`Cortex-A53 #1 (Power On Reset)` — a real, live JTAG scan, not
cached state); separately ran a Vivado batch-mode Tcl script doing
`connect_hw_server -url localhost:3121` against the same instance and
successfully found/armed the diagnostic build's `system_ila`. Both
clients worked against the same `hw_server` in the same test run (`xsct`
pulsing CTI 0 while a *separate* Vivado process re-armed and captured the
ILA moments later) with no contention errors from either side — the
combined test section 18 couldn't even attempt is now real, reusable
infrastructure.

**Re-ran the section 16 diagnostic bitstream build fresh** (rebuilt
current-HEAD `a53_app` — includes every fix through section 18, notably
`TRCAUXCTLR.SYNCDELAY` from section 15 — and reflashed
`bazel-out/orbtrace-vivado-ps-etm-ila-debug`'s cached bitstream + its own
paired `psu_init.tcl`; `orbtrace info` confirmed responsive). Booted R5-0
with `orbtrace_workload` via the known-safe JTAG-only flow (`load_r5.tcl`,
omitting `psu_init_run.tcl`). Ran `configure r5 tpiu4 2000000` → `start`;
reproduced the section 14/15/17 baseline exactly (`fifo_high_water=63`
pegged, `rx_bytes=0`).

**Ran the actual combined test — but re-scoped the synchronization
problem section 18 was stuck on, since it turned out to not matter.**
Section 18 was trying to figure out how to arm the ILA and pulse CTI
*at the same instant* to catch the transition live, inside a single
~41µs capture window (4096 samples at the 100 MHz `trace_clk`) — a hard
synchronization problem against a JTAG-scripted pulse whose own round-trip
latency is orders of magnitude longer than that window. Realized this
isn't actually necessary: if CTI-gated trace start were real, pulsing it
once should durably switch the system from "idle pattern forever" to
"real trace forever" (or at least visibly perturb it), not require
catching a single instant. So instead: captured a baseline ILA snapshot,
pulsed CTI 0 (unlock LAR, `CTIOUTEN6=1`/`CTIOUTEN7=1`, `CTICONTROL=1`,
same channel-0-to-`FLUSHIN`/`TRIGIN` mapping section 17 used, then a
50-pulse burst on `CTIAPPPULSE` — all via `xsct mwr` through the shared
`hw_server`, not OpenOCD), then captured a second ILA snapshot, and
compared.

Reusable scripts (session scratch, not committed — recreate from this
description, per this document's established convention):
- `ila_capture.tcl <outfile.csv> <label>`: `open_hw_manager`;
  `connect_hw_server -url localhost:3121`; the same `PARAM.FREQUENCY
  1000000` pre-`open_hw_target` fix section 16 documented; sets
  `PROBES.FILE`/`FULL_PROBES.FILE` from the diagnostic build's
  `debug_nets.ltx`; `refresh_hw_device`; arms on
  `[lindex [get_hw_probes -of_objects $ila] 2]` (probe index, not name —
  see gotcha below) `TRIGGER_COMPARE_VALUE eq1'b1`; `run_hw_ila` /
  `wait_on_hw_ila -timeout 60`; `upload_hw_ila_data`; `write_hw_ila_data
  -csv_file`.
- `cti_pulse.tcl`: `connect -url tcp:127.0.0.1:3121`; `targets -set
  -nocase -filter {name =~ "PSU"}`; unlocks CTI 0 at `CORESIGHT_BASE
  (0xfe80_0000) + 0x0019_0000 = 0xfe99_0000` (`LAR` offset `0xFB0`,
  `CTICONTROL` `0x000`, `CTIOUTEN6`/`CTIOUTEN7` `0x0B8`/`0x0BC`,
  `CTIAPPPULSE` `0x01C` — the same offsets section 17 already proved
  stick); 50× `mwr $CTIAPPPULSE 0x1` with `after 20` between pulses.

**Real gotcha hit and worked around:** `get_hw_probes *dbg_trace_valid*`
(matching section 16's own descriptive probe names) returned "No matching
hw_probes were found" — the diagnostic build's `.ltx` doesn't carry the
net names from `create_bd_debug.tcl`'s `connect_bd_net` calls, only
generic `probe0_1`..`probe3_1`. Recovered the mapping by `WIDTH` instead
(`list_property`/`get_property WIDTH`, since `NAME`/`MATCH` aren't valid
properties on a `hw_probe` object — `list_property $p` is the reliable way
to discover what's queryable): `probe0`=4-bit (`dbg_trace_data_raw`),
`probe1`=8-bit (`dbg_trace_byte`), `probe2`/`probe3`=1-bit each
(`dbg_trace_valid`/`dbg_cdc_write_ready`, order per section 16's own
`connect_bd_net` ordering — both read identically throughout this
session's captures so which is which didn't end up mattering for the
result).

**Result: no observable change, at a materially more sensitive
granularity than section 17's `rx_bytes`/`fifo_high_water`.** Baseline
capture (4096 samples, before any pulse): `probe1` (`dbg_trace_byte`)
constant `0x11` for all 4096 samples — no variation at all, a single
value, not even section 16's own two-value period-4 pattern. Post-pulse
capture (immediately after the 50-pulse burst): **identical** — `probe1`
still constant `0x11` across all 4096 samples, `probe0`/`probe2`/`probe3`
also bit-for-bit identical to baseline. `orbtrace stats` after the pulse
also unchanged (`rx_bytes=0`, `fifo_high_water=63`,
`sync_loss=283907430` — the exact same `sync_loss` value as immediately
before the pulse, corroborating section 17's own noted "`sync_loss` reads
identical across calls" anomaly rather than section 12/14's "climbing"
description). This extends section 17's negative CTI finding to raw
byte-pattern granularity: whatever channel/pulse-mapping this
investigation has tried, CTI 0 provably changes nothing observable
anywhere in the pipeline, from PL-side raw bytes up through the TCP stats
API.

**But: a materially concerning new finding surfaced while sanity-checking
the setup, undermining confidence in the above.** Read R5-0's
`g_heartbeat` (ELF symbol, `nm`-resolved to `0xffff4370`, `.bss`,
incremented by nearly every `Action` the workload dispatches — see
`applications/rpu/orbtrace_workload/main.c`) via direct JTAG memory
readback (`mrd -value`, through the `PSU` target — an ordinary AXI/APB
memory read, **not** an ETM register read, so none of section 15's
`TRCSTATR.PMSTABLE` unreliability-while-tracing caveat applies here).
Three reads spaced 300-500ms apart, both immediately after this session's
first R5 boot and again after a deliberate second reboot: **stuck at
exactly `0` every time.** Cross-validated the read path itself is sound,
not a wrong-address mistake: the same JTAG connection read
`0xFFFF0000` (R5's OCM reset vector) as `0xe59ff018` — bit-for-bit the
value `load_r5.tcl`'s own load step already verifies — and
`0xFFFFFF00` (the boot-sequence reset marker) as `0x52535431`, matching
`load_r5.tcl`'s own printed diagnostic exactly. If `g_heartbeat` were
being read correctly and the workload were genuinely looping, it should
have moved within any of these windows; it never did.

The second reboot attempt made this worse, not better: a first JTAG
attach attempt hit `Error: JTAG-DP STICKY ERROR`/`Invalid ACK (0) in DAP
response` outright (a transient bus issue, recovered on retry — noted,
not chased), and the retry that did boot printed `WARNING: UART0_SR=0x0
TX_EMPTY not set` and read back **`UART0_CR=0 MR=0 BAUDGEN=0 BAUDDIV=0
SR=0`** — every UART0 register zero, meaning `load_r5.tcl`'s own
`setup_uart` step plausibly never took effect this time (contrast the
first boot's `UART0_CR=0x00000114 MR=0x00000020 BAUDGEN=124 BAUDDIV=6
SR=0x00000802`, which looks like a real, working configuration). A
concurrent 15-20s UART capture across both boot attempts shows **only**
A53's own diag-thread output — no `--- ThreadX ETM Workload`/`[TEST
BEGIN]`/`[TEST PASS]` line ever appeared, where every prior session that
booted this same firmware saw it immediately. Leading hypothesis,
untested: UART0 register contention with A53's own diag thread (which
this session's `a53_app` runs unusually UART-chatty compared to earlier
sessions, judging by the diag log volume) — but this is speculation, not
confirmed.

**Net effect: this session's own CTI-negative result (and, by extension,
maybe R5-0's actual liveness during *every* prior session's captures too,
since nothing before this session ever checked `g_heartbeat` either) is
weaker evidence than it looks.** A `0x11`-constant, non-toggling ILA
capture with an unresponsive core behind it and a genuinely-toggling
`0xFF`/`0xDF` capture (section 16, against a core this document's own
text describes as "already running... from a prior session," i.e., known
live) are not obviously testing the same thing. This doesn't retract
section 16/17's own conclusions (still real, independently-obtained
results against their own sessions' hardware state) but does mean this
session's *addition* to them (the ILA-level CTI re-test) should be
treated as inconclusive rather than confirmatory until R5's liveness is
independently nailed down — see next steps.

**Session end state:** `flake.nix`'s `hw_server` addition and this
document's updates are the only code/doc changes, both committed. No
firmware changes this session (the CTI pulse and heartbeat/UART reads
were all live JTAG probes, same as section 17). Board reflashed back to
the last known-good *production* bitstream
(`bazel-out/orbtrace-vivado-m3-10mhz-r4`) + its own `psu_init.tcl` +
current `a53_app`; `orbtrace info` confirmed responsive. R5-0 was left in
whatever state the second (UART-registers-zeroed) boot attempt left it in
— **not** reflashed/reset again after that, since the production
bitstream reflash itself re-establishes a clean PL/A53 state independent
of R5's JTAG-only boot path (R5 is PS hardened silicon, untouched by a PL
bitstream reflash) — a future session should treat R5-0's state as
unknown/unverified until it re-boots it and checks `g_heartbeat` itself,
not assume it's still running `orbtrace_workload` correctly. No
destructive hardware state left behind; `hw_server` process from this
session stopped (not left running).

## 20. Phase 6 continued — R5 liveness confirmed for real, but surfaces a distinct ThreadX-tick bug that likely explains every "idle" capture (2026-08-20, continued)

Picked up section 19's own item 1 (cheapest next step: settle whether
R5-0 genuinely reaches its workload's main loop) directly, using real
UART evidence instead of JTAG memory readback this time — cheaper, and
sidesteps any question about whether `mrd`/`read_memory` itself is
trustworthy.

**R5-0 boots and runs ThreadX for real — decisively confirmed, twice,
via live UART capture, not inferred from a boot script's own diagnostics.**
Captured `/dev/ttyUSB1` (`stty raw -echo` + `cat` to a file) across a
fresh `openocd -f aes_zub.cfg -f load_r5.tcl` boot (JTAG-only, omitting
`psu_init_run.tcl`, same known-safe flow as every session since section
12) of `orbtrace_workload`: the real, live stream shows
`--- ThreadX ETM Workload (AES-ZUB R5F) ---`, `[TEST BEGIN]
orbtrace_workload`, `[TEST DIAG] R5-0 ETM trace workload running`,
`[TEST PASS] orbtrace_workload` in order, cleanly interleaved with A53's
own concurrent diagnostic chatter on the shared UART0 line (cosmetic
interleaving only, per section 14's already-known caveat). This is the
first time in the whole investigation this print sequence was captured
live rather than inferred — resolves section 19's doubt outright, for
this boot at least: R5-0 genuinely reaches `workload_entry`, the ThreadX
thread genuinely runs.

**But a repeat `g_heartbeat` JTAG readback (same technique as section 19,
address re-confirmed via `arm-none-eabi-nm` against the exact ELF just
booted — still `0xffff4370`, ELF unchanged) again read exactly `0`, six
samples spaced ~700ms apart, immediately after this UART-confirmed-good
boot.** Section 19's finding reproduces cleanly even with R5's liveness
no longer in any doubt — so the explanation isn't "R5 might not be
running," it's something more specific.

**Root-caused (partially) via `hello_world`, not `orbtrace_workload` —
cleaner signal, no CoreSight/ETM complexity involved at all.**
`hello_world`'s loop is `uart_print("Hello, World!\r\n");
tx_thread_sleep(100);` — a single self-contained liveness signal with no
`g_heartbeat`/JTAG-readback-reliability question at all: every 1 second
(100 ticks × 10 ms), *if the ThreadX tick is actually advancing*, another
`"Hello, World!"` should appear on UART. Captured a full boot +
9-second window: **`"Hello, World!"` appears in the capture exactly
twice** — once from the unconditional print before `TEST_PASS`, once
from the very first loop iteration (which prints *before* its own
`tx_thread_sleep(100)` call) — and **never again**, despite the capture
window being 9× the thread's own 1-second sleep interval. Reproduced
identically both with the original `timer.c` and with a speculative fix
(below) — not a one-off fluke.

**This means the bug is not specific to `orbtrace_workload`, to
CoreSight, or to anything this document's Phases 4-6 added — it's a
latent bug in the shared RPU ThreadX BSP itself
(`sdk/bsp/rpu/timer.c`/`startup.S`), present since Phase 3's
`hello_world`, silently undetected until now because Phase 3's own
"DONE" verification only ever checked for a single `[TEST PASS]` marker
— which the very first loop iteration produces *before* the first
blocking `tx_thread_sleep` call, regardless of whether the tick ever
advances again afterward.** Every RPU app built on this BSP to date has
only ever proven "the first loop iteration ran," never "the loop
actually loops."

**Investigated the TTC0/GIC path directly via JTAG, inconclusive but
narrowing.** Read TTC0 ch0's real registers (`0xFF11_0000` range,
externally AXI-visible, no `TRCSTATR.PMSTABLE`-style reliability caveat
since these aren't ETM registers) repeatedly while a workload thread sat
stalled in its first sleep:
- `TTC_CLK_CNTRL=0x00000007`, `TTC_CNT_CNTRL=0x00000002` (`CNT_INTERVAL`
  set, counter genuinely running — matches what `ttc0_init()`/
  `timer_start()` write), `TTC_INTERVAL_VAL=0x0000f424` (62500, matches
  the computed 100 Hz interval), `TTC_IER=0x00000001` (interval interrupt
  enabled) — every register the R5 core is supposed to have configured
  reads back exactly as programmed. **The TTC hardware itself is
  correctly configured and counting** — `TTC_COUNT_VAL` sampled
  repeatedly showed large, scattered values consistent with a real,
  continuously reloading down-counter (not stuck).
- `TTC_ISR` (`0xFF11_0054`, read-to-clear) read `0x00000000` across five
  consecutive JTAG reads, spaced widely enough apart (given real JTAG
  round-trip latency) that the counter almost certainly reloaded — and
  therefore should have re-latched an interval-interrupt bit — more than
  once between samples. Getting a clean `0` every time is genuinely
  ambiguous: it's consistent with *either* "the interrupt condition never
  latches at all" (a TTC/GIC configuration bug) *or* "something is
  reading and clearing it faster than this session's JTAG round-trip can
  catch" — which, if true, would mean the interrupt is actually firing
  and being serviced, pointing the bug instead at ThreadX's own
  tick-to-thread-wake logic (`_tx_timer_interrupt`, or the
  `_tx_thread_context_save`/`__tx_irq_processing_return`/
  `_tx_thread_context_restore` assembly glue this BSP's `startup.S`
  documents) rather than at the interrupt controller path at all. This
  session couldn't distinguish the two hypotheses.
- **The RPU's own GIC (PL390, `0xF900_0000`) is architecturally
  unreachable via the JTAG AXI-AP at all** — every read attempt
  (`GICD_CTLR`, `GICC_CTLR`, `GICC_PMR`, `GICD_ISENABLER0`) hit a `JTAG-DP
  STICKY ERROR`. Confirmed this is expected, not a bug: UG1085 Table 13-5
  explicitly labels this address range "RPU - Private CPU Bus for RPU
  MPCore" and its own text says "The RPU GIC's base address is aligned to
  the Cortex-R5F MPCore's low-latency peripheral port (LLPP) base
  address" — LLPP is core-private by architecture, not reachable from any
  external system bus master, JTAG included. This means **GIC state can
  never be diagnosed via JTAG readback for this core** — only inferred
  behaviorally (UART output, TTC register state, or a real ETM/ILA
  capture of the CPU's own instruction stream).
- **Tried one candidate fix, live-tested, no observable effect — reverted
  (not committed, tree is clean).** UG1085 Table 13-5 also documents a
  per-SPI security register (`PL390.spi_security`, `0xF900_0084`, 5
  registers wide) that `gic_init()`/`timer_start()` never program at all
  — if TTC0's SPI resets into Group 1 (non-secure) while `GICD_CTLR`/
  `GICC_CTLR` only enable Group 0 (bit 0, what the existing code writes),
  the interrupt would never signal even though every other register looks
  correctly configured. Tried writing `GICD_CTLR = 0x3` /
  `GICC_CTLR = 0x3` (`EnableGrp0 | EnableGrp1`) instead of `0x1`/`0x1`,
  rebuilt, reflashed `hello_world`: **`"Hello, World!"` still printed
  exactly twice in a 12-second capture — no change.** Either the group
  hypothesis is wrong, or enabling both groups at the distributor/CPU
  interface isn't sufficient without also touching the per-SPI security
  register directly (never tried — `0xF900_0084` is in the
  JTAG-unreachable LLPP range, so it can only be validated by another
  live-build-and-boot cycle, not a quick JTAG probe). Reverted the change
  (`timer.c` back to `GICD_CTLR = 1`/`GICC_CTLR = 1`) rather than leave an
  unconfirmed, ineffective speculative fix in tree — `git diff` clean.

**Why this plausibly reframes the entire Phase 6 mystery, not just a
side note.** ThreadX's own idle-scheduler behavior when no thread is
ready to run (which is exactly the state a single-thread app like
`hello_world`/`orbtrace_workload` enters the moment its one thread blocks
on `tx_thread_sleep`) is, absent any explicit low-power/tickless
configuration this BSP doesn't set up, a tight spin loop — not a genuine
CPU halt. **An ETM tracing a CPU stuck spinning in a tiny, fixed loop for
seconds at a time would produce exactly the kind of low-entropy,
tightly-periodic byte pattern sections 16 and 19 both captured**
(`0xFF`/`0xDF` period-4; later a flat constant `0x11`) — not because of
any CoreSight funnel/TPIU/CTI wiring defect, but because **there was
never any interesting instruction/branch content to trace in the first
place** by the time any of Phase 6's capture attempts actually ran.
Every capture attempt in this document from section 12 onward involved a
boot-then-configure-then-capture sequence that plausibly took well over a
second in wall-clock time — meaning R5 was almost certainly *already*
stuck in this idle spin, past its very first `tx_thread_sleep`, before
ETM tracing was ever enabled. This doesn't retroactively invalidate
sections 12-15's real, independently-confirmed *register-level* fixes
(funnel/TPIU addresses, ETMv4 offsets, trace clock) — those are still
correct and still needed — but it does mean the CTI negative result
(section 17, and section 19's own now-doubly-provisional re-test) may
have been testing an idle CPU rather than disproving a real hypothesis,
and the "no genuine ATB traffic" conclusion (section 16) may have an
explanation that has nothing to do with CoreSight at all.

**Session end state:** no net code changes — the one speculative
`timer.c` edit (`GICD_CTLR`/`GICC_CTLR` group-enable bits) was tried live
and reverted after confirming it didn't help; `git diff` is clean, only
this document changed. Board: no PL bitstream/A53 firmware reflash this
session (all work was R5-only JTAG boots via `load_r5.tcl`, confirmed
throughout not to disturb A53); `orbtrace info 192.168.1.50` reconfirmed
responsive at session end. R5-0 was left running whichever build booted
last during this session's testing (a `hello_world` build, harmless) —
per the same convention as section 19, treat R5-0's state as
unknown/unverified at the start of a future session rather than assuming
continuity.

## 21. Phase 6 continued — the ISR-entry counter test ran, and deepened the mystery instead of resolving it (2026-08-20, continued further)

Ran section 20's own recommended next step immediately (same session):
added `volatile uint32_t g_irq_entries`/`g_irq_last_intid`, incremented/set
unconditionally at the very top of `IRQHandler()` in `sdk/bsp/rpu/timer.c`,
before the `intid == TTC0_CH0_INTID` check — kept in tree, not reverted,
since it's pure additive instrumentation with no behavioral change (same
category as `orbtrace_workload`'s own `g_heartbeat`, not a speculative
fix). Rebuilt `hello_world`, reflashed via the same known-safe
`load_r5.tcl`-only (no `psu_init_run.tcl`) JTAG flow, confirmed
`orbtrace info` still responsive before and after (A53 undisturbed, as
established since section 12).

**Result: `g_irq_entries` climbs steadily at a fixed, very regular rate
(~70 counts per ~0.7 s sample interval — consistent with the TTC's
configured ~100 Hz tick) — proving `IRQHandler()` genuinely is being
entered by a real ARM IRQ exception, repeatedly, on a real hardware
cadence. But `g_irq_last_intid` reads exactly `0x3FF` (the architectural
"spurious/no interrupt" sentinel) on every single sample, across
thousands of entries.** This directly explains section 20's core finding
end to end: `intid == TTC0_CH0_INTID` (68) never matches, so
`_tx_timer_interrupt()` is never called, so ThreadX's tick counter never
advances, so `tx_thread_sleep()` can never expire — regardless of how
correctly the TTC/GIC are otherwise configured.

**Added three more diagnostic globals, same session, to distinguish real
hypotheses instead of guessing further — all came back negative,
narrowing the mystery rather than resolving it:**
- `GICC_RPR` (CPU interface Running Priority Register, offset `0x014`):
  reads `0xFF` (architectural "idle, nothing active") on every sample.
  **Rules out** "a previous interrupt's EOI was never correctly written,
  leaving something stuck Active and blocking acknowledgment of new
  ones" — if that were true, `RPR` would show a real, non-idle priority.
- TTC0's own distributor pending/active bits (`GICD_ISPENDR`/
  `GICD_ISACTIVER`, word 2, bit 4 — the same word/bit `gic_init()`
  itself uses): both read `0` on every sample. **Rules out** "TTC0's
  interrupt is genuinely latched pending or active at the distributor,
  just not being surfaced through `IAR`" — the distributor itself
  agrees with the CPU interface that nothing is outstanding for this ID.
- Word 0 of the same registers (covering PPI IDs 16-31, including
  UG1085's documented "Legacy IRQ signal (from PL)" at ID 31 — a
  GIC-SPI-independent path the TRM specifically calls out: "The FIQ
  signal and IRQ signal from the PL are inverted and then sent to the
  interrupt controller"): both read `0x00000000` on every sample.
  **Rules out** a stray/floating legacy PL-routed IRQ line as the
  exception source.
- Also ruled out, by construction rather than by reading a register: a
  genuine ARM7 FIQ being mis-routed into the IRQ vector. `startup.S`'s
  `__tx_fiq_handler` is a dead-end (`B __tx_fiq_handler`, an
  unconditional self-branch that never calls any C code) — if these
  exceptions were really arriving as FIQ, `IRQHandler()` could never be
  reached at all, contradicting the very fact that `g_irq_entries`
  advances. Whatever these are, they are demonstrably real ARM **IRQ**
  exceptions, taken through the documented `__tx_irq_handler` →
  `_tx_thread_context_save` → `__tx_irq_processing_return` → `BL
  IRQHandler` chain.

**Net position at the end of this session: a real, repeated, hardware-timed
IRQ exception is unambiguously reaching the R5 core and unambiguously
entering `IRQHandler()` — but every GIC-level structure this session
could reach (CPU interface IAR and RPR, distributor pending/active bits
for both the expected SPI and the one documented PPI alternative) agrees,
100% of the time, that nothing is outstanding.** This is a stronger,
more specific puzzle than section 20 left off with, not a solved one.
Two live hypotheses, neither confirmed:
1. **A genuine timing race narrower than this investigation can currently
   observe** — e.g. the real interrupt source is edge-triggered and
   self-clears at the distributor faster than the handful of instructions
   between vector entry and this session's register reads, even though
   TTC's own interval-interrupt bit is documented as level-held-until-read
   (section 19/20 already confirmed `TTC_ISR` behaves that way under
   direct JTAG reads) — this would require the real culprit to be a
   *different*, not-yet-identified, self-clearing interrupt source, not
   TTC0 itself.
2. **Stale/uncleared CoreSight debug state from R5-0's own persistently
   failing examine attempts.** Every session in this document, including
   this one, has seen `load_r5.tcl`'s `examine_and_run` step fail with
   `JTAG-DP STICKY ERROR` trying to examine R5-0's own CoreSight (a
   failure this document has treated as benign/ignorable throughout,
   "UART output is ground truth" — see section 11 onward). This session's
   findings raise a real, not-yet-tested possibility that repeated failed
   examine attempts, across many sessions without an intervening power
   cycle, have left genuine partial debug-domain state (e.g. a stuck
   watchpoint/breakpoint, `EDSCR`/halt-request residue) that could
   produce exactly this kind of "real exception fires, but nothing
   GIC-level explains it" symptom, if such state routes through — or
   otherwise disturbs — the same IRQ exception path independent of the
   GIC's own SPI/PPI bookkeeping.

**Session end state:** the diagnostic globals in `sdk/bsp/rpu/timer.c`
(`g_irq_entries`, `g_irq_last_intid`, `g_irq_last_rpr`,
`g_irq_ttc_pending_bit`, `g_irq_ttc_active_bit`,
`g_irq_ppi_pending_word0`, `g_irq_ppi_active_word0`) are real, working,
committed instrumentation — kept in tree deliberately (pure additions,
no behavioral change, same category as `orbtrace_workload`'s own
`g_heartbeat`), not reverted. No other firmware changes. Board: no PL
bitstream/A53 reflash this session; `orbtrace info 192.168.1.50`
reconfirmed responsive at session end. R5-0 left running the
final instrumented `hello_world` build — harmless, but per the
established convention, treat R5-0's state as unknown/unverified at the
start of a future session.

## 22. Phase 6 continued — real power cycle rules out the leading hypothesis, and a deeper check finds TTC0 was likely never the real IRQ source at all (2026-08-20, continued further still)

The user physically power-cycled the board (unplug/replug, not another
JTAG soft-reset) specifically to test section 21's leading hypothesis
(stale CoreSight debug-domain state, accumulated across many
`RST_LPD_TOP` soft-resets without a power cycle). Reflashed via the
normal `jtag_flash.sh` flow, pairing the known-good production bitstream
(`bazel-out/orbtrace-vivado-m3-10mhz-r4/zub_orbtrace.bit`) with *that
build's own* `psu_init.tcl` (section 14's standing rule) and a freshly
rebuilt `a53_app` (current HEAD, includes this session's `timer.c`
instrumentation — irrelevant to A53 itself, just picked up incidentally
by the rebuild). `orbtrace info` confirmed responsive immediately after.

**Result: the hypothesis is wrong. Both symptoms reproduce identically
from a genuine cold boot.** `load_r5.tcl` took "Path A: fresh reset" this
time (confirms this really was a cold boot, not a soft-reset masquerading
as one — `RST_LPD_TOP` read `0x547` before any write, matching a true
power-on state, not the "already running" state Path B handles).
CoreSight examine **still** failed with the exact same `JTAG-DP STICKY
ERROR`, immediately, on the very first attempt after power-on. The IRQ
diagnostics from section 21 (`g_irq_entries`, `g_irq_last_intid`, `RPR`,
TTC0/PPI pending+active bits) reproduced bit-for-bit: entries climbing at
the same steady rate, `intid` and `RPR` idle/spurious every time, all
pending/active bits `0` every time. **This rules out "stale JTAG/debug
state accumulated across soft-resets" as the cause of either symptom** —
whatever this is, it's a deterministic property of this exact hardware/
firmware/tooling combination, reproducible from a genuinely cold boot,
not an artifact of this investigation's own repeated JTAG sessions.

**Checked ThreadX's own vendored port source directly, rather than
continuing to guess.** Located the real `tx_thread_context_save.S` for
`ports/cortex_r5/gnu` (Eclipse ThreadX v6.4.3.202503_rel, the version
this project's `MODULE.bazel` pins, fetched into Bazel's external repo
cache) and read it in full: it does **not** touch `GICC_IAR` or any GIC
register at all — pure generic register/stack-pointer save logic,
dispatching to `__tx_irq_processing_return` afterward. **This rules out
the "ThreadX's own port already consumed the interrupt via an earlier
IAR read, and `IRQHandler()`'s own read is a second, necessarily-spurious
read" theory** that had been an open possibility since section 20 — there
is no earlier read to have consumed anything.

**Added one more diagnostic, `GICC_HPPIR` (Highest Priority Pending
Interrupt Register, CPU-interface offset `0x018`) — architecturally a
*non-consuming* read, unlike `IAR`.** Read it first, before `IAR`, on
every entry. **Result: `HPPIR` reads the same spurious `0x3FF` as `IAR`,
every single time.** This rules out an `IAR`-specific read anomaly (some
GIC implementations have known erratum around `IAR` under particular
conditions) — the distributor's live, side-effect-free view agrees with
the consuming read. Every angle this investigation can reach from
software agrees: at the moment of exception entry, nothing is
GIC-distributor-pending for this CPU.

**The one genuinely new, decisive finding this session: TTC0's own
interval-interrupt status bit never sets at all — checked directly via
external JTAG readback of `TTC_ISR`, independent of anything the R5 core
itself does.** `TTC_ISR` (`0xFF11_0054`, read-to-clear) read `0x0` on a
first read, `0x0` again 5ms later, and **still `0x0` 20ms later** — a
window comfortably longer than the interval period implied by
`TTC_COUNT_VAL`'s own state at the time (`0x7D2B` = 32043, well under
half of the 62500-count interval, meaning the counter should cross zero
and reload at least once, plausibly twice, inside that 20 ms window).
`TTC_IER` was confirmed `0x1` (interval interrupt enabled) at the same
moment. Per UG1085's own description of the TTC interrupt module ("the
interrupt register takes the interrupt signals from the timer-counter
module and stores them until the register is read" — a genuine
level-style latch, not a narrow pulse this session's own JTAG timing
could plausibly miss three times in a row), **a real, enabled interval
interrupt condition should have latched and stayed latched across at
least one of these three reads. It never did, at any point this whole
investigation has directly checked it from outside the ISR.**

**This likely means TTC0 was never actually the source of the repeated
IRQ exceptions sections 20-21 characterized at all — an assumption this
whole sub-investigation had been making implicitly, now directly
contradicted.** The apparent ~100 Hz correlation between `g_irq_entries`'
climb rate and TTC0's configured interval was never independently
verified against a real clock reference (only eyeballed from JTAG-session
sample spacing, which has its own, substantial, uncharacterized
round-trip latency) — it's now the leading suspect for having been a
coincidental or mistaken correlation, not evidence TTC0 is driving these
exceptions. What actually *is* generating a real, repeated ARM IRQ
exception that enters `IRQHandler()` is, as of this session, genuinely
open again — with TTC0 specifically now a *ruled-out* candidate rather
than the leading one.

**Session end state:** one more diagnostic global added and kept
(`g_irq_last_hppir`, plus the `GICC_HPPIR` register definition) — same
pure-addition, no-behavior-change category as section 21's instrumentation,
not reverted. No other firmware changes. Board: reflashed once this
session (production bitstream + its own `psu_init.tcl` + current
`a53_app`, following the genuine power cycle); `orbtrace info
192.168.1.50` reconfirmed responsive at session end. R5-0 left running
the final instrumented `hello_world` build (harmless JTAG-only boot) —
per the established convention, treat R5-0's state as unknown/unverified
at the start of a future session.

## 23. Phase 6 continued — the disable test corrects section 22: TTC0 IS the trigger, but its own ISR bit still can't be caught set (2026-08-20, continued yet further)

Ran section 22's own recommended cheapest next step immediately, same
session: temporarily wrapped `timer_start()`'s `TTC_IER =
TTC_INT_INTERVAL;` and `TTC_CNT_CNTRL = CNT_INTERVAL;` lines in `#if 0`
(everything else in `timer_start()` — `GICD_ISENABLER` for TTC0's INTID,
`GICD_CTLR`, `GICC_PMR`, `GICC_CTLR`, `cpsie i` — left untouched, so this
isolates specifically "does TTC0's own IER-enable + running counter
matter" from "does the GIC being generically enabled matter"). Rebuilt
`hello_world`, reflashed via the same known-safe JTAG-only flow, verified
`orbtrace info` responsive throughout.

**Result: `g_irq_entries` stayed exactly `0` across every sample, for the
entire test window — `IRQHandler()` was never entered at all with TTC0's
own enable/counter left off.** This directly contradicts section 22's
"TTC0 was likely never the real source" conclusion: disabling exactly
those two TTC-specific writes, and nothing else, is sufficient to fully
stop the IRQ exceptions. **TTC0 is causally necessary for these
exceptions — section 22's conclusion was wrong, corrected here.**
Reverted the `#if 0` immediately after the test (`git diff` clean,
confirmed byte-for-byte identical to the last commit); rebuilt and
re-ran the host ELF/size tests to confirm the revert was clean.

**This leaves a genuine, still-unresolved contradiction between two
solid, independently-repeated pieces of evidence:**
- TTC0's own enable state is causally necessary for the IRQ exceptions
  (this section, clean A/B test).
- Yet `TTC_ISR` — read via external JTAG, completely independent of
  anything the R5 core itself does — never once reads as set, across
  three separate reads spanning a 20 ms window that should have seen at
  least one real interval-reload event (section 22), and `GICC_IAR`/
  `GICC_HPPIR` (one consuming, one not) both agree nothing is
  GIC-distributor-pending at the moment `IRQHandler()` executes (sections
  20-21).

**Leading hypothesis, not yet checked:** `GICD_ICFGR` (Interrupt
Configuration Register, sets edge- vs level-sensitivity per SPI) is
never touched by this BSP at all — `gic_init()`/`timer_start()` configure
target CPU, priority, and enable, but never `ICFGR`. If TTC0's SPI
defaults to (or this specific GIC instance hardwires) edge-sensitive
routing at the *distributor*, the physical assertion reaching the CPU
could be a genuinely narrow pulse that the distributor's own edge-detect
logic auto-consumes essentially instantly — well before either the CPU's
own `IAR` read (a handful of instructions after vector entry) or an
external JTAG read (tens of milliseconds later) could observe it as
pending — while `TTC_ISR` itself, a register inside the *TTC peripheral*
and governed by the TTC's own read-to-clear semantics (independent of
however the GIC routes the resulting signal), should by UG1085's own
description still remain latched until read... **which is exactly the
part of this hypothesis that doesn't yet add up**, and is worth
scrutinizing directly rather than assumed away: `GICD_ICFGR` is in the
same JTAG-unreachable LLPP range as every other GIC register, so testing
it needs the same in-`IRQHandler()`-diagnostic-global technique this
whole investigation has been using, not a direct JTAG read.

**Session end state:** no net firmware change from this section (the
`#if 0` experiment was tried and fully reverted, confirmed via `git
diff`). Board: no reflash needed this section (reused section 22's
already-flashed, already-verified-responsive A53); R5-0 left running the
final (reverted-to-normal) `hello_world` build. `orbtrace info` confirmed
responsive. No destructive hardware state left behind.

## 24. Phase 6 continued — GICD_ICFGR checked, edge-triggering hypothesis ruled out too (2026-08-20, continued yet further)

Ran section 23's own recommended cheapest next step immediately, same
session: added `GICD_ICFGR(n)` (offset `0xC00 + n*4`, 2 bits per
interrupt ID, 16 IDs per register — `n = id/16`) and a diagnostic global
reading TTC0's specific 2-bit field (`shift = 2*(id%16)`) from inside
`IRQHandler()`, same technique as every other GIC diagnostic this
investigation has added. Rebuilt, reflashed, booted, read via JTAG.

**Result: `ttc_icfgr_bits = 0x1` on every sample.** Per the standard
GICv1/v2 `ICFGR` bit layout, bit `2n` is the Model bit (fixed/
implementation bit, not meaningful for sensitivity) and bit `2n+1` is the
actual Int_config bit (`1` = edge-triggered, `0` = level-sensitive).
`0x1` means bit `2n+1` reads `0` — **TTC0's SPI is configured
level-sensitive, not edge-triggered.** This directly rules out section
23's leading hypothesis: an edge-triggered narrow-pulse explanation
doesn't apply here.

**Net position, now checked from essentially every angle software alone
can reach:** TTC0 is causally necessary for the IRQ exceptions (section
23's disable test), its SPI is configured level-sensitive at the
distributor (this section), yet `TTC_ISR`, `GICC_IAR` (consuming),
`GICC_HPPIR` (non-consuming), and the distributor's own pending/active
bits for that exact ID all agree, every time, that nothing is
outstanding. For a level-sensitive interrupt, "pending" at the
distributor is architecturally just a live mirror of the raw input line
while not Active — so this observation is logically consistent with "the
underlying `TTC_ISR` condition itself is only ever true for a window
narrower than any read path (CPU-internal or external JTAG) has managed
to catch," which pushes the real question back onto **why `TTC_ISR`
would ever self-clear at all**, given nothing in this code path reads it
outside the dead `if (intid == 68)` branch, and UG1085's own TTC chapter
describes it as a genuine store-until-read latch, not a pulse. That
specific contradiction remains the crux of the mystery and is not
resolved by anything checked so far.

**Session end state:** one more diagnostic global kept
(`g_irq_ttc_icfgr_bits`, plus the `GICD_ICFGR` register macro) — same
pure-addition, no-behavior-change category as every prior diagnostic in
sections 20-23. Host ELF/size tests re-verified passing. Board: no
reflash needed (reused section 23's already-flashed, already-verified
A53); `orbtrace info` confirmed responsive. R5-0 left running the final
instrumented `hello_world` build. No destructive hardware state left
behind.

## 25. Phase 6 continued — in-ISR TTC_ISR read still shows nothing latched, and IER-alone is confirmed insufficient (2026-08-20, continued yet further still)

Picked up section 24's next-steps items 1 and 2 directly, same session. Board
was confirmed present and the A53 Orbtrace service reachable over TCP port
3401 (`ZUBoard-Orbtrace/1` banner) before touching R5 at all — ICMP `ping`
to `192.168.1.50` fails and is a red herring: the A53's own hand-rolled
TCP/IP stack (visible via its UART diagnostic chatter, `diag: ISR=...
NWSR=0x86 ...`) evidently never implements ICMP echo; a raw TCP connect to
port 3401 (`/dev/tcp/192.168.1.50/3401`) succeeds immediately, and
`bazel run //applications/orbtrace/model:orbtrace -- info` confirms this
properly. Worth remembering for a future session that doesn't want to
mis-diagnose "ping fails" as "board is down."

**New instrumentation, item 2's spirit: read `TTC_ISR` and `TTC_COUNT_VAL`
from *inside* `IRQHandler()` itself, unconditionally, for the first time
in this investigation.** Every prior `TTC_ISR` read (sections 19, 22) was
an external JTAG read, tens of milliseconds after the exception —
section 22 already flagged this as a real methodological gap ("too slow to
rule out a self-clearing condition"). Added `g_irq_ttc_isr_bits` (raw
`TTC_ISR`, read as the very first register access in `IRQHandler()`,
before `GICC_HPPIR`/`GICC_IAR`... actually after those two, since they're
GIC registers and reading them first was already established as the
cleanest non-consuming order — but *before* anything else touches TTC) and
`g_irq_ttc_count_val` (raw `TTC_COUNT_VAL` at the same moment, settling the
counter-direction/behavior question). Host tests extended and passing,
real aarch64 cross-build succeeds (`bazel test --config=host
//tests:rpu_hello_world_elf_test //tests:rpu_hello_world_size_test
//tests:rpu_orbtrace_workload_elf_test //tests:rpu_orbtrace_workload_size_test`
all pass).

**Real hardware result: `TTC_ISR` reads exactly `0x00000000` from inside
the handler too, every single sample.** Booted the instrumented
`hello_world` via the known-safe JTAG-only flow (`load_r5.tcl`, omitting
`psu_init_run.tcl`), confirmed live via UART (`--- ThreadX Hello World
---` / `[TEST BEGIN]` / `[TEST PASS]`, and confirmed the tick-stall bug
still reproduces exactly as sections 20-24 describe: exactly 2 `"Hello,
World!"` lines across a full 25-second capture, not the ~25 a genuinely
advancing 1 Hz tick would print). Sampled `g_irq_entries` /
`g_irq_ttc_isr_bits` / `g_irq_ttc_count_val` via 6 separate JTAG sessions
(`uscale.axi` `mdw`, ~1.5s apart, each a fresh `openocd` connect/disconnect
to avoid any single-session artifact): `g_irq_entries` climbed steadily
(`0x2ce6` → `0x30d9`, ~336 counts over ~9 wall-clock seconds — roughly
37/s, not the expected ~100/s, but the per-sample JTAG connect/teardown
overhead dominates that window's real elapsed time, so this isn't a clean
rate measurement, just confirms "climbing steadily"). **`g_irq_ttc_isr_bits`
read `0x00000000` on all 6 samples. `g_irq_ttc_count_val` read exactly
`0x0000000d` (13) on all 6 samples, bit-for-bit identical every time.**
`g_irq_ttc_icfgr_bits` stayed `0x1` (level-sensitive, matches section 24).

**This closes off the "JTAG readback is too slow" explanation for good.**
`TTC_ISR` reading a stored, latched-until-read value of exactly zero from
literally the first instructions of the exception handler — not a
polling loop tens of milliseconds later — means the interval condition
genuinely is not observably set at the point software can first look,
for a register UG1085 describes as "stores [the interrupt] until the
register is read," not a register that could plausibly self-clear on its
own between the hardware event and a same-microsecond read.

**The identical `TTC_COUNT_VAL=13` across six independent samples is a new,
genuinely strange data point, not explained by anything checked so far.**
If this really is a free-running interval counter (as `ttc0_init()`
programs it — `CNT_INTERVAL` set, counter never explicitly held at a fixed
value), a value this small (13 out of a 62500 interval), and this
*identical* across six physically separate JTAG connect/read round-trips
(each with its own, uncorrelated few-hundred-millisecond USB/JTAG latency),
is consistent with "software always happens to read `COUNT_VAL` a fixed,
tiny number of ticks after every interval-reset event" — i.e., the counter
genuinely does reach its interval and reset (explaining why `IRQHandler()`
entries correlate with TTC0 being enabled at all, per section 23), and
whatever generates these IRQ exceptions does so essentially immediately
after that reset, every time, with sub-JTAG-sample-jitter precision. That
level of determinism is hard to square with "an unrelated, coincidental
interrupt source" and easy to square with "this genuinely is the TTC
interval event, but something about how its signal reaches the CPU/GIC
leaves no trace in any status register this investigation can reach."

**Ran section 24 next-steps item 1 for real: `TTC_IER` enabled, counter
left statically disabled (`CNT_DIS`, no `CNT_INTERVAL` toggle) —
temporarily wrapped just the `TTC_CNT_CNTRL = CNT_INTERVAL;` line in
`timer_start()` in `#if 0` (everything else, including `TTC_IER =
TTC_INT_INTERVAL;` and the GIC enable sequence, untouched). Rebuilt,
reflashed, booted, sampled 6 times over ~9s.**

**Result: `g_irq_entries` stayed exactly `0` across every sample — no
entries at all with the counter statically disabled, even though `TTC_IER`
was genuinely enabled.** This directly answers item 1's open question:
**merely enabling `TTC_IER` is not sufficient by itself; the counter must
actually be counting (in interval mode) for any IRQ to occur.** Combined
with section 23's original disable test, this narrows the causal
requirement from "TTC0's enable state generally" down specifically to "the
counter genuinely running in interval mode" — strengthening, not
weakening, the case that a real interval-match hardware event is the
trigger, not some artifact of the `IER` write itself. Reverted the `#if 0`
immediately after (`git diff sdk/bsp/rpu/timer.c` now shows only the two
new permanent diagnostic globals/reads, confirmed via `git diff --stat`);
rebuilt, host-tested, and reflashed the reverted build onto R5-0 before
ending the session.

**Net position: item 1 resolved cleanly (counter must genuinely run), item
2 resolved cleanly but deepens rather than dissolves the mystery (TTC_ISR
is unset even at the earliest possible software vantage point). The core
contradiction from section 24 stands, now on firmer footing: a real,
counter-driven hardware event reproducibly and immediately produces a real
ARM IRQ exception into `IRQHandler()`, yet every status register this
investigation can reach — `TTC_ISR` (now including an in-ISR read),
`GICC_IAR`, `GICC_HPPIR`, distributor pending/active bits — agrees, every
single time, that nothing is outstanding.** The remaining live
possibilities are unchanged from section 24's own framing (something
faster than even an in-ISR read, or a signal path this investigation
hasn't identified at all) — this session's contribution is ruling out the
"read too slow" objection specifically, not resolving the mystery itself.

**Session end state:** two new permanent diagnostic globals
(`g_irq_ttc_isr_bits`, `g_irq_ttc_count_val`) committed, same
pure-addition/no-behavior-change category as every diagnostic since
section 20. The `#if 0` counter-disable experiment was tried live and
fully reverted (`git diff` clean on that line). No PL bitstream/A53
firmware reflash this session (all work was R5-only JTAG boots via
`load_r5.tcl`, confirmed via `orbtrace info` not to have disturbed A53 at
any point, checked both before and after this session's R5 work). R5-0
left running the final, reverted `hello_world` build (harmless). No
destructive hardware state left behind.

## 26. Phase 6 continued — real ~105 Hz rate confirmed; three more CoreSight-examine hypotheses tried and ruled out (2026-08-20, same day, continued still further)

Picked up next-steps items 3 and 4 directly, same session as section 25.
Board reconfirmed reachable first (`orbtrace info` → `ZUBoard-Orbtrace/1`) —
no reflash needed, R5-0 still running section 25's final `hello_world`
build.

**Item 3, resolved cleanly: the ~100 Hz correlation is real, not a JTAG-sampling
artifact.** Bracketed a genuine wall-clock `time.sleep(10)` (Python, not
in-script `after` delays) between two independent, short-lived `openocd`
JTAG sessions, each doing nothing but `mdw` on `g_irq_entries`. Result:
entries climbed from `30229` to `31282` (delta `1053`) across a
precisely-timed `10.0002 s` window — **105.30 Hz**, close enough to the
BSP's intended 100 Hz (`TTC_INTERVAL_VAL`/`TTC_COUNTER_CLK_HZ` computed
assuming a 100 MHz `TTC_REF_CLK_HZ`) that the small delta is more likely a
~5% error in that assumed reference clock than evidence of a different
mechanism. This closes off any remaining doubt that `g_irq_entries`'
climb rate was some polling/session artifact rather than a real,
counter-clock-derived hardware rate — it's exactly the kind of tight,
deterministic timing a genuine interval timer produces.

**Item 4, three new hypotheses tried, all ruled out — `JTAG-DP STICKY
ERROR` on R5-0 CoreSight examine remains unexplained.** Tested against
the *already-running* R5-0 (no reboot needed for these three checks):

1. **JTAG clock speed.** Tried `uscale.r5_0 arp_examine` at 3000 kHz
   (default), 500 kHz, and 100 kHz in the same session (`adapter speed`
   between attempts, `clear_stickyerr` before each). Identical
   `JTAG-DP STICKY ERROR` at all three speeds. Rules out a
   Vivado-Hardware-Manager-style "TCK vs. debug hub clock ratio" issue
   (section 16's own `PARAM.FREQUENCY 1000000` fix doesn't apply here —
   different failure shape).
2. **LPD debug clock gating (`DBG_LPD_CTRL`), the same class of bug
   section 14 found for the FPD trace clock.** Found the real register in
   UG1085 (Table 37-7, Table 39-10: `CRL_APB.DBG_LPD_CTRL`, offset
   `0x068` → `0xFF5E0068`, "used by RPU debug logic and funnel"; unlike
   `DBG_TRACE_CTRL`'s documented reset default of "Clock stop," this one's
   documented reset default is `CLKACT=Enabled`). Read live:
   `0x01010500` — `CLKACT=1` (bit 24, genuinely enabled),
   `DIVISOR0=5` (not the raw-reset `0x20`, so *something* — plausibly
   this build's own `psu_init.tcl`, not checked further this session —
   already reprograms it for a faster debug clock, matching the same
   `DIVISOR0=5` pattern section 14 found for `DBG_TRACE_CTRL`). **Ruled
   out**: the LPD debug clock is genuinely running, not gated.
3. **RPU debug-logic held in reset (`CRL_APB.RST_LPD_DBG`).** UG1085
   Table 39-11 documents this exact register's bits
   (`rpu_dbg{0,1}_reset`, `dbg_lpd_reset`) as controlling debug-logic
   reset for the LPD/RPU — a real, specific, TRM-named candidate for
   "CoreSight examine fails because the debug logic itself is held in
   reset," distinct from the clock-gating question. Read live at the
   well-known ZynqMP offset `0x218` (`0xFF5E0218`, not independently
   confirmed against this archive's address tables — no explicit hex
   offset was found near the register's TRM description text, only
   recalled from familiarity with Xilinx's BSP headers; worth
   double-checking against UG1087 Register Reference if available
   before trusting this address further): `0x00000000` — if the
   convention matches every other `RST_*` register this project has
   already confirmed (`RST_LPD_TOP`: bit=1 means "in reset," matching
   `load_r5.tcl`'s own comments), an all-zero reading means **nothing is
   held in debug-reset**. **Ruled out**, with the caveat that the
   register address itself is less rigorously confirmed than the other
   findings in this document — worth re-deriving from UG1087 rather than
   memory if this needs revisiting.
4. **AP index/type mismatch.** Enumerated all APs on `uscale.dap` via
   `apid` (0-3): AP0 `0x34770004` (class 8 = MEM-AP, type 4 = AMBA
   AXI3/4 — matches `uscale.axi`'s own `-ap-num 0`), **AP1 `0x44770002`
   (class 8 = MEM-AP, type 2 = AMBA APB2/3 — matches `uscale.r5_0`'s
   configured `-ap-num 1`, and matches the "APB-AP" naming this
   investigation has used since section 1)**, AP2 `0x24760010` (class 0 =
   JTAG-AP, the fourth CoreSight access path UG1085 Table 39-10 lists
   alongside JTAG-DP/AXI-AP/APB-AP), AP3 `0x00000000` (no further AP).
   **Ruled out**: `-ap-num 1 -dbgbase 0xFE810000` is genuinely the right
   AP, of the right type, at the right index — not a stale/wrong
   configuration.

**A fourth idea (XPPU firewalling the APB-AP's access to the CoreSight
region) was considered but not tested — reasoned through instead, and
judged unlikely enough not to be worth a live probe this session.**
UG1085 Table 10-4 lists `CRL_APB` (`0xFF5E_0000`) and `RPU`
(`0xFF9A_0000`) as XPPU-protected — both already-known-relevant
(`load_r5.tcl` already has an XPPU-blocked-write fallback for
`RPU_GLBL_CNTL`) — but the CoreSight debug region (`0xFE80_0000`
upward) never appears in that table or the System Addresses chapter's
reserved-range list. Section 9's own text ("The JTAG DAP controller can
use the AXI interconnect to access the APB slave interfaces") describes
a *different*, generic APB-bridge access path (for peripherals like
SYSMON) distinct from a dedicated CoreSight debug APB-AP, which by
standard ARM DAP architecture is normally its own physically separate
bus, not routed through the system's protected AXI interconnect at all —
consistent with AP1's own IDR identifying it as a genuine, standard
CoreSight-class APB-AP. Worth revisiting only if every other avenue is
exhausted.

**Net position on item 4: four real, specific, TRM-or-architecture-motivated
hypotheses tried across this session and section 16, all negative.**
Power-up handshake (section 16), JTAG clock speed, LPD debug clock
gating, RPU debug-logic reset, and AP configuration are now all confirmed
*not* the explanation. What remains unexplored: the actual ROM-table walk
`arp_examine` performs once it starts talking to AP1 (this investigation
has only checked *preconditions* for that walk succeeding, never traced
the walk itself — e.g. via OpenOCD's own `-d3` debug-level tracing to see
exactly which transaction inside the examine sequence first returns the
sticky error), and the address-confidence caveat on `RST_LPD_DBG` above.

**Session end state:** no firmware/code changes this session beyond
section 25's (already committed). All of this session's new work was
read-only JTAG register probes (adapter-speed changes, `DBG_LPD_CTRL`/
`RST_LPD_DBG` reads, AP enumeration) against R5-0's *already-running*
`hello_world` — no reboot needed for items 3/4, so no new UART/boot
evidence to report beyond section 25's. Board: no PL bitstream/A53
reflash; `orbtrace info 192.168.1.50` reconfirmed responsive both before
and after this session's R5 JTAG probing. R5-0 left running the same
`hello_world` build section 25 ended with. No destructive hardware state
left behind.

## Next steps for a future session

1. **Superseded, resolved 2026-08-20 (section 25) — kept for history.**
   ~~Extend the disable-test methodology to isolate TTC_IER from the
   running counter~~ — done: IER-enabled-with-static-counter produces
   zero entries, so the counter must genuinely be running/reaching its
   interval, not just IER being set.
2. **Partially resolved 2026-08-20 (section 25).** The `TTC_ISR`
   "other bits" question is now moot for the interval bit specifically —
   an in-ISR read at the earliest possible vantage point still shows
   `0x0`, ruling out "JTAG read too slow to catch it." Still open: whether
   a genuinely *different* TTC_ISR bit (match/overflow/event) explains
   anything, and confirming counter direction — `TTC_COUNT_VAL` read a
   suspicious identical constant (13) across six independent samples,
   worth understanding on its own terms (see section 25's discussion).
3. **Resolved 2026-08-20 (section 26) — kept for history.**
   ~~Independently verify the actual `g_irq_entries` rate against a real
   clock~~ — done: a wall-clock-bracketed 10s window measured **105.30
   Hz**, confirming the entry rate is genuinely clock-derived and close to
   the BSP's intended ~100 Hz, not a sampling artifact.
4. **`JTAG-DP STICKY ERROR` on R5-0 CoreSight examine: four hypotheses
   ruled out 2026-08-20 (section 26), still unresolved.** JTAG clock
   speed, `DBG_LPD_CTRL` (LPD debug clock gating), `RST_LPD_DBG` (RPU
   debug-logic reset — note: this register's address was recalled from
   memory of Xilinx BSP headers, not independently confirmed against this
   repo's UG1085 archive; re-derive from UG1087 Register Reference if
   revisiting), and DAP AP index/type have all been checked and are not
   the explanation. Not yet tried: OpenOCD's own `-d3` transaction-level
   debug tracing during `arp_examine`, to see exactly which step of the
   ROM-table walk first returns the sticky error, rather than only
   checking preconditions for it to succeed. Worth understanding on its
   own terms regardless of the IRQ mystery: a working halt/register-dump
   capability would let a future session read the real
   `LR_irq`/`SPSR_irq`/mode at the exact moment of one of these mystery
   exceptions, which no amount of further memory-mapped polling from
   inside `timer.c` can substitute for.
5. **Once the tick is confirmed genuinely advancing (heartbeat or
   equivalent moving under JTAG readback across several seconds), redo
   the ETM capture from scratch** — both the plain `configure r5 tpiu4` →
   `start` → `capture` sequence (section 14's baseline) and, if that
   alone doesn't move `rx_bytes`, the combined ILA-while-pulsing-CTI-0
   test (tooling already proven end-to-end in section 19: `hw_server` is
   a real `flake.nix` devShell tool, Vivado Hardware Manager and `xsct`
   share one instance concurrently, `ila_capture.tcl`/`cti_pulse.tcl`
   shape reusable, recreate from section 19's description). Every
   capture result from sections 12-19 should be treated as measuring an
   idle CPU until this is redone against a workload confirmed genuinely
   looping.
6. Get the real **Arm CoreSight SoC-400 Technical Reference Manual**
   (UG1085's own Ref 39 citation) — not in the private archive; check
   there first before searching further afield. Still the authoritative
   source for exact funnel/TPIU ATB-handshake behavior beyond the simple
   port-select bitmask this investigation has already confirmed working
   — worth having regardless of how item 1 resolves.
7. Once `rx_bytes` moves, mirror `M3_TRACE_VERIFICATION_PLAN.md`'s Phase
   E/F methodology exactly: confirm real content recovery against
   `applications/orbtrace/firmware/rpu`'s known-reproducible `Workload`
   sequence before trusting anything downstream (this project has been
   burned before by declaring victory on `rx_bytes > 0` alone — see that
   document's own Phase E cautionary history).
8. Before investing further in Phase 6-8's ambition level, it's worth
   doing the real UG1085/Cortex-A53 TRM check flagged in section 3, point
   5 — if this silicon's ETM genuinely has address/data comparators or an
   ETR path, that changes how ambitious those phases are worth being from
   the start.
9. Everything from Phase 6 onward is new engineering, not just wiring —
   size it accordingly before starting, the same way
   `M3_TRACE_VERIFICATION_PLAN.md` Phase H sized the ETM-on-M3 path
   before recommending against starting it without a clear bandwidth case.
