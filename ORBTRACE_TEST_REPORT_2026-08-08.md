# Orbtrace application — analysis and test report — 2026-08-08

## Executive result

Orbtrace's host-verifiable test suite is healthy (**24/24 tests pass**), and its
RTL/PL side is now verified end-to-end against real Vivado 2023.2 on this
machine: a full synthesis → implementation → bitstream → XSA → `psu_init.tcl`
run succeeds and passes every gate `build.tcl` enforces.

**Orbtrace cannot be deployed to hardware in its current state.** This is a
repo-level gap, not an environment or network problem: the on-target firmware
crates were never wired into bootable images. See "Deployment blocker" below —
this is the top-priority item to continue this work.

Two real bugs from the `orbtrace/` → `applications/orbtrace/` reorg
(commit `ed1751f`, "Reorganize project structure") were found and fixed in
this session (see "Fixes applied").

## Snapshot and provenance

| Item | Value |
|---|---|
| Repository | `zub_1cg` |
| Commit at session start | `ed1751f Reorganize project structure` |
| Date / timezone | 2026-08-08, Europe/Rome |
| Board USB device | FTDI FT2232H, USB ID `0403:6010` — connected, confirmed via `lsusb` |
| Vivado | 2023.2, installed at `/home/v/opt/vitis/Vivado/2023.2`, run via the FHS bwrap wrapper at `/nix/store/d0k9ac9rqizjgl7mdb1s1jxadbc38pj7-vivado-bwrap` |
| Vitis / xsct | 2023.2, at `/home/v/opt/vitis/Vitis/2023.2/bin/xsct` |
| Bazel | 8.7.0 (via `nix develop`) |

## Fixes applied this session

### 1. `applications/orbtrace/rtl/rtl_unit_test.sh`

Hardcoded the pre-reorg runfiles path:

```sh
rtl_dir="${TEST_SRCDIR}/${TEST_WORKSPACE}/orbtrace/rtl"
```

Verified against the actual Bazel runfiles tree
(`.../rtl_unit_test.runfiles/_main/applications/orbtrace/rtl` exists;
`.../_main/orbtrace/rtl` does not) — this would fail immediately in any
environment able to run it at all. Fixed to
`${TEST_SRCDIR}/${TEST_WORKSPACE}/applications/orbtrace/rtl`.

### 2. `applications/orbtrace/vivado/build.tcl`

`repo_dir` was computed one directory level too shallow post-reorg
(`[file join $script_dir ../..]` from `applications/orbtrace/vivado` lands at
`applications/`, not the repo root). The RTL source glob
(`[file join $repo_dir orbtrace rtl *.sv]`) accidentally still resolved to the
correct absolute path because the missing `applications/` prefix canceled the
shallow `repo_dir` — but `output_dir` (`bazel-out/orbtrace-vivado`) landed at
`applications/bazel-out/orbtrace-vivado` instead of the repo-root `bazel-out/`.

Fixed `repo_dir` to `[file join $script_dir ../../..]` and added the
`applications` path segment back into the RTL glob/header joins so both the
sources and the output directory resolve correctly.

## Host-verifiable test suite — all pass, run fresh (not cache replay)

```sh
nix develop -c bazel test --config=host --cache_test_results=no \
  //applications/orbtrace/model:orbtrace_model_test \
  //applications/orbtrace/model:register_schema_test \
  //applications/orbtrace/firmware/common:firmware_common_test \
  //applications/orbtrace/firmware/vexriscv:trace_workload_test \
  //applications/orbtrace/firmware/a53:control_firmware_test
```

| Target | Result | Test count |
|---|---:|---:|
| `orbtrace_model_test` | PASS | 11 |
| `register_schema_test` | PASS | (shell) |
| `firmware_common_test` | PASS | 4 |
| `trace_workload_test` | PASS | 1 |
| `control_firmware_test` | PASS | 8 |

## RTL simulation (XSim) — partially blocked by environment, not code

`bazel test //applications/orbtrace/rtl:rtl_unit_test --test_env=XILINX_VIVADO=...`
fails on this host regardless of sandboxing (`--spawn_strategy=local` too):
Vivado's bundled scripts (`xvlog`, etc.) have a `#!/bin/bash` shebang, and bare
NixOS has no `/bin/bash` outside an FHS chroot. This is a gap in the local
Vivado FHS environment definition, not a repo issue.

Working around it by driving `xvlog`/`xelab`/`xsim` manually inside the Vivado
FHS bwrap chroot:

