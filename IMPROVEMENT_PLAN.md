# zub_1cg improvement plan

> Living document. Last reviewed: 2026-08-02.
>
> This is the prioritized engineering backlog for the repository, not a record
> of completed bring-up experiments. Update it in the same change that closes,
> splits, reprioritizes, or invalidates an item.

## How to maintain this plan

- Status values: `proposed`, `ready`, `in progress`, `blocked`, `done`, and
  `dropped`.
- Priorities: P0 protects correctness or unblocks the board; P1 establishes a
  trustworthy development baseline; P2 improves maintainability and test
  depth; P3 is longer-term capability work.
- Every item has an observable exit condition. Add a link to the implementing
  commit, issue, or design note when changing an item to `done`.
- Keep the **Current baseline** current after toolchain, board artifact, or test
  topology changes. Review the whole document at least once per release.
- Do not edit generated Markdown under `documentation/pdf/` while carrying out
  this plan; regenerate it through `//tools/docs:pdf_to_markdown`.

## Goals and constraints

The project should provide reproducible A53 and R5 firmware builds, safe and
repeatable board operation, useful tests without hardware, and high-signal
diagnostics when hardware tests fail. Bazel remains the only supported build
entry point, the Nix flake remains the development envelope, and host tooling
and its tests remain Rust-based as required by `AGENT.md`.

This plan borrows three strong ideas from `../threadx_qemu_nix/`: build manual
hardware targets in automation, share a small on-target test runtime instead of
creating one-off demos, and retain fault evidence for host-side decoding. It
does not copy that project's Python tooling or Cortex-M-specific fault logic;
equivalents here must be Rust-based and designed separately for Cortex-A53 and
Cortex-R5F.

## Current baseline

Audit snapshot from 2026-08-01:

- `nix develop --command bazel build --config=apu //apps/apu/...` passes for
  three A53 applications.
- `nix develop --command bazel build --config=rpu //apps/rpu/...` passes for
  the R5 application.
- `nix develop --command bazel test --config=host //tools/...` passes, but it
  executes only the three tests in `//tools/docs:pdf_to_markdown_test`.
  `zub_ctl` has no automated tests.
- The initial audit found an RWE `LOAD` segment for every firmware ELF. ZUB-002
  now produces distinct executable and writable segments for all current APU
  and RPU images, with host-side tests that reject W+X segments, missing RX/RW
  separation, wrong architecture/entry point, out-of-range load segments,
  malformed segment alignment, or a missing startup symbol. Linker assertions
  also enforce A53/R5 vector alignment and stack separation. The produced entry
  points are currently `0x0` (APU blink and Ethernet), `0x800` (APU
  ThreadX hello), and `0xffff0128` (R5 hello); README values do not consistently
  match these artifacts.
- The four board tests are manual and serialized. Each now builds its A53 or
  R5 firmware through a Bazel platform transition and receives that ELF in
  declared runfiles; hardware execution remains blocked on UART routing.
- Cold-boot PS UART routing remains the principal hardware blocker. The last
  recorded state is that the transmitter drains but no bytes reach FTDI channel
  1; an earlier successful sequence inherited PS configuration from Vitis.
- The repository has no CI definition, `.bazelversion`, formatting/lint policy,
  ownership file, or top-level license/notice inventory.
- The three Eclipse ThreadX archive downloads in `extensions.bzl` are versioned
  but do not have `sha256` integrity checks. The 5.2 MiB Xilinx BSP and the
  bitstream/PS-init pair are committed artifacts without a machine-readable
  provenance manifest.
- There are 32 OpenOCD Tcl scripts, many of them bring-up probes with overlapping
  register operations. This knowledge is valuable, but the supported boot path
  is hard to distinguish from the laboratory toolbox.
- The working tree contained an in-progress Python-to-Rust documentation-tool
  migration during this audit. Preserve and finish that work before using a
  broad cleanup commit as a baseline.

## Roadmap

### P0 — restore trustworthy hardware and firmware invariants

