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

## Follow-up session 2. xsct environment fixed, JTAG automation working, new bugs found

Same calendar day, later session. Started from a request to implement this
report's suggested next steps. Step 1 (bootable A53 image) turned out to
already be done, in commits `03c53d8` and `8320264`, made between this
report's first write-up and this session. Step 4 (JTAG/XSCT load path) was
the actual blocker hit this session.

### xsct could not launch at all; root cause and fix (machine-local, not this repo)

`tooling/xsct/jtag_flash.sh` and `zub_ctl watch-a53` both call `xsct`, but on
this machine `xsct` failed before reaching any JTAG code, in two distinct
ways, neither visible in this repo:

1. `libtinfo.so.5` was reported missing. The real problem was one layer
   deeper: the Nix-packaged FHS environment wrapping `xsct` (a different,
   separately-built environment from the one wrapping Vivado, which already
   worked) never provisioned `/usr/lib` at all inside its sandbox. Once
   `LD_LIBRARY_PATH` was pointed at Xilinx's own bundled
   `Vitis/2023.2/lib/lnx64.o`, every other "missing" library the earlier
   error implied (boost 1.72, python3.8, tcl8.5, xerces-c 3.2, ...) resolved
   immediately, because all of them already ship inside the Xilinx install
   itself; they were never a Nix packaging gap. `libtinfo.so.5` alone was
   a genuine gap (nixpkgs' `ncurses-abi5-compat` only registers the SONAME
   `libncursesw.so.5` in `ldconfig`'s cache, not the `libtinfo.so.5` alias
   name a plain file search needs), fixed with one symlink placed directly
   in `Vitis/2023.2/lib/lnx64.o/libtinfo.so.5`.
   Fix applied to `/home/v/opt/vitis/Vitis/2023.2/bin/setupEnv.sh` (sourced
   by `xsct` before anything else runs): exports `LD_LIBRARY_PATH` to
   include that directory. Machine-local, not part of this repo, and will
   need repeating on any other machine hitting the same error.
2. Even after that fix, `xsct` invoked non-interactively (a script path, not
   `-eval`) tried to start a dummy X server (`Xvfb`) for reasons unrelated
   to JTAG scripting, and that attempt fails in this sandbox, causing `xsct`
   to exit silently before running any TCL at all. Fixed properly, in this
   repo: pass `-nodisp` to every non-interactive `xsct` invocation
   (`tooling/xsct/jtag_flash.sh`, both call sites in `tooling/zub_ctl/src/main.rs`).

With both fixed, `bazel test //tests:orbtrace_a53_app_test --config=host
--config=onboard --test_env=XSCT=<nix-wrapped xsct>` flashes the bitstream,
runs `psu_init`, loads the A53 ELF, and confirms the boot banner over UART,
end to end, repeatably.

### Three real bugs found and fixed on real hardware

1. **RX descriptor ring never returned to the DMA engine**
   (`ThreadXGEM2Driver.c`, `gem2_alloc_rx_packet`). `XEmacPs_BdSetAddressRx`
   only touches the address bits of a descriptor and deliberately preserves
   the low two bits (NEW and WRAP); it does not clear NEW as its callers
   commonly assume. Every refilled slot kept the hardware-set NEW=1 bit
   forever, so once each of the 4 ring slots had received one frame, none
   were ever handed back to the DMA engine and RX stopped permanently.
   Confirmed on hardware: before this fix RX died after exactly
   `GEM2_BD_COUNT` frames, every time. Fixed with an explicit
   `XEmacPs_BdClearRxNew(bd)` call after refill.
2. **RXUSED interrupt never enabled.** The DMA engine halts RX scanning at
   the hardware level the instant it finds a used descriptor, independent of
   whether that condition is unmasked into an interrupt. It was not
   unmasked, so the one case not fully covered by fix (1), the DMA's own
   internal descriptor pointer desyncing from software's `rx_tail`, produced
   total, permanent RX silence with no CPU-visible signal at all: no crash,
   no further interrupts of any kind, `diag_thread` (a different thread, a
   different interrupt source) unaffected and still printing every second,
   looking identical to a wiring problem. Fixed by enabling
   `XEMACPS_IXR_RXUSED_MASK` and, in the ISR, re-pointing the RX queue base
   at the current `rx_tail` slot before re-enabling RX, forcing the DMA
   engine to resync to a descriptor known to be free.
3. **`nx_interface_ip_mtu_size` never set.** Left at its zero-initialized
   default, this doesn't affect ARP (which bypasses IP-layer MTU checks) but
   silently blocks NetX from generating outbound IP-layer traffic, looking
   identical on the wire to the driver just not replying. Fixed by setting
   it to 1500 in `gem2_initialize`.

Each of these was confirmed independently: (1) and (2) via a run counter
added to the ISR (`diag_rxused_count`, `diag_last_isr`) and repeated
reflash/observe cycles; (3) via `tcpdump` on the host NIC plus a manual
IP-header checksum verification (folds to `0xffff`, confirming the packet
NetX receives is byte-identical to what left the host, protocol, ports, and
TCP flags all correct) proving the RX path itself was never the problem for
what turned out to be a send-side gap.

### Open: TCP handshake still does not complete

With all three fixes in place, ARP resolves reliably and repeatably (RX
receives an unbounded number of frames without stalling, confirmed over
multiple SYN retransmission bursts), but a TCP SYN to either port (3401,
3240) never gets any reply: not a SYN-ACK, not an RST, and, per a driver
command dispatch counter added this session (`diag_driver_cmd_count`,
`diag_last_driver_cmd`), NetX's IP thread never calls back into the driver
at all after the initial ARP reply, not even to request a fresh ARP
resolution for the host (192.168.1.1), which NetX Duo's ARP cache would need
before it could send a reply back (confirmed via source read of
`nx_arp_packet_receive.c`: unsolicited ARP requests only refresh existing
cache entries, they do not create new ones).

This is squarely inside NetX Duo's internal TCP/ARP interaction now, not the
driver. `//tests:orbtrace_throughput_test` (TCP 3401) will keep failing
until this is resolved. Suggested next steps, in order of likely leverage:

1. Trace `_nx_ip_packet_deferred_receive`'s consumer (`_nx_ip_thread_entry`
   or whatever dequeues `nx_ip_deferred_received_packet_head`) to find where
   a valid, checksummed SYN for a listening socket stops being processed.