- `xvlog` compiled all 9 RTL/testbench files cleanly — **zero errors**.
- `xelab` failed with `GNU binutils package not found` — a missing package in
  the local `vivado-fhs` Nix derivation's `targetPkgs` (not part of this
  repo). Elaboration/simulation pass/fail from the actual testbenches
  (`orbtrace_capture_tb`, `orbtrace_dap_tb`, `orbtrace_pipeline_tb`) is
  **still unverified** — fixing the Nix FHS env's `binutils` dependency is a
  prerequisite to close this out.

## Vivado synthesis/implementation — succeeded end-to-end

Ran the corrected `build.tcl` against real Vivado 2023.2 (~8 minutes,
xczu1cg-sbva484-1-e):

```sh
XILINX_VIVADO=/home/v/opt/vitis/Vivado/2023.2 \
XSCT=/home/v/opt/vitis/Vitis/2023.2/bin/xsct \
  <vivado-fhs-bwrap-wrapper> -mode batch -source \
  applications/orbtrace/vivado/build.tcl
```

Result: **all of `build.tcl`'s own release gates passed.**

| Metric | Value | Gate |
|---|---:|---|
| WNS (setup slack) | +2.455 ns | must be ≥ 0 |
| WHS (hold slack) | **+0.015 ns** | must be ≥ 0 — passes, but only 15 ps of margin |
| Critical CDC findings | 0 | must be 0 |
| Critical methodology findings | 0 | must be 0 |
| Synthesis/implementation errors | 0 | must succeed |

Artifacts produced at `bazel-out/orbtrace-vivado/` (independently re-hashed
one to confirm the manifest is accurate):

| Artifact | Size | SHA-256 |
|---|---:|---|
| `zub_orbtrace.bit` | 2,970,744 B | `9da1d2cadc12134bcfeb68570ed44d2641491f6392f1fd5311d42efb5730e3d8` |
| `zub_orbtrace.xsa` | 1,501,949 B | `ed5156f71003578b9a08dec6b273f2ad40c2d73c02b1534071e207015adc5bfe` |
| `psu_init.tcl` | 610,818 B | `6edbd912927a5e75a1a5161f154b565e79f5024c2f1d2ceddecd7da9ab924b18` |

**Not yet flashed to the board.** This is a first-time hardware bring-up of a
brand-new PL design with no consumer firmware able to exercise it (see below),
so there's nothing meaningful to observe on real silicon beyond "did it
configure" — didn't think that risk (plus the razor-thin 15 ps hold margin)
was worth taking without an explicit decision to do so.

## Deployment blocker (top priority to continue this work)

**The on-target Orbtrace firmware cannot be built into a bootable image at
all**, independent of any bitstream/network/environment concern. Compare
`applications/apu/blink/BUILD.bazel`, which uses the `a53_firmware(...)`
macro (`sdk/rules/firmware.bzl`) to produce a real cross-compiled,
loadable ELF:

```python
load("//sdk/rules:defs.bzl", "a53_firmware")
a53_firmware(name = "blink", srcs = ["src/main.c"], ...)
```

against Orbtrace's three firmware `BUILD.bazel` files, which are all just:

```python
rust_library(name = "...", srcs = ["src/lib.rs"], ...)
rust_test(name = "..._test", crate = ":...")
```

- `applications/orbtrace/firmware/a53` — the control-service logic
  (framing, DMA-ring, CMSIS-DAP mailbox handling) — is a `#![no_std]`
  library with **no linker script, no entry point/vector table, no
  `firmware()` macro wiring**. It is unit-tested on the host but never
  cross-compiled into something bootable.
- `applications/orbtrace/firmware/common` — same situation, shared
  framing/DMA logic.
- `applications/orbtrace/firmware/vexriscv` — same situation, and doubly
  blocked: it targets RV32IMAC, and **`sdk/toolchains` has no RISC-V
  toolchain at all** (only `aarch64_none_elf` and `arm_none_eabi`). The
  "VexRiscv Rust workload boots" acceptance criterion in
  `applications/orbtrace/TESTING.md` could not be built even if a
  `firmware()`-style macro existed for it today.

The only genuinely executable artifact Orbtrace produces is
`applications/orbtrace/model`'s `orbtrace` `rust_binary` — the **host-side
CLI/client** used by `tests/orbtrace_throughput_test.sh`, not the board-side
service it's supposed to talk to.