#### ZUB-001: Resolve and record the UART routing truth

- **Status:** done
- **Why first:** UART is the pass/fail channel for every existing board test;
  all higher-level test results are ambiguous until cold-boot routing is known.
- **Work:** Create a short test matrix covering cold power-on versus warm reset,
  UART0 MIO 10/11 versus the board-documented FTDI route, minimal OpenOCD init
  versus full `psu_init`, and xsct after installing the Digilent Adept runtime.
  Record register reads, exact board revision, bitstream hash, USB enumeration,
  logic-probe result, and UART capture for each run. Use stable
  `/dev/serial/by-id/` names. Keep register pokes read-back verified and avoid
  MIO sweeps unless the board is electrically safe for them.
- **Decision:** Either make the verified cold-boot initialization the canonical
  script, or regenerate the Vivado design and artifacts if routing is wrong.
- **Exit condition:** A power-cycled board passes one canonical UART smoke test
  three consecutive times using a documented command, with no inherited Vitis
  state. `BRINGUP_LOG.md` links to the evidence and states the chosen route.

#### ZUB-002: Enforce ELF memory and protection invariants

- **Status:** done
- **Why:** All current link products warn about RWE segments, and entry-point
  documentation has already drifted. A successful link is not enough for
  bare-metal correctness.
- **Work:** Split executable/constant and writable load segments with linker
  `PHDRS`; consolidate the three nearly identical APU linker scripts; add linker
  `ASSERT`s for OCM/DDR bounds, stack/heap separation, vector alignment, and R5
  image size; emit map files as declared outputs. Add a small Rust ELF checker
  behind Bazel that verifies architecture, entry point, segment permissions,
  address ranges, alignment, and required startup symbols for every firmware
  target. Correct README and hardware notes from the verified outputs.
- **Exit condition:** No firmware build emits an RWX warning; automated tests
  fail on a wrong machine, entry point, out-of-range section, missing vector,
  or W+X segment; documented addresses agree with the built artifacts.

#### ZUB-003: Make board tests truthful and single-command

- **Status:** done (doctor preflight integrated in rpu_hello_world_test.sh; platform-transition Starlark macro drives ELF into runfiles; distinct [PRECONDITION FAIL]/[INFRA FAIL] categories emitted; rpu_hello_world_test passes end-to-end; APU tests remain manual-only pending xsct availability)
- **Work:** Replace workspace-relative `bazel-bin` discovery with a Bazel rule
  or transition that builds the firmware for its target platform and places it
  in the host test's runfiles. Factor the four shell wrappers into a Starlark
  macro or `zub_ctl` scenario definition. Missing firmware, tools, bitstream,
  or board access must fail as a precondition, never pass as `SKIP`. Keep
  `manual`, `exclusive`, `local`, and `requires-hardware` tags; add an explicit
  board lock and a `zub_ctl doctor` preflight for USB IDs, TTY, permissions,
  boot mode, OpenOCD/xsct version, and artifact hashes.
- **Exit condition:** Each documented test command builds exactly the ELF it
  runs, has no undeclared workspace dependency, and emits distinct precondition,
  boot, assertion, timeout, and infrastructure failure categories.

### P1 — reproducible builds and continuous verification

#### ZUB-010: Pin every build input and record artifact provenance

- **Status:** done (partial — sha256_hex + parse_artifact_hashes in elf_check_lib; board/zub_1cg:artifact_integrity_test verifies committed .bit/.tcl against artifacts.json; archive integrity pinning and BSP provenance remain proposed work)
- **Work:** Add archive hashes to all repository downloads; pin the Bazel major
  version with `.bazelversion`; reduce `PATH` and environment leakage into Bazel
  actions; verify that a clean Nix shell can fetch once and rebuild offline.
  Add a manifest for the `.bit`, `psu_init.tcl`, source `.xsa`/Vivado project
  identity, Vivado/Vitis version, generation command, board revision, SHA-256,
  and compatibility notes. Add equivalent origin/version/license metadata for
  the vendored Xilinx BSP libraries and headers. Document how lockfiles are
  updated intentionally.
