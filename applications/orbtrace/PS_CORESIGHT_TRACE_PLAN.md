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
| 6 | First real hardware capture on the R5/A53 path, verify PS/ETM sync (mirrors M3 Phase E/F) | NOT STARTED |
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

## Next steps for a future session

1. **Cheapest, do first:** Phase 6 (first real ETM capture). This needs
   two things `coresight::select()` doesn't do yet: (a) actually
   programming the R5-0 ETM's own trace-enable/config registers (not just
   unlocking components and wiring funnels — see section 11's "Not yet
   done"), and (b) a capture attempt against `applications/rpu/
   orbtrace_workload` mirroring `M3_TRACE_VERIFICATION_PLAN.md`'s Phase
   E/F methodology (get a real capture first, verify content against the
   known-reproducible `Workload` sequence before trusting anything
   downstream). Remember section 10's psu_init/A53-network gotcha — an
   R5 JTAG session and a live A53 Orbtrace capture can't coexist, so
   deciding the capture-side workflow (does verifying an R5 capture need
   JTAG at the same time as the A53's TCP trace stream, or can they be
   sequenced?) is itself part of this phase's scoping.
2. Before investing further in Phase 6-8's ambition level, it's worth
   doing the real UG1085/Cortex-A53 TRM check flagged in section 3, point
   5 — if this silicon's ETM genuinely has address/data comparators or an
   ETR path, that changes how ambitious those phases are worth being from
   the start.
3. Everything from Phase 6 onward is new engineering, not just wiring —
   size it accordingly before starting, the same way
   `M3_TRACE_VERIFICATION_PLAN.md` Phase H sized the ETM-on-M3 path
   before recommending against starting it without a clear bandwidth case.