This explains why the 2026-08-07 on-target report
(`ON_TARGET_TEST_REPORT_2026-08-07.md`) found the Orbtrace control service
unavailable at `192.168.1.50:3401`, and why re-running that test today would
show the same thing: there has never been a way to load a board-side service
in the first place. It is not a missing deployment step or network
misconfiguration.

## Follow-up session — A53 service implemented and flashed to real hardware

Everything below happened after this report was first written, in the same
calendar day. The A53 control service now boots on real hardware and its
network stack partially works; full Orbtrace throughput is still blocked,
now on an RGMII hardware-timing issue rather than missing software.

### What was built (all host-verified, `bazel test --config=host` green)

- **New Rust→aarch64-bare-metal toolchain** (`sdk/toolchains/aarch64_unknown_none/`).
  `rules_rust` 0.62.0 does not list `aarch64-unknown-none` as a supported
  triple, so `rust.toolchain(extra_target_triples=...)` cannot fetch it.
  Vendored `rust-std-1.86.0-aarch64-unknown-none` directly (pinned sha256 in
  `MODULE.bazel`) and wired a manual `rust_toolchain`.
- **`applications/orbtrace/firmware/a53/src/ffi.rs`** — the extern "C"
  boundary (`orbtrace_control_feed`, `orbtrace_dap_feed`) letting C hand raw
  TCP bytes to the existing, already-tested Rust protocol state machine.
  Built as a `rust_static_library` (`control_firmware_a53` target).
- **`applications/orbtrace/firmware/a53_app/`** — new ThreadX + NetX Duo
  service: brings up GEM2 networking, then runs TCP server threads on 3401
  (control) and 3240 (DAP) that pump bytes through the Rust FFI. Produces a
  valid, JTAG-loadable A53 ELF (`//tests:orbtrace_a53_app_elf_test`,
  `//tests:orbtrace_a53_app_test`).
- Fixed two **pre-existing, unrelated** repo bugs found along the way:
  `third_party/netxduo/BUILD.bazel`'s glob (`common/src/nx_*.c`) silently
  excluded all 132 `common/src/nxe_*.c` error-checking wrapper files —
  nothing had exercised NetX Duo's checked API surface before this session.
  `tooling/xsct/jtag_flash.sh` had stale pre-reorg default paths
  (`board/zub_1cg/...` → `sdk/boards/zub_1cg/...`).

### On-target debugging: two real firmware bugs, one open hardware issue

Flashed via `tooling/xsct/jtag_flash.sh` / `zub_ctl watch-a53` with the
already-built `bazel-out/orbtrace-vivado/zub_orbtrace.bit` +
`sdk/boards/zub_1cg/psu_init.tcl` (the shared board PS8 config — the
Orbtrace Vivado project's own generated `psu_init.tcl` has **zero**
GEM-related register writes, so it must not be used for this). Host side
connected via a USB-Ethernet adapter on `192.168.1.1/24`.

1. **GEM2 was hardcoded to 100 Mbps** (`ThreadXGEM2Driver.c`), but the
   board's Microchip KSZ9131RNXC PHY (confirmed via schematic:
   `internal/documentation/pdf/AES-ZUB-1CG-DK-G-Rev1_SCH_2022-08-05/`) always
   negotiates Gigabit with any real link partner, and the project's own 400
   Mbit/s throughput target is unreachable at 100 Mbps regardless. Fixed by
   calling `XEmacPs_SetOperatingSpeed(&sCtx.mac, 1000U)`.
2. **NetX Duo sends ARP replies via a driver command our driver never
   handled.** `_nx_arp_packet_receive` (netxduo `common/src/nx_arp_packet_receive.c`)
   issues `NX_LINK_ARP_RESPONSE_SEND` (also `NX_LINK_ARP_SEND` /
   `NX_LINK_RARP_SEND` for outbound requests), which `nx_driver_gem2()`'s
   switch fell through to `default:` and silently dropped. Fixed by routing
   all three to the existing `gem2_packet_send()` path. This alone took the
   board from "receives ARP requests, never replies" to "attempts a reply
   for every request" — confirmed via new diagnostic counters
   (`gem2_diag_get()`, temporary instrumentation left in place, see below).