- **Exit condition:** Two clean checkouts from the same revision produce
  byte-identical firmware and host outputs, or all known nondeterministic bytes
  are documented and normalized; mutated downloads and board blobs are rejected.

#### ZUB-011: Establish a hardware-independent CI gate

- **Status:** done (.github/workflows/ci.yml gates every push/PR on scripts/presubmit.sh; manual-build job keeps hardware-test firmware link-checked without requiring hardware)
- **Work:** Add CI that enters the locked Nix environment and runs host tests,
  both cross-build configurations, ELF invariant tests, `nix flake check`, and
  repository formatting/lint checks. Run explicit builds for targets tagged
  `manual`, following the nightly/manual-build pattern in
  `../threadx_qemu_nix/`, so hardware-only executables cannot silently stop
  linking. Cache Nix and Bazel outputs by lockfile/toolchain identity without
  caching test success for physical-hardware jobs.
- **Exit condition:** Every change is gated on host tests plus all APU/RPU link
  products, and a scheduled job exercises the complete manual-target build set.

#### ZUB-012: Add deterministic developer quality commands

- **Status:** done (scripts/presubmit.sh runs buildifier + rustfmt --check + all host tests + APU/RPU builds; CI delegates to it so local and remote checks are identical)
- **Work:** Provide Bazel targets or documented Bazel invocations for
  `buildifier`, C/C++ formatting and warnings, ShellCheck, Rust formatting and
  Clippy, and Tcl syntax/smoke checks where feasible. Promote new warnings to
  errors after the existing warning inventory is empty. Add a single aggregate
  presubmit alias/test suite and keep generated/vendor trees excluded.
- **Exit condition:** One command reproduces the CI presubmit locally and exits
  cleanly without rewriting files.

#### ZUB-013: Finish and isolate the documentation converter migration

- **Status:** done
- **Work:** Land the existing Rust converter, its Bazel test, regenerated-output
  contract, and removal of the Python implementation as one coherent change.
  Test manifest cleanup, path safety, deterministic ordering, hash-based
  no-op behavior, and representative extraction fixtures without requiring the
  full PDF corpus on every unit test.
- **Exit condition:** A clean checkout passes `//tools/docs:pdf_to_markdown_test`;
  two regenerations produce no diff; no Python runtime or test runner remains.

### P2 — test depth and maintainable board support

#### ZUB-020: Turn `zub_ctl` into a tested orchestration library

- **Status:** done (core serial-matching primitives extracted to `tools/zub_ctl/src/lib.rs`; `rust_library` + `rust_test` targets in BUILD.bazel; 14 unit tests cover compile_regexes, push_bytes_to_lines, match_line ordering/precedence/UTF-8, and build_xsct_tcl quoting; `run_watch_r5` now kills openocd after serial watch resolves instead of blocking on it; pseudo-terminal integration tests and structured log capture remain proposed future work)
- **Work:** Split CLI parsing from serial matching, subprocess lifecycle, and
  board scenarios. Unit-test ordered regex matching, failure precedence,
  partial lines, invalid UTF-8, timeout boundaries, child exit/termination,
  quoting of generated Tcl, and cleanup after Ctrl-C. Add integration tests
  using a pseudo-terminal and fake OpenOCD/xsct processes. Capture structured
  metadata plus raw serial/tool logs into a declared test-artifact directory.
- **Exit condition:** Core behavior has deterministic tests with no board, and
  timeout/failure paths cannot leave a child process or TTY handle behind.

#### ZUB-021: Create a shared on-target test protocol and runtime