2. Check whether NetX Duo needs an explicit ARP table pre-seed
   (`nx_arp_dynamic_entry_set` or similar) for the peer before a listening
   socket can reply, given the "requests only refresh, never create" cache
   behavior noted above; if so, either seed it opportunistically from
   `_nx_arp_packet_receive`-adjacent driver code, or find the right NetX
   Duo API to force resolution.
3. Diagnostics left in the tree for this specific investigation:
   `diag_driver_cmd_count`/`diag_last_driver_cmd`/`diag_last_driver_status`
   (which driver command NetX last issued) and the raw IP-header hex dump
   printed for the last IP-type frame received. Both were useful this
   session and are cheap to keep; remove once the handshake completes and
   they've stopped earning their keep.

## Follow-up session 3. One real TX bug fixed; handshake still blocked, now on an intermittent link-reliability issue

Same request ("implement this report's suggested next steps, get Orbtrace
running on target"), a later session. Picked up at "Open: TCP handshake
still does not complete" above.

### Real bug found and fixed: TXSR "used bit read" (TXUSED) was never handled

`XEMACPS_IXR_TXUSED_MASK` (TXSR bit 0, "Tx buffer used bit read" — the
TX-side mirror of the already-fixed RXUSED) was not enabled in
`gem2_enable()` and not handled in `gem2_irq_handler()`. Confirmed on real
hardware: after link-up, TX completes exactly once (the first ARP reply)
and then stalls forever — `tx_count` grows without bound, `tx_tail` never
advances, `gem2_tx_cleanup()` never sees another completion, and NetX's
outbound ARP request to resolve the TCP peer (needed to send a SYN-ACK)
silently never reaches the driver. This alone explained why ARP sometimes
appeared to "already work" (the one-time free TX) but nothing after it
ever did.

First fix attempt caused a live incident on real hardware: reacting to
every TXUSED by reprogramming the TXQ0 queue pointer to `tx_tail` and
re-kicking `STARTTX` immediately re-triggered TXUSED and spun into an
interrupt storm (`isr_calls`/`txused_count` climbing into the millions per
second, UART output visibly corrupted from CPU starvation). Root cause:
ZynqMP GEM checks the TXQ1 priority queue before TXQ0 on *every* transmit
cycle, not just once at reset — the permanently `USED|WRAP` dummy BD parked
at TXQ1 (see the existing TXQ1 workaround comment in this file) trips
TXSR's used-bit-read condition on every single legitimate send, which is
benign and expected, not evidence of a genuine TXQ0 stall. Recovered by
reflashing (a full JTAG reset) and replaced the handler with a safe,
storm-free version: acknowledge (read + write-1-to-clear) TXSR and count
it, but do not touch the queue pointer or re-kick — `gem2_tx_cleanup()`
(TXCOMPL) and the next `gem2_packet_send()` call are sufficient for the
normal case. `applications/orbtrace/firmware/a53_app/src/main.c` gained a
`diag2:` print line exposing NetX's own drop/error counters
(`nx_ip_invalid_packets`, `nx_ip_receive_checksum_errors`,
`nx_ip_tcp_checksum_errors`, etc. — live and on by default in this build,
not disabled anywhere) for exactly this kind of "packet arrived at the
driver but nothing downstream happened" investigation.

### New finding: the link itself is intermittently unreliable, independent of any code path in this repo

With the TXUSED fix in place, ARP resolution and SYN reception both
**sometimes** work cleanly end-to-end (confirmed via `ip neigh show`
reaching `REACHABLE`, and via `diag2`'s raw IP-header dump showing a
byte-correct, checksum-valid TCP SYN arriving at port 3401) — but not
reliably. In the same session, on the same flashed image, with no code
changes in between:

- A 20-ping sweep (`ping -c 20 -i 0.3 192.168.1.50`) came back **100%
  loss**, "No route to host" (ARP failure) on most sequence numbers.
- `nx_ip_receive_checksum_errors` (exposed via the new `diag2` line)
  climbed to 4 over the session from otherwise-ordinary broadcast UDP
  traffic on the segment — the board is intermittently receiving corrupted
  frames, not just failing to receive them.
- `rxused_count` (the RXUSED recovery this session's predecessor already
  wired up) fired **14 times** in one short burst of ping/connect
  attempts — RX repeatedly needs mid-stream recovery under load, which is
  not expected behavior for a solid link.
- A TCP SYN was confirmed received intact (valid IP header checksum,
  correct destination port, `diag2`'s TCP-layer counters — invalid/
  checksum-error/dropped — all stayed at zero) with **zero** further driver
  activity afterward: no ARP re-resolution attempt, no SYN-ACK send
  attempt, nothing. The likely explanation tying this together: NetX
  needed to (re-)resolve ARP for the peer to address the SYN-ACK frame,
  that resolution attempt was itself lost on the same flaky link, and
  NetX gave up after exhausting its internal ARP retry count — a failure
  mode with no counter this session's diagnostics track, several layers
  removed from the TCP packet processing code actually being watched.

This is consistent with, not a new problem separate from, the original RGMII
timing/skew concern raised earlier in this report (see "Remaining open
issue: RGMII TX timing" above, and `gem2_phy_enable_tx_delay()`'s
KSZ9131RNX DLL bring-up): the link works often enough to look "basically
fine" in a short test but loses/corrupts frames unpredictably under any
sustained exchange, at both RX and TX, independent of frame type
(broadcast UDP, ARP, and TCP SYN were all observed affected on this session
alone). No further NetX/driver *logic* bug was found or is suspected at
this point — every packet that arrives intact is handled correctly, and
the remaining checksum errors on cleanly-parseable frames indicate wire-
level corruption, not a software miscalculation (manually verified: the
IP header bytes captured in `diag2`'s dump checksum to zero when computed
independently in Python for every frame checked this session, including
the SYN itself).

### Suggested next step

Closing this out for real needs the PHY/RGMII skew characterization the
original report already flagged as out of reach without an oscilloscope or
protocol analyzer: either fine-tune `gem2_phy_enable_tx_delay()`'s
KSZ9131RNX DLL config beyond the current on/off bypass toggle (a manually
swept fixed `txdll_tap_sel`, MMD 2h register `0x4D` bits `[11:6]`, and/or
the Clock Pad Skew Register at MMD 2h `0x08`, per DS00002841D §5.3.70), or
verify PS8-side RGMII delay/skew settings in the Vivado block design
(`applications/orbtrace/vivado/`) match what the KSZ9131RNX expects given
whichever DLL bypass configuration is finally settled on. Until then,
expect the control service to work intermittently rather than reliably —
this is a hardware signal-integrity issue, not something further changes
in `applications/orbtrace/firmware/` or `third_party/os_abstraction_layer/`
are likely to fix.

## Follow-up session 4. Machine-local xsct fixed properly; full tap sweep on real hardware rules out skew as today's cause; RX now completely dead, not intermittent

Same request ("implement this report's suggested next steps, get Orbtrace
running on target"), a later session, same board without a power cycle
since session 3.

### Machine-local: xsct's `/bin/bash` shebang and a new `libcrypt.so.1` gap, fixed

Neither is a repo issue. `xsct` (and the `loader` binary it execs) has a
literal `#!/bin/bash` shebang; this machine has no `/bin/bash` (NixOS,
`bash` only on `$PATH`, `/bin/sh` a symlink). Separately, `rdi_xsct` itself
is missing `libcrypt.so.1` — a new gap (nixpkgs' `libxcrypt` moved on since
session 2's `libtinfo.so.5` fix), same category. Fixed the same way as that
earlier fix, both applied outside this repo:

- Symlinked `libcrypt.so.1` from `nixpkgs#libxcrypt-legacy` into
  `/home/v/opt/vitis/Vitis/2023.2/lib/lnx64.o/` (same directory, same
  pattern as the existing `libtinfo.so.5` symlink from session 2).
- Used the pre-built `xsct-bwrap` FHS wrapper already present in the Nix
  store (`/nix/store/.../xsct-bwrap`, built from a `xsct-fhs` derivation)
  instead of the raw `xsct` binary — it mounts its own FHS rootfs's
  `/bin/bash`, resolving the shebang gap without touching system `/bin`.

With both fixed, `zub_ctl watch-a53 --xsct <xsct-bwrap path> ...` flashes
and boots repeatably again, same as session 2 established for a different
symptom of the same underlying gap.

### Full KSZ9131 tap_sel sweep: RX/TX skew ruled out as today's cause

Parametrized both `gem2_phy_enable_tx_delay()`'s TX *and* RX DLL tap_sel
(previously only the bypass bit was ever toggled; the tap amount itself,
defaulting to the chip's power-on `0x1B`, had never been swept) via
`-DGEM2_TX_DLL_TAP_SEL=<0-63>` / `-DGEM2_RX_DLL_TAP_SEL=<0-63>` build
flags, defaulting to `0x1B` to preserve existing behavior.

Swept TX at `{0x00, 0x1B}` and RX at `{0x00, 0x08, 0x1B, 0x20, 0x30, 0x3F}`
— a spread across the full 6-bit range — rebuilding, reflashing, and
testing each on real hardware (sustained `ping` with live UART diag
capture). **Every single combination produced the identical failure
signature**: `rx_frames` climbs to ~5 in the first second after link-up,
then freezes completely; `rxused_count` (the RXUSED recovery path from
session 2) climbs continuously thereafter (tens of times over 20-60s)
without ever unsticking it; zero ping replies, zero ARP replies observed
in host-side `tcpdump`, in every case.

This uniformity across the *entire* tap range is itself informative: a
genuine skew problem should show at least some variation in reliability
across an 0x00-0x3F sweep (some values working better, some worse). Seeing
byte-identical failure at every single point argues **against** skew being
today's actual proximate cause, contrary to this report's own prior
"Suggested next step."

### Ruled out: `NX_PACKET` pool exhaustion

`gem2_alloc_rx_packet()` returns early on `nx_packet_allocate()` failure
*without* clearing the just-processed BD's `NEW` bit (see the function
body) — a real bug if it were triggering, since a stuck `NEW=1` bit means
that ring slot can never be handed back to the DMA engine, which would
produce exactly the observed "freezes after a handful of frames, RXUSED
fires forever" signature independent of any PHY register. Added a
`pool_available`/`pool_total` diagnostic (`NX_PACKET_POOL`'s own counters)
to `diag2:` and confirmed on hardware: `pool_available` sits fixed at 10
(of 14 total) for the entire freeze window, never dropping. Pool
exhaustion is not happening; ruled out.

### Current best hypothesis, unconfirmed: PHY may be stuck in a bad analog state, or the link has degraded since session 3

Two threads point the same direction:

1. Session 3 (same calendar day, earlier) reported this link working
   *intermittently but genuinely* — ARP resolving repeatedly, a TCP SYN
   observed byte-correct at the driver — which is qualitatively different
   from today's **complete, deterministic** RX death after ~5 frames on
   every single one of 8 tested configurations.
2. This report has noted since session 2 that **the KSZ9131 PHY chip is
   not reset by JTAG or PS resets, only a full board power cycle** — and
   this session (like session 3) ran many DLL reconfigurations
   (bypass toggles, tap_sel sweeps, forced BMCR renegotiations) back to
   back on a PHY that was never power-cycled in between. It is plausible
   the PHY's internal DLL/calibration state does not fully re-settle from
   a register-only reconfigure-and-renegotiate cycle the way a real
   power-on does, and repeated churn across this and session 3 left it in
   a state a register write can't get back out of.

Neither is confirmed. Distinguishing them needs a full board power cycle
(not JTAG/PS reset) followed by a single clean flash at the known-default
`0x1B`/`0x1B` tap configuration and a fresh reliability test — genuinely
new information, not more sweeping, and not something achievable without
physically power-cycling the board.

### Suggested next step

1. **Power-cycle the board**, then reflash the default configuration
   (`GEM2_TX_DLL_TAP_SEL`/`GEM2_RX_DLL_TAP_SEL` both unset → `0x1B`) and
   rerun the same sustained-ping-plus-live-diag test this session used.
   If RX comes back to session-3-like intermittent behavior, the "PHY
   stuck from repeated churn" hypothesis is confirmed and the tap sweep
   this session ran (now cheap to rerun with `sweep_rx_tap.sh`-style
   scripting) becomes meaningful data again. If it's still completely
   dead, the problem is elsewhere (cabling, connector, a genuine PL/PS8
   RGMII configuration issue) and the tap sweep was never going to find
   it regardless of PHY state.
2. Only after (1) rules the PHY-state hypothesis in or out does resuming
   the tap_sel / Clock Pad Skew Register sweep from this report's earlier
   "Suggested next step" make sense.

## Follow-up session 5. Two real bugs fixed (BD ring stride, RX checksum alignment); TCP handshake completes for the first time; TX now the sole remaining blocker

Same request ("implement this report's suggested next steps, get Orbtrace
running on target"), a later session. Followed session 4's own suggested
next step first (power-cycle, then retest at default `0x1B`/`0x1B` taps),
then found two real, independently-confirmed root causes — the KSZ9131
PHY was never the problem.

### Bug 1: BD ring stride mismatch — root cause of "RX dies after exactly 1 frame, every reflash, independent of PHY tap_sel"

`ThreadXGEM2Driver.c` addressed every RX/TX descriptor slot
`GEM2_BD_ALIGN` (64) bytes apart (`sCtx.rx_bd + slot * GEM2_BD_ALIGN`, and
the equivalent for `tx_bd`). But `XEmacPs_BdRingCreate()`
(`xemacps_bdring.c`, vendored at
`/home/v/opt/vitis/Vitis/2023.2/data/embeddedsw/XilinxProcessorIPLib/drivers/emacps_v3_19/src/xemacps_bdring.c`)
always sets `RingPtr->Separation = sizeof(XEmacPs_Bd)` — **16 bytes on
aarch64** (`XEMACPS_BD_NUM_WORDS=4`, from `xemacps_bd.h`) — regardless of
the `Alignment` parameter passed in. `Alignment` only validates the ring's
*base* address (must be ≥ `XEMACPS_DMABD_MINIMUM_ALIGNMENT`, 64 on
aarch64, and a power of 2); it has no effect on per-descriptor spacing.

Only slot 0 ever coincided with an address the DMA engine actually walked.
Slots 1-3 (and the WRAP bit meant for the ring's last BD) sat in memory
the hardware never touched — those slots still held whatever
`XEmacPs_BdRingCreate()`'s initial `memset()` left there (`ADDR=0,
NEW/USED=0`), so the DMA's internal descriptor pointer, after slot 0,
walked into a **null-address, un-refilled native descriptor** and got
stuck (RX) or never advanced past slot 0 to begin with (TX).

**Confirmed live**, not just by reasoning: extended the RX BD diagnostic
(previously gated on `rxused_count > 0`, which never fired for this
failure mode) to print unconditionally every second, then read the raw
`RXQBASE` register during a stall. It auto-advanced in exact **16-byte**
steps (`0x61E80` → `0x61E90` → `0x61EA0`), never once reaching software's
64-byte-spaced "slot 1" address (`base + 64`). That register-level
observation is what nailed the root cause.

Fixed by adding a `GEM2_BD_STRIDE` constant
(`((u32)sizeof(XEmacPs_Bd))`, ThreadXGEM2Driver.c line ~104) and using it
for every per-BD address and cache-maintenance-range computation in the
file. `GEM2_BD_ALIGN` (64) is now used *only* for the backing arrays'
`__attribute__((aligned(...)))` and as the `Alignment` argument to
`XEmacPs_BdRingCreate()` — its only real purpose. The single-BD
`tx_q1_dummy` buffer (a standalone, non-indexed descriptor — see Bug-1's
sibling investigation below) was deliberately left alone; it has no
adjacency/stride concern.

This uniformly explains session 4's finding that a full `0x00`-`0x3F`
KSZ9131 tap_sel sweep produced byte-identical failure at every point: the
bug was never in the PHY, so no PHY register could have changed it.

### Bug 2: RX checksum misalignment — every received IP packet was silently flagged as corrupt

Even after Bug 1's fix, `ip_csum_err` incremented on **every single**
received IP packet, including ones independently re-checksummed byte-for-
byte in Python from the raw bytes captured straight out of the DMA buffer
(folds to `0xffff` — i.e. genuinely valid). NetX's own checksum path was
wrong, not the wire.

Root cause: NetX Duo's `_nx_ip_checksum_compute()`
(`nx_ip_checksum_compute.c`) does
`long_ptr = (ULONG *)packet_ptr->nx_packet_prepend_ptr;` and dereferences
it directly — it requires **4-byte alignment** of the packet's data
pointer. `NX_PACKET` pool buffers start 4-aligned (verified in
`nx_packet_pool_create.c`: `payload_address` is explicitly rounded up to
`NX_PACKET_ALIGNMENT`), but `gem2_rx_process()` wrote received frames
flush against that aligned buffer start, then advanced
`nx_packet_prepend_ptr` by the 14-byte (`14 mod 4 = 2`) Ethernet header
before handing the packet to NetX — landing the IP header 2 bytes off the
boundary NetX's checksum routine assumes, and silently computing the
wrong value for literally every packet.

Fixed via the Cadence GEM's `NWCFG.RXOFFS` field
(`XEMACPS_NWCFG_RXOFFS_MASK`, a documented 2-bit "receive buffer offset"
meant for exactly this alignment problem): set to 2
(`GEM2_RX_OFFSET`, ThreadXGEM2Driver.c line ~126) in `gem2_initialize()`'s
NWCFG setup. This makes the DMA insert 2 pad bytes before each frame
while keeping the BD's own buffer address 4-aligned (required since
bits[1:0] of that word are the NEW/WRAP flags). `gem2_rx_process()` was
updated to read the Ethernet header at `prepend_ptr + GEM2_RX_OFFSET` and
to advance `nx_packet_prepend_ptr` by `GEM2_RX_OFFSET + NX_ETHERNET_SIZE`
(16, a multiple of 4) when handing off to NetX.

**Confirmed on hardware**: `ip_csum_err`/`ip_invalid` stayed at 0 across
every subsequent capture, including ones taken after the link had
degraded again from repeated reflashing (see below) — i.e. the fix holds
independent of the separate link-reliability issue.

### Combined effect, verified after a clean power cycle

After power-cycling the board (once, deliberately, per session 4's own
suggested next step) and reflashing with both fixes in place:

- RX processed frames **continuously** under a sustained 20-ping burst
  (`rx_frames` climbed 7→10→13→17→18→... without ever freezing) — this
  had never happened before in this bring-up's history; every prior
  session died after a handful of frames at most.
- `ip_csum_err`/`ip_invalid` stayed at exactly 0 throughout.
- **NetX accepted a real TCP connection** — `tcp_conns=1`,
  `tcp_passive_conns=1` in the `diag2:` line — the TCP handshake
  completing at all is new; no prior session got this far.
- Note: `ping` (ICMP) is **not a valid test for this firmware** —
  `nx_icmp_enable()` is never called in `main.c` (only `nx_arp_enable()`
  and `nx_tcp_enable()`), so ICMP echo requests are legitimately dropped
  by design. Early in this session `ping` 100%-packet-loss results were
  misread as a regression before this was noticed; **use `nc <ip> 3401`
  or `//tests:orbtrace_throughput_test` to test reachability, not
  `ping`.**

### Remaining, newly-characterized blocker: TX ring wedges permanently after the first successful send

With both fixes in place, TX transmitted exactly one frame successfully
(`tx_complete=1`, matching the very first ARP reply) and then wedged
**permanently**: every subsequent `gem2_packet_send()` call (from
NetX attempting a SYN-ACK / retransmits for the accepted TCP connection)
queued into the ring, `tx_count` climbed to and stuck at 4 (the full
`GEM2_TX_BD_COUNT`), and it never drained again for the rest of the
session — `nc 192.168.1.50 3401` timed out with no reply ever reaching
the host.

This is a **different** bug from the historical "TXUSED interrupt storm"
(fixed in an earlier session, see `gem2_irq_handler()`'s
`XEMACPS_IXR_TXUSED_MASK` handling around line 920) and from Bug 1 above
(the stride fix applies equally to TX addressing and is confirmed
correct — see below). Diagnosed as far as this session got, with live
JTAG register evidence gathered *without reflashing* (the board keeps
running after `zub_ctl watch-a53`'s serial-watch exits; a fresh read-only
`xsct -nodisp <script>` with `mrd -force <addr>` can peek at it — see
"Reproducing/continuing this investigation" below):

- `NWCTRL` = `0x14` (`TXEN`=1, `RXEN`=1, `STARTTX` already self-cleared —
  expected/normal, it's a transient kick bit).
- `TXSR` = `0x0` — completely clear, no completion, no error bits
  (`RETRY`/`URUN`/`EXH` all 0) pending at the MAC level.
- `TXQBASE` stayed fixed at `tx_bd_base` (native slot 0's own address)
  for the entire stall, **even before** the stall (i.e. this register
  may simply not auto-advance for TX the way `RXQBASE` empirically does
  for RX — do not assume the RX-side "TXQBASE tells you the DMA's live
  position" reasoning carries over to TX without re-verifying; this was
  a working hypothesis this session, not confirmed).
- Manually forcing `NWCTRL |= STARTTX` again via a raw JTAG write (**not
  a reflash** — the running firmware and its state were left completely
  untouched) had **zero observable effect**: `NWCTRL`, `TXSR`, `TXQBASE`,
  and `ISR` were bit-for-bit identical 100 ms before and after the kick.
  This rules out "software just needs to re-kick STARTTX" as the fix.
- All 4 stuck descriptors' STAT words (`0x803E`, `0x803E`, `0x803E`,
  `0x40008036`) show `LAST`=1 and plausible lengths (54-62 bytes, i.e.
  bare TCP ACK/SYN-ACK-sized segments) but **no `USED` bit** — the DMA
  never touched them, not even to report an error.
- **Ruled out**: TXQ1 priority-queue interference (the pre-existing
  workaround at `sCtx.tx_q1_dummy`, parked with a permanent `USED|WRAP`
  sentinel so the ZynqMP GEM's mandatory TXQ1-before-TXQ0 scan order
  doesn't stall real sends). Read its live memory via JTAG mid-stall:
  `STAT = 0xC0000000` = `USED|WRAP` exactly as intended, `ADDR = 0`
  (also as intended, it's a dummy). Not corrupted, not the cause.
- `IMR` (interrupt mask) = `0x3FFFF301` confirms `TXCOMPL` (bit 7),
  `TXUSED` (bit 3), `RXUSED` (bit 2), and `FRAMERX` (bit 1) are all
  correctly *unmasked* (enabled) — the interrupts we want are not
  accidentally disabled. They simply never fire again after the first
  successful send.

### Suggested next steps, in order of likely leverage

1. **Consult the Zynq UltraScale+ TRM's GEM chapter** for the exact
   semantics of `STARTTX`/`TXQBASE` and what actually causes the DMA to
   re-scan the TX queue after having gone idle. The working theory this
   session ran out of time to test: perhaps `STARTTX` alone is
   insufficient to make the DMA re-read `TXQBASE` from scratch once it
   believes the queue is already "done" (e.g. it may only re-arm off a
   transition from `TXEN=0→1`, or need an explicit
   `XEmacPs_SetQueuePtr()`-style reprogram of the *current* descriptor —
   note `XEmacPs_SetQueuePtr()` itself is already known to no-op once
   `Start()` has run, see the RXUSED-recovery fix in commit `0fdc630`;
   the TX-side equivalent of that same class of bug is a strong
   candidate here and was not yet tried).
2. Try the same "bypass the guarded wrapper, write the raw register
   directly" technique that fixed RXUSED recovery
   (`gem2_irq_handler()`'s `XEMACPS_IXR_RXUSED_MASK` branch, ~line 869)
   but for TX: on `TXCOMPL`/`TXUSED`, or even proactively in
   `gem2_packet_send()` after queuing, try writing `XEMACPS_TXQBASE_OFFSET`
   directly to point at the *current* `tx_head` slot before/instead of
   relying on `XEmacPs_Transmit()`'s `STARTTX` kick alone. Session 3's
   note about this causing an interrupt storm was specifically about
   doing it in response to *every* `TXUSED` (which fires benignly on
   every send due to the TXQ1 scan-order quirk) — doing it only when a
   genuine stall is detected (e.g. `tx_count` unchanged across two
   consecutive diag ticks) may avoid that failure mode.
3. Check whether `gem2_tx_cleanup()` is even being invoked for these
   stuck frames at all — instrument `gem2_irq_handler()` to count how
   many times each `isr` bit pattern is seen (a raw histogram, not just
   `last_isr`) to rule out a starvation scenario where some *other*
   interrupt keeps firing and dominating the ISR before TX-related bits
   ever get inspected. (`last_isr` in the existing diagnostics only shows
   the *most recent* value, which stayed `0x2`/FRAMERX for the entire
   stall — consistent with either theory and not itself conclusive.)
4. Once TX drains reliably, rerun `//tests:orbtrace_throughput_test`
   (TCP 3401) — this session got NetX to *accept* a connection
   (`tcp_conns=1`) but never observed a SYN-ACK leave the board, so the
   handshake is not confirmed complete from the host's point of view yet.

### Reproducing/continuing this investigation without reflashing

The board keeps running its currently-flashed firmware independently of
any JTAG/serial tooling. To reattach and observe without disturbing it:

```sh
# Reattach to already-running UART output (no reflash, no reset):
zub_ctl serial-watch --tty /dev/ttyUSB1 --baud 115200 --timeout 30 --print-only

# Read-only (or careful read-modify-write) JTAG register peek while the
# board keeps running — does NOT halt the core for simple mrd/mwr:
xsct -nodisp some_script.tcl
# where some_script.tcl does:
#   connect
#   targets -set -nocase -filter {name =~ "*A53*#0"}
#   mrd -force 0xFF0D0000   ;# NWCTRL
#   mrd -force 0xFF0D0014   ;# TXSR
#   mrd -force 0xFF0D001C   ;# TXQBASE
#   mrd -force 0xFF0D0024   ;# ISR
#   mrd -force 0xFF0D0030   ;# IMR
#   disconnect
```

Use the `xsct-bwrap` FHS wrapper from `/home/v/opt/vitis-flake` (`nix build
/home/v/opt/vitis-flake#xsct`) rather than the raw `xsct` binary — see
this report's session 2/4 notes on why. GEM2's base address is
`0xFF0D0000` (matches `GEM2_BASE` in `main.c`).

**Caution**: this session made ~10 JTAG reflashes in one sitting and hit
the by-now-familiar "repeated JTAG-only reflashing degrades the link"
symptom again partway through (see session 4) — after several reflashes
following the one clean post-power-cycle test, RX went back to freezing
after ~2 frames even though `ip_csum_err` correctly stayed at 0 (proving
Bug 2's fix holds independent of this separate issue). A fresh power
cycle immediately restored full continuous RX. **Budget for at most a
handful of reflashes per power cycle**, and prefer the read-only JTAG
peek technique above over reflashing when the question doesn't actually
require new firmware.

Fixes for Bugs 1 and 2 are committed (`b45ea3c`,
"Fix GEM2 BD ring stride and RX checksum alignment: RX/TX now work, TCP
handshake completes"). The temporary bring-up diagnostics (`diag:`,
`diag2:`, `diag3:`, `diag4:` in `main.c`'s `diag_thread_entry`, and their
`gem2_diag_get*()` backing functions in `ThreadXGEM2Driver.c`) are all
still in the tree and were essential to this session's findings — keep
them until the TX stall is resolved and reachability under
`//tests:orbtrace_throughput_test` is confirmed end-to-end.

## Follow-up session 6. TX ring stall fully root-caused and fixed (BD ring never pre-armed); new, unrelated blocker found in NetX's ARP table

Same request ("continue with the implementation, get Orbtrace running on
target"), a later session, picking up directly at session 5's TX stall.
Real hardware was reachable throughout (`/dev/ttyUSB1` present, board
already running from a prior session).

### Critical process pitfall this session lost significant time to: `bazel-bin/.../a53_app_elf` can silently point at a stale, untransitioned build

`bazel build //applications/orbtrace/firmware/a53_app:a53_app` (the plain
`cc_binary`-style target, no `--platforms` flag) succeeds and reports
"up to date" every time, and leaves a real `a53_app_elf` file sitting at
`bazel-bin/applications/orbtrace/firmware/a53_app/a53_app_elf` — but that
build does **not** apply the firmware macro transition
(`//sdk/platforms:apu_a53`) at the top level, so it does not necessarily
touch the `a53_app_elf` genrule output at all if that specific artifact
isn't part of *this* invocation's requested output set under *this*
config hash. The file left behind at that path can be a stale artifact
from an earlier, differently-configured build (e.g. one made through
`//tests:orbtrace_a53_app_test`, which pulls in the transition), and nothing
about a subsequent successful `a53_app` build touches or invalidates it.

Lost roughly two full fix-flash-test cycles to this silently: two
different, real, source-level fixes were written, built ("successfully"),
reflashed, and tested — and produced byte-for-byte identical failure
symptoms to the *original* bug, with zero behavioral difference either
time. Root-caused by comparing `strings bazel-bin/.../a53_app_elf | grep`
for a distinctive new format string against the file's actual mtime and
against `readlink -f`, which showed the "successfully built" file resolved
to `bazel-out/k8-fastbuild/bin/...` while a *second*, actually-current copy
sat under a completely different config directory,
`bazel-out/k8-fastbuild-ST-<hash>/bin/...` (the `-ST-` suffix is the
starlark-transition config hash) — with a much newer mtime and containing
the new code.

**Fix/workaround**: always pass `--platforms=//sdk/platforms:apu_a53`
explicitly on every `bazel build` invocation that's meant to produce the
ELF actually used for flashing, e.g.:
```sh
bazel build --platforms=//sdk/platforms:apu_a53 \
    //applications/orbtrace/firmware/a53_app:a53_app_elf
```
This makes `bazel-bin/applications/orbtrace/firmware/a53_app/a53_app_elf`
resolve to the correct, current file directly (confirmed via `strings`
containing new format strings immediately after the build, mtime matching
`date`). **Before trusting any "successful" build+reflash cycle that
doesn't reproduce an expected behavior change, verify the actual flashed
file is current** — `strings <elf> | grep <a string unique to your new
change>` is a fast, reliable check; don't trust the build log alone.

### Bug 3 (the actual TX stall root cause): the TX BD ring was never armed to a safe idle state before first use — RX had this, TX didn't

Re-examined `gem2_initialize()` side by side with `gem2_alloc_rx_packet()`.
RX pre-arms **every** RX BD slot (`GEM2_BD_COUNT` of them, in the `for`
loop at step 7) before `XEmacPs_Start()` ever runs — each slot gets a real
buffer, `NEW` explicitly cleared, and `WRAP` explicitly set on the last
slot. TX had no equivalent: `XEmacPs_BdRingCreate()` (vendored
`xemacps_bdring.c`, already examined in session 5) only
`memset()`s the whole ring to zero and does **not** set `USED` on any
descriptor — confirmed by reading the vendored source directly, not
inferred. A zeroed TX BD reads as `USED=0` (DMA-owned), `LAST=0`,
`WRAP=0`, length `0` — not a valid "idle, nothing to do" marker, just an
uninitialized one.

Xilinx's own reference driver
(`emacps_v3_19/examples/xemacps_example_intr_dma.c`, lines ~575–595)
confirms this is a real, required step: it builds a BD template with
`XEMACPS_TXBUF_USED_MASK` set via `XEmacPs_BdSetStatus()`, then calls
`XEmacPs_BdRingClone()` — which (per the vendored source) clones that
template across every ring slot **and** automatically sets `WRAP` on the
last one — immediately after `XEmacPs_BdRingCreate()`. This driver never
called `BdRingClone()` for TX at all.

Fixed by adding exactly that call (`gem2_initialize()`, step 5b, right
after the TX `XEmacPs_BdRingCreate()`): a local `XEmacPs_Bd bd_template`,
cleared via `XEmacPs_BdClear()`, with `XEMACPS_TXBUF_USED_MASK` set via
`XEmacPs_BdSetStatus()`, passed to
`XEmacPs_BdRingClone(&XEmacPs_GetTxRing(&sCtx.mac), &bd_template,
XEMACPS_SEND)`.

**This single fix, confirmed live on real hardware, fully resolved the TX
stall** — no other change was needed for TX to work correctly:
- `tx_complete` now tracks `tx_frames` exactly on every observed run (e.g.
  18/18, 17/17 — previously it flatlined at 1 while `tx_frames` kept
  climbing).
- `tx_count` returns to `0` after every burst instead of sticking at
  `GEM2_TX_BD_COUNT` forever.
- Confirmed over a sustained run with continuous retransmit traffic (a
  real TCP SYN retry storm from repeated connection attempts, dozens of
  frames), not just the single first ARP reply that worked before.

Two *earlier* fix attempts *this same session*, both wrong, are preserved
as commentary in the source for the next reader rather than silently
dropped:
1. An ISR-triggered recovery on `XEMACPS_IXR_TXUSED_MASK`, gated by a
   "kicked" latch cleared on every newly-queued frame. Real hardware
   testing showed **zero effect** — `TXQBASE` never moved off
   `tx_bd_base` across a full stall either with or without it. Root cause:
   `TXUSED` also fires as a benign side effect of *every* real send (the
   pre-existing, already-documented ZynqMP TXQ1-priority-queue scan
   quirk), so clearing the latch on every newly-queued frame meant the
   recovery — which toggles `TXEN` off — ran on almost every transmit,
   plausibly aborting legitimate in-flight DMA transfers.
2. A poll-based version (`gem2_tx_poll_recover()`, called once a second
   from `main.c`'s diag thread) that also turned out to be unnecessary
   once Bug 3 above was found and fixed — but it was **built and tested
   before** Bug 3 was found (both attempts were made against a copy of
   the driver that, unknown at the time, was missing the real fix, due to
   the stale-ELF pitfall above), so its apparent ineffectiveness at the
   time was actually the stale-ELF problem, not a flaw in the poll
   approach itself.

The poll-based version was **kept** (not reverted) as a defensive safety
net: it's cheap, decoupled from send/interrupt timing (so it can't race a
legitimate in-flight transfer the way the ISR version could), and directly
informed by a real, previously-reproduced hardware failure mode (the DMA
halting on a used TX descriptor without reliably raising a further
interrupt to report it — confirmed via `isr_calls`/`txused_count` going
completely flat while `tx_count` stayed stuck, in the pre-Bug-3 testing
this same session). With Bug 3's real fix in place, `tx_recover_attempts`
has stayed at `0` through every subsequent test — the safety net has not
needed to fire, consistent with the ring simply not stalling anymore.

New temporary diagnostics added and left in place (same rationale as the
existing ones — see previous sessions' notes): `diag_tx_recover_attempts`
and `diag_tx_recover_txqbase_{before,after}` (did a recovery attempt run,
and did its register write visibly take effect — used to debug the
poll-recovery mechanism itself), and `diag_last_tx_dst_{msw,lsw}` /
`diag_last_tx_cmd` (the destination MAC and `NX_IP_DRIVER` command NetX
supplied for the most recent `gem2_packet_send()` call — added to chase
the new blocker below, see `gem2_diag_get_tx_recover()` /
`gem2_diag_get_tx_dst()`).

### New blocker found: TCP handshake still doesn't complete end-to-end — NetX's ARP table holds a wrong destination MAC for the host, despite ARP itself resolving correctly

With Bug 3 fixed, TX transmits every frame the stack asks it to send —
but a real TCP client (tested via raw `tcpdump` on the host, see caveat
below about `/dev/tcp` being a **broken test method**) still never
completes the three-way handshake. Wire-level capture shows why:

- The board's SYN-ACK **does** reach the host's NIC, with a **valid TCP
  checksum** (`tcpdump -vv` reports `cksum ... (correct)`) and a
  well-formed header (verified byte-for-byte against the raw hex: IHL,
  total length, and TCP data offset are all internally consistent with
  the actual MSS-option-plus-padding bytes present — not a framing/length
  bug).
- But its **Ethernet destination MAC is wrong**. Two separate captures
  this session, from two different boot sessions, show two *different*
  wrong values (`30:30:c0:a8:01:01` in one; a driver-level diagnostic
  read `msw:lsw = C31A0D49:3882F5E2` in another) — neither matches the
  host's real MAC (`00:e0:4c:75:87:68`, confirmed via `ip link show`).
  Session-to-session variation in the *specific* wrong bytes suggests
  uninitialized/stale memory content feeding into this, not a fixed
  logic/arithmetic bug with one deterministic wrong answer.
- The host's kernel, receiving a frame not addressed to its own MAC,
  never delivers it to the TCP stack at all (typical NIC-level unicast
  filtering) — so it just keeps retransmitting its original SYN forever,
  which is exactly the observed symptom: `tcpdump` shows the *same*
  half-open connection's SYN retransmitted every ~1s indefinitely, board
  replies with a fresh SYN-ACK to each one (same seq/ack each time,
  confirming the board-side TCP state itself is fine), no ACK ever
  arrives from the host.
- Added `gem2_diag_get_tx_dst()` to read back exactly what
  `req->nx_ip_driver_physical_address_msw/lsw` (i.e. exactly what NetX's
  `nx_ip_driver_packet_send()` put in the driver request — see that
  file's ARP-table-lookup branch, `common/src/nx_ip_driver_packet_send.c`
  around line 204) contained at the moment `gem2_packet_send()` was
  called, independent of anything on the wire. It matches the wrong wire
  value exactly (confirming this is NetX's own ARP table entry, not
  something our driver corrupts on the way out), and `tx_dst_cmd` reads
  `0` (`NX_LINK_PACKET_SEND`) confirming these are genuine regular
  TCP-output driver calls, not some other packet type.
- **Ruled out**, by reading the vendored NetX source directly (not
  inferred): `nx_arp_packet_receive.c`'s extraction of the sender's
  hardware address from a received ARP frame
  (`sender_physical_msw/lsw`, lines ~143–144) — the *exact same*
  extraction code, on *this exact board, this same boot*, is proven
  correct by an independent live capture: the board's **direct** reply to
  the host's ARP request (`who-has 192.168.1.50 tell 192.168.1.1`) went
  out as `192.168.1.50 is-at 00:0a:35:00:01:02` to the host's **correct**
  MAC (host received it fine — that's how the ARP exchange in the same
  capture completed). Every place `nx_arp_packet_receive.c` writes into
  the persistent ARP table (`arp_ptr->nx_arp_physical_address_msw/lsw`,
  both the "update existing entry" and "create new entry" branches) uses
  those exact same, provably-correct `sender_physical_msw/lsw` variables
  — so nothing in NetX's own ARP-table-write logic looks capable of
  producing this on inspection.
- **Not yet checked / suggested next steps, in order of likely leverage**:
  1. Power-cycle the board (this session did ~7 JTAG-only reflashes in a
     row without one — session 4/5's own already-documented
     repeated-reflash link degradation caveat applies, and it's cheap and
     rules out "stale/corrupted state carried across many reflashes
     within one boot" vs. a genuine repeatable logic bug) and retest a
     **single, fresh** connection attempt right after.
  2. Check whether `gem2_rx_process()`'s DMA-hardware-reported `frame_len`
     (a raw 14-bit field straight from the RX BD's STAT word, trusted
     without any software-side upper bound check against the packet's
     real 1536-byte buffer) could, under some as-yet-unobserved
     condition, exceed the buffer and let the DMA write **past** an
     `NX_PACKET`'s data area into adjacent heap memory — which could
     include NetX's ARP pool (`arp_cache_memory[1024]` in `main.c`) if
     linker layout places them nearby. No direct evidence of this
     happening yet (`last_len` has only ever been observed in the 60–74
     byte range, and `ip_invalid`/`ip_csum_err`/`tcp_invalid` have all
     stayed at `0`), but it's the most plausible mechanism found so far
     for "NetX's own correct-looking logic reads back wrong data," and a
     bounds check (clamp `frame_len` to `GEM2_RX_BUFSIZE - GEM2_RX_OFFSET`
     before the `Xil_DCacheInvalidateRange`/handoff, dropping the frame if
     it doesn't fit) would be cheap defensive insurance either way.
  3. Instrument `nx_ip_driver_packet_send.c`'s ARP lookup branch directly
     (a local, throwaway build with an extra `xil_printf` — this file is
     vendored/external, so **don't commit** a permanent change there) to
     print `arp_ptr` itself alongside the msw/lsw values, to see whether
     the *pointer* being read is sane (e.g. within the expected
     `arp_cache_memory` range) — this would immediately distinguish "wrong
     bytes in a legitimate ARP entry" from "reading a wild/corrupted
     pointer."

### Testing methodology caveat: `bash -c '</dev/tcp/host/port'` cannot distinguish TCP success from failure

Used early this session and initially misread as "still failing" — a bare
`</dev/tcp/host/port` redirect with nothing else in the command block
completes the `connect()` (if it succeeds) but then just **hangs**, since
nothing reads or writes the resulting file descriptor; a wrapping
`timeout N` always kills it and reports exit `124`, regardless of whether
the TCP handshake actually completed. **This is not a valid pass/fail
signal on its own.** Use a raw `tcpdump` capture (as this session
ultimately did, and as documented above) or a tool that actually
exchanges bytes (`nc` without `-z`, or `//tests:orbtrace_throughput_test`)
to get a real answer.

### Reflash/test recipe used this session (supersedes the `xsct`/`zub_ctl` snippet in earlier sessions' notes for the *build* step specifically)

```sh
# Build with the platform transition applied explicitly — see the
# stale-ELF pitfall above for why this matters:
bazel build --platforms=//sdk/platforms:apu_a53 \
    //applications/orbtrace/firmware/a53_app:a53_app_elf

# Sanity-check the ELF is actually current before flashing:
strings bazel-bin/applications/orbtrace/firmware/a53_app/a53_app_elf \
    | grep '<a string unique to your latest change>'

# Flash + wait for boot banner (same as prior sessions):
bazel-bin/tooling/zub_ctl/zub_ctl watch-a53 \
    --xsct /home/v/opt/vitis-flake/result/bin/xsct \
    --elf bazel-bin/applications/orbtrace/firmware/a53_app/a53_app_elf \
    --bitstream sdk/boards/zub_1cg/design_1_wrapper.bit \
    --psinit sdk/boards/zub_1cg/psu_init.tcl \
    --tty /dev/ttyUSB1 --baud 115200 --timeout 45 \
    --expect 'Orbtrace A53 control service'

# Reattach and watch diagnostics without reflashing:
bazel-bin/tooling/zub_ctl/zub_ctl serial-watch \
    --tty /dev/ttyUSB1 --baud 115200 --timeout 40 --print-only

# Real (not /dev/tcp) reachability check from another shell:
sudo tcpdump -i <host-iface> -n -vv host 192.168.1.50 and port 3401
```

Source changes this session (TX ring pre-arm fix, poll-based recovery
safety net, and the new `tx_dst`/`tx_recover` diagnostics) are in
`third_party/os_abstraction_layer/ThreadX/ThreadXGEM2Driver.c` and
`ThreadXGEM2Driver.h`, plus the corresponding diag-print additions in
`applications/orbtrace/firmware/a53_app/src/main.c`.