3. **Remaining open issue: RGMII TX timing.** With (1) and (2) fixed, RX
   works cleanly (frames received, ISR fires, NetX processes them) but no
   reply ever reached the host — confirmed with `tcpdump` on the host NIC
   (zero bytes received across many tests) despite the GEM2 DMA engine
   itself reporting every TX descriptor complete with no error bits
   (`tx_complete` counter, `last_tx_stat=0x8040801C`). Researched the
   Microchip KSZ9131RNX datasheet (DS00002841D §4.9.3.1/§5.3.70): the PHY's
   RXC delay DLL is enabled by default but its **TXC delay DLL is disabled
   by default** (`bypass_txdll=1` in the TX DLL Control Register, MMD
   device 2h / register 0x4Dh). Implemented an MDIO fix in
   `gem2_phy_enable_tx_delay()` (PHY auto-discovered at address 7 via
   clause-22 ID scan, indirect MMD access via direct registers 0x0D/0x0E
   per §5.3): explicitly set both RX and TX DLL bypass bits deterministically
   (the PHY chip is **not** reset by JTAG/PS resets, only a full power
   cycle, so a mid-session control experiment — deliberately bypassing RX
   delay to confirm these registers have real effect on this link, which it
   did: RX immediately dropped to 0 frames — left stale state across
   several reflashes and had to be explicitly reverted), then force a BMCR
   auto-negotiation restart (clause-22 registers 0/1) so the new DLL
   settings apply at link (re)establishment rather than being hot-swapped
   under an already-running link. **Result: a frame from the board reached
   the host for the first time this session** (`tcpdump` captured a frame
   from a source MAC that is a nibble/byte-rotated version of the board's
   real MAC), but the payload is corrupted — the textbook signature of an
   RGMII skew value that's enabled but not precisely tuned, not an on/off
   problem anymore. Stopped here by user decision; next step is either
   scope/protocol-analyzer-based skew tuning, or disabling DLL auto-tune
   for a manually swept fixed `txdll_tap_sel` value (§5.3.70,
   `TX_DLL_CTRL` bits `[11:6]`, default `1Bh`) and/or the Clock Pad Skew
   Register (MMD 2h / 0x08h) fine trim.

### Temporary instrumentation left in the tree

`applications/orbtrace/firmware/a53_app/src/main.c`'s `diag_thread_entry`
and `third_party/os_abstraction_layer/ThreadX/ThreadXGEM2Driver.c`'s
`gem2_diag_get()`/`diag_*` counters print live RX/TX/ISR state once per
second over UART — genuinely useful for continuing the skew investigation,
not meant to ship long-term. Remove once RGMII TX is confirmed working.

### Explicitly out of scope this session

TCP 3402 trace-payload streaming (AXI DMA ring), the RISC-V toolchain for
the VexRiscv workload, and the Vivado FHS `binutils` gap (RTL XSim) from
the original report are all still untouched.

## Suggested next steps, in order

1. **Wire up a bootable A53 image for the Orbtrace control service.** Add a
   `rust_binary`/no-std entry point + linker script for
   `applications/orbtrace/firmware/a53`, following the pattern
   `sdk/rules/firmware.bzl` already establishes for the C-based `apu_a53`
   apps (or extend `firmware.bzl` to support Rust `no_std` binaries if that
   doesn't exist yet — check `sdk/rules/firmware.bzl` and
   `sdk/rules/defs.bzl` before assuming either way).
2. **Add a RISC-V (`riscv32imac` or similar) toolchain** to
   `sdk/toolchains/` before `firmware/vexriscv` can be built at all, plus a
   corresponding Bazel platform under `sdk/platforms` and firmware macro
   wiring, mirroring `apu_a53`/`rpu_r5_0`.
3. **Fix the local Vivado FHS environment's missing `binutils`** (the Nix
   flake that built `/nix/store/...-vivado-fhs`, not this repo) so
   `//applications/orbtrace/rtl:rtl_unit_test` can actually elaborate/run
   its three XSim testbenches and confirm the RTL functionally, not just
   syntactically.
4. Once (1) is done: add a JTAG/XSCT load path for the Orbtrace bitstream +
   A53 image (mirroring `tooling/openocd/load_r5.tcl` /
   `tooling/xsct/load_a53.tcl`), then flash and boot for real — this is when
   the freshly-built `bazel-out/orbtrace-vivado/zub_orbtrace.bit` and
   `psu_init.tcl` become usable, and yesterday's/today's blocked
   `//tests:orbtrace_throughput_test` becomes attemptable on real hardware.
5. Watch the **15 ps hold margin** (WHS) on future RTL changes to this
   design — it currently passes `build.tcl`'s `< 0` gate but has almost no
   room before a routing/placement change could flip it negative.