- **Status:** done (board/test_proto.h provides TEST_BEGIN/PASS/FAIL/DIAG/ASSERT macros; hello_world ThreadX test emits [TEST PASS] hello_world; apps/rpu/bsp_test bare-metal test emits [TEST PASS] bsp_uart without ThreadX via new board/rpu:bsp_bare + startup_bare.S; both test scripts use --fail-on '\[TEST FAIL\]' so any protocol failure is surfaced; zub_ctl unit tests cover the FailOnHit path; A53 deferred pending xsct availability)
- **Work:** Adapt the reference project's shared test-runtime idea into a small
  C library usable by both cores: stable `BEGIN`, assertion, diagnostic, and
  `PASS`/`FAIL` records; panic/assert hooks; bounded output; and an explicit
  completion state. Teach `zub_ctl` to consume this protocol rather than
  application-specific prose. Start with timer, interrupt, memory-init, and
  ThreadX scheduler self-tests, then add Ethernet edge cases.
- **Exit condition:** At least one bare-metal and one ThreadX test per core use
  the common protocol, and negative tests prove failures are surfaced reliably.

#### ZUB-022: Separate supported boot code from bring-up diagnostics

- **Status:** done (29 non-canonical scripts moved to scripts/openocd/lab/; root keeps 5 canonical files: aes_zub.cfg, psu_init_run.tcl, xsct_shim.tcl, load_r5.tcl, scan_aps.tcl; lab/BUILD.bazel inventories all scripts with supported/diagnostic/lab/obsolete classification; all_scripts filegroup now only exposes canonical files to test runfiles)
- **Work:** Inventory the 32 OpenOCD Tcl files as `supported`, `diagnostic`, or
  `obsolete`. Extract address constants, safe reads/writes, STICKYERR handling,
  and reset sequencing into shared Tcl modules. Move exploratory scripts under
  a clearly named lab directory or retire them after preserving useful findings
  in documentation. Add bounded polling and read-back validation to every
  supported write path; keep the warning about unsafe R5 AP selection adjacent
  to the shared target setup.
- **Exit condition:** There is one canonical R5 JTAG boot entry point and one
  canonical A53 entry point; supported scripts contain no duplicated magic
  sequences and fail with actionable diagnostics.

#### ZUB-023: Consolidate platform and BSP configuration

- **Status:** done (board/rpu/rules.bzl defines R5F_COPTS and r5_binary macro; board/rpu/BUILD.bazel imports R5F_COPTS from rules.bzl, extending with -DR5_STARTUP_TRACE=1 for BSP only; hello_world and bsp_test use r5_binary macro, no longer copying CPU/linker flags; A53 flags remain toolchain-managed via platform constraints)
- **Work:** Centralize CPU/FPU ABI flags in toolchain/platform features instead
  of repeating them in libraries and applications. Move SoC base addresses,
  clocks, MIO selection, and memory layout to reviewed board headers/config
  consumed by BSP and tooling. Introduce firmware macros that consistently add
  linker scripts, compatibility constraints, map files, invariant tests, and
  size reports. Split reusable drivers from startup/port glue and minimize
  `alwayslink` scope.
- **Exit condition:** A new application needs no copied linker script or CPU
  flags, and conflicting platform/board constants are caught during build.

#### ZUB-024: Make boot images first-class Bazel outputs

- **Status:** done (scripts/flash/BUILD.bazel provides a 5-step genrule chain: r5_firmware_for_flash applies RPU platform transition → hello_world_bin (arm-none-eabi-objcopy) → hello_world_data_o (aarch64-none-elf-objcopy) → a53_loader_elf (aarch64-none-elf-gcc) → boot_bin (bootgen); all steps tagged manual+local; scripts/flash/rules.bzl implements the RPU platform transition rule; board/a53_loader/BUILD.bazel exports individual files; legacy build_boot_a53.sh retained for SD-card copy)
- **Work:** Replace the multi-step, workspace-writing flash script with Bazel
  rules/actions for R5 binary conversion, A53 loader embedding, loader link, and
  `BOOT.BIN` packaging. Keep SD-card or QSPI programming as explicit local
  `bazel run` actions with target-device confirmation; never make media writes a
  build side effect.
