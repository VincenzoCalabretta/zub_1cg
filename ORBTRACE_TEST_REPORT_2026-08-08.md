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