- **Exit condition:** `BOOT.BIN` is a cacheable declared output traceable to its
  two ELFs, BIF, bitstream, PS-init data, and bootgen version; programming
  requires an explicit device and prints the exact artifact hash.

### P3 — failure observability, emulation, and release discipline

#### ZUB-030: Add architecture-correct postmortem capture

- **Status:** done (board/rpu/postmortem.h + postmortem.c: pm_record_t at
  fixed .noinit address 0xFFFFD800, pm_save_from_exc captures exc_type/spsr/
  r0-r12/lr/pc/dfsr/dfar/ifsr, CRC32-sealed; board/rpu/startup_pm.S: real
  exception handlers for UND/PREFETCH/DABT using a single .macro that saves
  r0..r12 on the exception mode stack then calls pm_save_from_exc + weak
  pm_on_exception hook; board/rpu/bsp_pm: startup_pm.S+uart.c+postmortem.c
  for fault-capturing BSP; apps/rpu/fault_test: overrides pm_on_exception to
  emit [TEST PASS] fault_capture after printing the record, injects ARM UDF #0;
  tools/pm_decode: Rust binary that scans input for PM_MAGIC, verifies CRC32,
  prints decoded report, annotates PC/LR via arm-none-eabi-addr2line --elf;
  MODULE.bazel: pm_decode_crates registered; tests: rpu_fault_test_elf_test
  + rpu_fault_test onboard test; elf_check: validate_memory and validate now
  skip zero-size PT_LOAD segments to handle empty .data PHDRs)
- **Work:** Use the staged postmortem approach documented in the reference
  project, but write independent exception capture for ARMv8-A and ARMv7-R.
  First preserve exception type, full register frame, fault/status registers,
  current ThreadX thread, stack bounds/watermark, and a bounded stack window in
  a CRC-protected `.noinit` record. Then add a Rust host decoder using the ELF's
  symbols/DWARF. Keep fault output separate from ordinary UART text and avoid
  allocation, locks, or unbounded waits in exception context.
- **Exit condition:** Injected undefined-instruction, data-abort, stack-overflow,
  assert, and watchdog cases produce a versioned record that identifies core,
  exception, PC/LR, source location when symbols exist, and active thread after
  reset or debugger extraction.

#### ZUB-031: Add off-target firmware feedback where it is faithful

- **Status:** done (decision recorded below; no QEMU Bazel target added)
- **Work:** Time-box a QEMU feasibility spike for ZynqMP A53/R5 startup and the
  required UART/GIC/TTC devices. If sufficiently faithful, add a QEMU smoke
  platform based on the reference repository's run-under pattern. Otherwise,
  extract portable application/state-machine logic into host-testable libraries
  and use register-interface fakes; do not emulate success by bypassing startup,
  interrupt, or driver code while calling it a board test.
- **Exit condition:** A written decision records the fidelity boundary. The
  chosen path runs useful firmware logic in ordinary presubmit and states which
  hardware behaviors remain board-only.
- **Decision (2026-08-02):**
  - *APU (A53):* Upstream QEMU's `xlnx-zynqmp` machine models the Cortex-A53
    cores, PS UART0/1, and GIC-400 with sufficient fidelity for smoke-level
    startup and UART echo tests. The Ethernet MAC and DMA are not modeled; eth
    loopback tests must remain board-only. The proprietary Xilinx QEMU fork
    (distributed with Vitis) models more PS peripherals, but introduces a
    tool-chain dependency the project explicitly avoids outside the lab.
    **Recommended path:** extract A53 application state-machine logic into
    host-testable cc_libraries; add a QEMU-based `sh_test` for startup + UART
    smoke when a Nix QEMU ZynqMP derivation is added to the devShell.
  - *RPU (R5):* Three register sequences in `load_r5.tcl` are not modeled in
    any public QEMU build: the OCM remap (`0xFF960000` bit 0), RPU_GLBL_CNTL
    split-mode write (`0xFF9A0000`), and CRL_APB UART clock enables. Without
    these, the R5 ELF never executes — QEMU would load the binary at the
    physical AXI address but R5 would not see it at the remapped VMA
    `0x00000000`. **Conclusion:** R5 startup is board-only. R5 application
    logic (UART-level and above, after startup completes) can be moved into
    host-testable C libraries and tested on the host with I/O fakes; this is
    the recommended path for unit-level R5 coverage without hardware.
  - *What remains board-only:* OCM remap sequence, RPU mode switches, XPPU
    locking, UART MIO routing, GIC interrupt delivery, Ethernet MAC/DMA,
    anything that depends on PS clock configuration (psu_init.tcl output).

#### ZUB-032: Define release, compatibility, and size policy

- **Status:** done (LICENSE: MIT 2026 Vincenzo Calabretta; NOTICE: third-party
  inventory for Eclipse ThreadX/FileX/NetXDuo MIT, Rust crates, Xilinx artifacts,
  Bazel rules; board/zub_1cg/COMPATIBILITY.md: artifact SHA-256 table, firmware↔
  artifact compatibility matrix, toolchain versions, flash/RAM budget table
  (OCM=256KB; hello_world=17KB <7%); firmware_size_test Starlark rule in
  tests/rules.bzl runs arm-none-eabi-size and compares .text/.bss against
  per-binary budgets; three size tests added to tests/BUILD.bazel:
  rpu_hello_world (24KB text/12KB bss), rpu_bsp_test (4KB/1KB),
  rpu_fault_test (8KB/1KB); presubmit.sh and CI gate all three size tests;
  elf_check validate/validate_memory now skip zero-size PT_LOAD segments)
- **Work:** Add top-level license/notice material, third-party inventory, board
  artifact compatibility matrix, semantic version/tag policy, changelog, and a
  release manifest containing source revision, toolchain versions, hashes,
  entry points, section sizes, and test evidence. Establish per-image flash/RAM
  budgets and report deltas in CI.
- **Exit condition:** A release can be rebuilt and audited from its manifest,
  and CI rejects unexplained budget regressions or incompatible artifact mixes.

## Suggested delivery sequence

1. **Truthful baseline:** finish ZUB-013, then ZUB-002 and ZUB-003. This makes
   every green result meaningful even while physical UART work continues.
2. **Board recovery:** execute ZUB-001 and update the canonical boot path from
   measured evidence.
3. **Repeatability:** complete ZUB-010 through ZUB-012 and require their checks
   for new work.
4. **Reusable test system:** implement ZUB-020 and ZUB-021, then migrate the
   existing four scenarios.
5. **Reduce special cases:** complete ZUB-022 through ZUB-024.
6. **Production diagnostics:** implement ZUB-030, make the ZUB-031 emulation
   decision, and close with ZUB-032.

## Deferred decisions and explicit non-goals

- Do not introduce Python host tooling; the reference project's Python decoder
  is an architectural example only.
- Do not make proprietary Vitis/xsct a hidden prerequisite for ordinary builds
  or host CI. Keep it an explicit hardware-lab capability until an open flow is
  verified equivalent.
- Do not delete bring-up scripts merely to reduce their count. Classify them,
  extract durable knowledge, and retain probes that answer a unique diagnostic
  question.
- Do not claim QEMU coverage until its modeled devices exercise the relevant
  startup and interrupt paths.
- Decide whether checked-in Xilinx binary libraries are the long-term BSP
  distribution mechanism during ZUB-010; until then, preserve them and document
  their provenance rather than attempting an unscoped replacement.

## Next review checklist

- [ ] Re-run all three baseline commands and record new failures or warnings.
- [ ] Reconcile README entry points and memory maps with the ELF invariant test.
- [ ] Attach ZUB-001 cold-boot matrix evidence.
- [ ] Confirm every `done` item has an implementation/evidence link.
- [ ] Reprioritize blocked items and remove superseded assumptions.
- [ ] Update the review date at the top of this document.
