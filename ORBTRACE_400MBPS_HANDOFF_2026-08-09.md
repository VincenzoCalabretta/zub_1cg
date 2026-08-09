# Orbtrace 400 Mbit/s Target Trace Handoff

Date: 2026-08-09  
Target: ZUBoard 1CG  
Board address: `192.168.1.50`  
Host address: `192.168.1.1`

## Current status

The 400 Mbit/s real-time trace claim is **not yet proven**, but the entire
target trace path has now carried real data:

```text
PL test source -> Orbflow encoder -> AXI Stream -> AXI DMA -> DDR
               -> A53/NetX -> GEM2 -> Ethernet -> host
```

Important unit correction: the documented claim is **400 Mbit/s = 50 MB/s**,
not 400 MB/s. A 3 GB capture in 60 seconds is the intended acceptance test.

## What is proven

- Port 3402 is implemented and accepts TCP connections.
- AXI DMA writes valid Orbflow packets into target DDR.
- The A53 sends those packets to the host with valid Ethernet/IP/TCP
  checksums.
- The host received captures of 4,380 and 5,110 bytes.
- PL statistics during testing showed zero dropped bytes, sync loss, and DMA
  faults.
- The corrected bitstream meets timing:
  - WNS: +1.823 ns
  - WHS: +0.010 ns
- Loaded artifact hashes:
  - Bitstream:
    `84edbb493b478597692618b9e08c19171ce27badeffa78f99709cbc2bfca2ebe`
  - Latest A53 ELF:
    `43c899dfe80ed34e292486d02aa082ac1b6581b4e98d57a20209807f502154fb`

## Current blocker

The wire capture identified a precise Ethernet/TCP corruption boundary.

For port 3402, the board correctly:

1. Completed the TCP handshake.
2. Sent six valid trace segments totalling exactly 4,380 bytes:
   - 1028 + 5 + 1032 + 1032 + 1032 + 251 bytes
3. Received a correct cumulative ACK from the host for sequence 4381.

One second later, subsequent board frames became malformed:

- TCP source port changed from 3402 to `0`.
- Destination port became `32774`.
- Sequence numbers and urgent fields became nonsensical.
- Ethernet and IP addresses remained correct.
- Checksums were internally valid for the corrupted contents.

This places the remaining immediate fault in the target's TX packet/descriptor
handling, not in the PL encoder, AXI DMA, PHY, host receiver, or basic TCP
handshake.

A leading hypothesis is that `gem2_packet_send()` mutates a NetX packet by
prepending an Ethernet header and does not restore the packet before NetX
reuses or retransmits it. Descriptor cache-line interactions in the expanded
ring should also be checked.

## Changes made

The intentional, uncommitted changes from this investigation are:

### `applications/orbtrace/firmware/a53_app/src/main.c`

- Added the TCP 3402 trace server.
- Added a 32-descriptor AXI DMA S2MM ring.
- Copies completed DMA packets into NetX packets.
- Uses a dedicated non-cacheable DMA memory region.

### `applications/orbtrace/vivado/create_bd.tcl`

- Disabled the unconnected AXI DMA status/control stream:
  `C_SG_INCLUDE_STSCNTRL_STRM=0`.
- Before this fix, payloads reached DDR but descriptors never completed.

### `sdk/bsp/apu/lscript_a53.ld`

- Added `.trace_dma` at `0x10000000` in a separate ELF segment.

### `tests/orbtrace_throughput_test.sh`

- Fixed the broken TCP preflight.
- Added deterministic PL test-source configuration.
- Fixed 64-bit rate arithmetic overflow.
- Added configurable capture size and minimum rate.

### `third_party/os_abstraction_layer/ThreadX/ThreadXGEM2Driver.c`

- Increased the TX ring from 4 to 64 descriptors.
- Limited UART descriptor diagnostics to four entries to prevent array
  overflow.
- This removed the original four-packet saturation: diagnostics reached
  `tx_frames=28`, `tx_complete=28`, `tx_count=0`, with no bad-destination
  drops.

The worktree also contains many unrelated concurrent changes. They must not be
reverted or attributed to this work.

## Recommended continuation

1. Reflash to obtain a clean target state.
2. Compare `gem2_packet_send()` with the NetX reference driver contract.
3. Instrument packet pointers, prepend/append pointers, and flattened TX bytes
   before the first send and retransmission.
4. Fix the post-4,380-byte packet corruption.
5. Verify uninterrupted 1 MB, 10 MB, then larger captures.
6. Measure the existing RTL rate.

There is a second known limitation after TCP is fixed: the current 100 MHz
encoder architecture takes roughly three cycles per input byte, giving an
estimated ceiling near **267 Mbit/s**. Therefore, proving 400 Mbit/s will
likely require a pipelined or ping-pong Orbflow/COBS encoder before running the
final 3 GB/60-second acceptance test.

## Continuation: steps 2–4 implemented and hardware-verified; step 5 partially done

Steps 2–4 of the recommended continuation (compare `gem2_packet_send()`
against the NetX reference driver contract, instrument packet pointers, fix
the post-4,380-byte corruption) were carried out at the code level in one
session, then verified on real hardware (board connected, JTAG + UART) in a
follow-up session. Step 6 (RTL rate ceiling) still hasn't been measured.

`gem2_packet_send()` in
`third_party/os_abstraction_layer/ThreadX/ThreadXGEM2Driver.c` permanently
mutated the outbound `NX_PACKET`'s `nx_packet_prepend_ptr` (-= `NX_ETHERNET_SIZE`)
and `nx_packet_length` (+= `NX_ETHERNET_SIZE`) to make room for the Ethernet
header it builds, and never restored either field. `nx_packet_transmit_release()`
for a successfully queued send only happens later, asynchronously, from
`gem2_tx_cleanup()` on DMA completion — so if NetX's TCP retransmission timer
hands the same still-queued `NX_PACKET` pointer back to `gem2_packet_send()`
before that completion callback runs, the function re-enters with
`prepend_ptr` already short by 14 bytes from the first send and decrements it
a second time, and computes the flattened frame length from an
`append_ptr - prepend_ptr` difference that grew by the same 14 bytes. This
matches the reported fault signature (corruption appearing exactly one RTO —
about a second — after the last good segment) and was already the leading
hypothesis recorded above.

Fix applied: both places that return from `gem2_packet_send()` after the
header mutation (the normal successful-queue path, and the "drop and let
TCP's own retransmission timer retry later" bad-destination path) now restore
`nx_packet_prepend_ptr`/`nx_packet_length` to their original values before
returning, so a retransmitted packet is seen by the driver in the same state
NetX originally handed it in.

Instrumentation added alongside the fix (`gem2_diag_get_tx_pkt_state()`,
printed as `diag5:` from `diag_thread_entry` in
`applications/orbtrace/firmware/a53_app/src/main.c`) exposes:

- `tx_retransmit_count` — increments whenever `gem2_packet_send()` sees the
  same `NX_PACKET` pointer as the previous call, i.e. an actual retransmit.
- `tx_prepend`/`tx_length` before and after the restore, each call.

On the next hardware run, if `tx_retransmit_count` advances around the
~1-second mark and the corruption from this handoff is gone, that confirms
the fix; if the corruption persists, the `diag5:` line pins down whether the
restore is failing to run or the fault lies elsewhere.

### Hardware verification (this session)

The board was reflashed and driven end-to-end with `XILINX_ROOT` pointed at
a real local Vivado/Vitis 2023.2 install (`~/opt/vitis`; the env var is now
set in `~/dotfiles-nix/dotfiles/zsh/zprofile` for future shells). The host's
USB-Ethernet adapter needed a NetworkManager profile bound to its MAC address
(`nmcli connection add type ethernet con-name zub_1cg-board mac
00:e0:4c:75:87:68 ip4 192.168.1.1/24`) rather than a bare `ip addr add`,
because the adapter renumbers (`enp0s20f0uN`, N changes) across USB
re-enumeration/replug and NetworkManager was flushing manually-added
addresses on an unmanaged/disconnected device.

**Bitstream/PS-init bug found and fixed independently of the TCP bug.** The
bitstream and `psu_init.tcl` actually checked into the repo
(`sdk/boards/zub_1cg/design_1_wrapper.bit`, sha256 `bbf9c427...`) are **not**
the Orbtrace design — the repo's own `sdk/boards/zub_1cg/artifacts.json`
labels that bitstream "PL bitstream for the Ethernet-loopback-capable
design." Flashing it and starting a trace capture correctly fails
(`trace_dma_initialize()` returns `NX_NOT_SUCCESSFUL`, "trace DMA
initialization failed") because there's no real Orbtrace AXI DMA at that PL
address in that image. The real Orbtrace bitstream only existed as a stray
Bazel-cache artifact from a prior session's Vivado build
(`bazel-out/orbtrace-vivado/zub_orbtrace.bit`, sha256 `84edbb49...`, matching
this handoff's recorded hash) — it was never copied into the repo.

Flashing that correct bitstream paired with its own directly-exported
`psu_init.tcl` (also from that same stray build directory) hung
indefinitely at PS/DDR init — reproduced 3/3 times, including immediately
after a full power cycle, so it isn't leftover JTAG lock state. Root cause:
`applications/orbtrace/vivado/create_bd.tcl` creates the `zynq_ultra_ps_e`
PS cell and hand-sets only a few properties (GP0/GP2 AXI, IRQ, trace clocks)
— it never calls `apply_bd_automation ... {apply_board_preset "1"}`. DDR is
therefore left at generic IP defaults instead of the real LPDDR4 geometry
(`sdk/boards/zub_1cg/board_preset.tcl`'s `zub1cg_apply_ps_preset` explicitly
asserts `LPDDR4`/32-bit/8192 Mbit via `zub1cg_require_property` for exactly
this reason). A `psu_init.tcl` exported straight from that under-configured
PS calibrates DDR against the wrong parameters and its wait-loop never
returns. Confirmed by swapping in the board-preset-validated
`sdk/boards/zub_1cg/generated/psu_init.tcl` (same one the Ethernet-loopback
design uses — DDR/PS config is board-level, not per-application) alongside
the real `zub_orbtrace.bit`: the flash completed in seconds, the board
booted, and the trace path worked end-to-end. **`create_bd.tcl` still needs
`apply_board_preset` added and a fresh bitstream/XSA/`psu_init.tcl` build +
commit to fix this permanently** — the swapped-in generic `psu_init.tcl` is
a same-session workaround, not a real fix, and may not correctly configure
AXI HPM/fabric-width registers that do depend on the full design's actual
GP-port usage (Orbtrace enables GP0+S_AXI_GP2; the generic handoff enables
GP0+GP1) — no problem was observed from this mismatch in this session's
testing, but it hasn't been specifically audited either.

**TCP fix confirmed working under real retransmit load.** With the correct
bitstream/psu_init running the real Orbflow → AXI Stream → AXI DMA → DDR →
GEM2 → Ethernet path (PL test source in `swo-nrz` mode via
`orbtrace configure ... test swo-nrz 2000000`), a ~20-second capture
streamed 330 KB (390,159 total `rx_bytes` per the PL stats counters) with
`dropped_bytes=0 sync_loss=0 dma_faults=0` throughout — the same
zero-fault bar the original (unfixed) session had already cleared, now
sustained well past the original ~4,380-byte/~1-second failure point.
`tx_retransmit_count` (the new diagnostic) climbed to 17 real NetX TCP
retransmits over the run; every single `diag5:` line showed
`tx_prepend`/`tx_length` identical before and after
(e.g. `tx_prepend=0x63688->0x63688 tx_length=100->100`), confirmed by
scripting a check over the full UART log — zero drift events. No connection
resets, no corrupted frames, no `tx_dropped_bad_dst`. This is direct
hardware confirmation that the header-mutation root cause is fixed.

### What's still open

- `create_bd.tcl` needs `apply_board_preset` added, and a fresh Vivado
  build + committed bitstream/XSA/`psu_init.tcl` to replace the stale
  Ethernet-loopback artifacts currently checked in (`design_1_wrapper.bit`,
  `sdk/boards/zub_1cg/psu_init.tcl`) — this session flashed from Bazel-cache
  and `~/opt/vitis`-local artifacts that aren't committed anywhere.
- Step 5 (uninterrupted 1 MB, 10 MB, then larger captures) was only
  partially exercised (a few hundred KB over ~20 s in two short runs, not a
  sustained 1 MB+ run) — worth a longer, unattended run once the bitstream
  fix above is committed.
- Step 6 (measuring the existing RTL rate, and the ~267 Mbit/s encoder
  ceiling from a 100 MHz/3-cycles-per-byte architecture) has not been
  measured at all yet.
- The full 3 GB/60-second acceptance test still requires the pipelined/
  ping-pong Orbflow/COBS encoder work noted below, regardless of the TCP fix.

## Continuation (same day, follow-up session): board-preset fix built and hardware-verified; new sustained-load hang found

This session picked up the "What's still open" list above. An in-progress,
uncommitted migration was already sitting in the worktree (`board_preset.tcl`,
`create_ps_handoff.tcl`, `export_psu_init.tcl`, `generate_psu_init.sh`,
`publication_guard.bzl`, plus `BUILD.bazel`/`artifacts.json`/
`COMPATIBILITY.md` edits) that moves `psu_init.tcl` to a gitignored,
regenerate-on-demand local artifact instead of a committed one. This session
finished that migration and used it to actually close out the `create_bd.tcl`
item.

### `create_bd.tcl` / `build.tcl` fix

`applications/orbtrace/vivado/create_bd.tcl` now calls
`zub1cg_apply_ps_preset` (from `sdk/boards/zub_1cg/board_preset.tcl`,
i.e. `apply_bd_automation ... {apply_board_preset "1"}`) on the PS cell
right after creating it, before the Orbtrace-specific GP0/S_AXI_GP2/IRQ/trace
property overrides — mirroring the same fix already validated as a
same-session workaround in the prior continuation above.

Wiring this in surfaced a second, previously-unknown bug:
`applications/orbtrace/vivado/build.tcl` didn't select a board part at all
(no `board_part` was ever set on the project, so `apply_board_preset` had
nothing to apply against). Fixing that required `AVNET_BDF_ROOT` (the Avnet
BDF checkout root) and calling `zub1cg_select_board`. Doing this the "obvious"
way — after `create_project`, matching where `create_bd.tcl` runs — silently
produced zero matching board parts. Confirmed by direct experiment: Vivado
only picks up a custom `board.repoPaths` repo if it's set **before**
`create_project` establishes the board catalog; setting it after is a no-op
with no error. `create_ps_handoff.tcl` already had this right (it sets
`board.repoPaths` once before `create_project`, then again — harmlessly
redundantly — inside `zub1cg_select_board` after). `build.tcl` now does the
same: `set_param board.repoPaths` before `create_project`, then
`zub1cg_select_board` after.

### Fresh Vivado build: succeeded

Built via `~/opt/vitis` (Vivado/Vitis 2023.2, through this repo's Nix
devShell) with the fixed `create_bd.tcl`/`build.tcl`. Synthesis,
implementation, bitstream write, XSA export, and `psu_init.tcl` export from
that XSA all completed cleanly:

- Bitstream sha256: `30bed13ed1ff248fd88dbba50187cc1961e0c9ff4eaa1d15029c75285b32ff0d`
- WNS +2.319 ns, WHS +0.010 ns (both improved over the previous build)

This build's own `psu_init.tcl` (exported from the Orbtrace XSA into
`bazel-out/orbtrace-vivado/`) is a separate thing from
`sdk/boards/zub_1cg/generated/psu_init.tcl` (the shared, board-level one
produced by `bazel run //sdk/boards/zub_1cg:generate_psu_init` from the
minimal PS-only project in `create_ps_handoff.tcl`) — the latter was already
present and valid (matches its pinned sha256) and needed no regeneration;
it's what was actually used to flash below. Neither file is meant to be
committed — `bazel-out/` is fully gitignored and the finished migration
above makes `psu_init.tcl` gitignored too. There is no bitstream commit step
either: `applications/orbtrace/vivado/README.md` already documents the
Orbtrace bitstream as a `bazel-out`-only build product, never checked into
git (unlike the unrelated, still-committed Ethernet-loopback
`design_1_wrapper.bit`, which this session did not touch).

### Hardware reflash: DDR-hang bug confirmed fixed; TCP control path confirmed working

Flashed the new bitstream + the existing valid
`sdk/boards/zub_1cg/generated/psu_init.tcl` + a freshly built A53 ELF via
`tooling/xsct/jtag_flash.sh`. **No PS/DDR hang** — the exact failure this
handoff documented for the under-configured generic `psu_init.tcl` did not
reproduce, confirming the `create_bd.tcl` fix works on real hardware, not
just in simulation/synthesis.

`ping` to the board failed and initially looked like a regression, but is a
red herring: this firmware never calls `nx_icmp_enable()`, so it does not
answer ICMP at all (control/trace/DAP are TCP-only). A raw TCP connect to
port 3401 succeeded immediately, confirming the board, network stack, and
control service were all healthy.

### New finding: sustained real trace-data load hangs the board

The prior continuation's hardware verification only ever ran short bursts —
a ~20 s / ~330 KB run. This session ran `orbtrace configure ... test
swo-nrz ...`, `orbtrace start`, then a real capture, and partway through the
board stopped responding entirely:

- `isr_calls`, `rx_frames`, `tx_frames`, and `diag_tx_recover_attempts` all
  went completely flat across repeated UART reads spanning several seconds —
  not just network-driver-level stall, since `diag_tx_recover_attempts` (the
  polled TX-stall recovery in `gem2_tx_poll_recover()`, designed to retry
  every second indefinitely per its own doc comment) stopped incrementing
  too, meaning `main.c`'s diag thread itself stopped making progress.
- `diag_tx_recover_attempts` had reached 7 before freezing.
- A brand-new TCP connect attempt issued while UART was being captured
  produced zero reaction (`isr_calls` never moved), ruling out "no new
  traffic happened to arrive" as an innocent explanation — this is a real
  hang, not idle quiet.
- The board was recovered with a JTAG reflash (bitstream + psu_init + ELF,
  same as above) and confirmed responsive again afterward. It was left in
  this freshly-reflashed, responsive state at the end of this session.

This is a different failure from the already-fixed `gem2_packet_send()`
header-mutation bug (that fix's own diagnostic, `tx_prepend`/`tx_length`,
showed no drift in this session's run either, before the hang). Root cause
is unknown — worth investigating the 32-descriptor AXI DMA S2MM ring and/or
TX recovery interaction under real sustained throughput, but that
investigation was not carried out this session; this is a fresh, previously
unexercised failure mode to pick up next.

### Step 6 (RTL rate): analyzed, not yet implemented

Traced `applications/orbtrace/rtl/orbtrace_orbflow_encoder.sv` (used with
the production `MAX_PAYLOAD=1024`, instantiated from `orbtrace_core.sv`)
cycle-by-cycle. `input_ready` only asserts in the `LOAD` state, so `LOAD`
(ingest, ~1 cycle/byte) and `FIND`/`CODE`/`DATA` (COBS-encode and emit,
~2 cycles/byte for large groups) never overlap for a given packet — total
~3 cycles/byte at 100 MHz confirms the documented ~267 Mbit/s ceiling.

Proposed fix: double-buffer (ping-pong) the `packet[]` array so `LOAD` for
the next packet overlaps `FIND`/`CODE`/`DATA` draining the current one.
Since the encode phase (~2 cycles/byte) is slower than ingest (~1 cycle/
byte), sustained throughput becomes bound by the encode phase alone —
~2 cycles/byte → 50 MB/s → exactly the 400 Mbit/s target. (Overlapping
`FIND` for the next group with `DATA` emitting the current one would add
further headroom but isn't required to hit spec.) An existing `xsim`
testbench, `applications/orbtrace/rtl/tb/orbtrace_pipeline_tb.sv`, already
instantiates the encoder directly and checks COBS correctness cycle-by-
cycle, and is a workable starting point for verifying a redesign, though it
only exercises one packet today — back-to-back overlap tests would need to
be added. Not implemented this session; deferred pending the sustained-load
hang above, and by explicit choice to stop and write up findings rather than
continue open-ended hardware debugging.

### Repository state at end of session

All changes from this session (and the prior in-progress migration it
completed) are uncommitted in the working tree:
`applications/orbtrace/vivado/create_bd.tcl`,
`applications/orbtrace/vivado/build.tcl`, deletion of
`sdk/boards/zub_1cg/psu_init.tcl`, plus the previously-uncommitted
`sdk/boards/zub_1cg/{board_preset.tcl,create_ps_handoff.tcl,
export_psu_init.tcl,generate_psu_init.sh,publication_guard.bzl}` and their
`BUILD.bazel`/`artifacts.json`/`COMPATIBILITY.md`/`.gitignore` wiring. No
commit was made — nothing in this session was asked to be committed.
Verified `bazel build`/`bazel test` pass for the affected packages
(`sdk/boards/zub_1cg/...`, `applications/orbtrace/...`, and the rest of the
non-hardware-toolchain-dependent tree); a pre-existing, unrelated
`sdk/bsp/rpu` cross-compiler toolchain failure and missing
`third_party/{threadx,filex,netxduo}` vendored sources were both confirmed
pre-existing and out of scope.

## Continuation (follow-up session): sustained-load hang traced to a ThreadX priority-starvation bug; fix applied, not yet hardware-verified

This session picked up the "New finding: sustained real trace-data load
hangs the board" item above. The board and its JTAG/UART tooling were not
reachable from this session's environment (`192.168.1.50` did not respond to
ping, no `/dev/ttyUSB*`/`/dev/ttyACM*` device, and no `orbtrace`/`xsct`
binaries on `PATH`), so this was a code-only investigation: no reflash, no
new hardware run, no confirmation on real hardware. That verification is
still owed to whoever picks this up next.

### Root-cause hypothesis: `trace_thread` can starve `diag_thread` forever

`trace_thread_entry()` in `main.c` runs a tight loop calling
`trace_dma_send_completed()` and, when the Orbtrace AXI DMA ring has nothing
ready, calling `tx_thread_relinquish()`. Thread priorities set in
`tx_application_define()` are: the NetX IP thread = 1 (highest), `control`/
`dap` = 2, `trace` = 3, `diag` = 4 (lowest); all four are created with
`TX_NO_TIME_SLICE`. `diag_thread` is the *only* caller of
`gem2_tx_poll_recover()` in `ThreadXGEM2Driver.c` — by that function's own
design comment, polling from a non-ISR context once a second is "the only
signal left" to recover a genuinely stalled GEM2 TX ring, since the DMA
halting on a used descriptor does not reliably re-interrupt.

`tx_thread_relinquish()` is documented ThreadX behavior: it is a no-op when
the calling thread is the only one ready at its own priority level, which
`trace_thread` always is here. Combined with `TX_NO_TIME_SLICE`, ThreadX's
preemptive scheduler will never switch to `diag_thread` (priority 4) while
`trace_thread` (priority 3) stays ready — regardless of how long it has been
running — unless `trace_thread` itself blocks on something, which only
happens inside `nx_tcp_socket_send(..., NX_WAIT_FOREVER)` when the TCP
window is genuinely full. Under the short, low-rate captures verified
earlier in this handoff, the window filled and drained often enough that
`trace_thread` blocked regularly, letting `diag_thread` interleave — which
matches those earlier sessions' clean `diag5:` logs throughout the run.
Under **sustained** load, ACKs can keep draining the window fast enough that
the send call never blocks, so `trace_thread` can end up never yielding real
CPU time — permanently starving `diag_thread`. That means
`gem2_tx_poll_recover()` stops running, so if/when the GEM2 TX ring hits its
already-documented "DMA halts on a used descriptor" condition, nothing ever
pokes it back, and since `diag_thread` is also the only thread producing
UART output, this reads on the wire as a total, silent freeze — matching
every reported symptom (`isr_calls`, `rx_frames`, `tx_frames`, and
`diag_tx_recover_attempts` all flat together, and a fresh TCP connect
attempt getting zero reaction).

This is a hypothesis, not something confirmed on hardware this session — but
it is a real, demonstrable defect in the polling loop's scheduling behavior
independent of whether it is the full explanation for the specific hang
observed previously.

### Fix applied

`trace_thread_entry()`'s inner loop (`applications/orbtrace/firmware/a53_app/src/main.c`)
now counts every iteration (not just `TRACE_DMA_NOT_READY` ones) and calls a
real `tx_thread_sleep(1)` every 65536 iterations, regardless of whether the
DMA is continuously idle, continuously busy, or some mix of both. This
bounds the worst case: `diag_thread` (and anything else below priority 3)
can now be starved for at most a bounded, small number of iterations instead
of potentially forever, so a genuine hardware stall stays observable and
recoverable via `gem2_tx_poll_recover()` instead of silently permanent.

Verified only at the build/test level this session:
`bazel build //applications/... //sdk/... //tooling/...` (excluding the
pre-existing broken `sdk/bsp/rpu` cross-compiler target noted in the prior
continuation) and `bazel test //tests/...` both succeed, including
`orbtrace_a53_app_elf_test`. No hardware run was performed.

### What's still open

- This fix needs a real sustained-load hardware run (the same
  `orbtrace configure ... test swo-nrz ...` / `orbtrace start` /
  long-duration-capture sequence that triggered the hang) to confirm the
  freeze either no longer reproduces, or — if it does — that `diag_thread`'s
  counters keep advancing through it (proving starvation is fixed even if a
  separate underlying DMA-stall bug remains to chase).
- If the hang still reproduces with `diag_thread` now demonstrably still
  running, the next place to look is the Orbtrace AXI DMA S2MM ring itself
  (`TRACE_DMA_BASE` in `main.c`) — unlike the GEM2 TX ring, it currently has
  no `DMASR`-based halt/error detection or recovery at all;
  `trace_dma_send_completed()` only checks the per-descriptor completion bit,
  so a genuinely halted engine looks identical to "no data yet" and is never
  distinguished or recovered.
- Step 5 (uninterrupted 1 MB, 10 MB, then larger captures) and step 6
  (measuring the existing RTL rate / the ~267 Mbit/s encoder ceiling, and
  the proposed ping-pong Orbflow/COBS encoder redesign) remain exactly as
  described in the prior continuation — untouched this session.

## Continuation (same session, hardware run): starvation fix confirmed on real hardware; a separate, deeper GEM2 total-freeze bug isolated; no valid throughput number obtained

This picked up directly where the previous continuation left off: the board
and JTAG/UART tooling turned out to be reachable after all in this session's
environment (the earlier "not reachable" read was wrong — ICMP genuinely
isn't implemented by this firmware, so `ping` failing is expected; a raw TCP
connect to port 3401/3402 is the correct liveness check, and both succeeded
immediately). `/dev/ttyUSB1` (the FTDI JTAG+Serial combo's UART interface)
and `xsct`/`openocd`/`picocom` were all available through this repo's Nix
devShell once `XILINX_ROOT=~/opt/vitis` was set.

### Reflash and starvation-fix verification

Rebuilt the A53 ELF (containing the `poll_count`/`tx_thread_sleep(1)` fix
from the previous continuation), reflashed via
`XILINX_ROOT=~/opt/vitis XSCT=<nix xsct> BITSTREAM=<bazel-cache
zub_orbtrace.bit, sha256 30bed13e...> PSINIT=sdk/boards/zub_1cg/generated/psu_init.tcl
tooling/xsct/jtag_flash.sh <elf>` — no PS/DDR hang, board came up healthy
(`orbtrace stats`/`info` over TCP both responded immediately).

Ran `orbtrace configure 192.168.1.50 test swo-nrz 2000000`, `orbtrace
start`, then `orbtrace capture ... 2000000000` with a 100 s timeout, while
logging `/dev/ttyUSB1` at 115200 baud to a file for the same duration. The
UART log **confirms the starvation fix works**: `diag_thread`'s own tick
counter (one `diag:` block per second) ran for the full duration without
ever stalling, and its counters visibly progressed throughout — `isr_calls`
33→596, `rx_frames` 18→180, `tx_frames` 13→362,
`tx_recover_attempts` 2→7, `rxused_count` 0→52 — unlike the prior
continuation's fully-frozen UART output, where nothing advanced at all once
the hang hit. This is exactly what the fix was supposed to produce:
`gem2_tx_poll_recover()` and the diagnostic thread demonstrably kept running
under real sustained load instead of being starved out.

### But: a separate, deeper GEM2 interrupt-freeze bug reproduced twice

Despite `diag_thread` staying alive, the underlying GEM2/network hardware
still went completely silent partway through both attempts:

- **First attempt:** `isr_calls` (and every other ISR-driven counter)
  climbed steadily for about the first 40 seconds of the log, then froze at
  `isr_calls=596` for the remaining ~70+ seconds of the 100 s test window,
  confirmed by grepping every `diag: ISR=...` line's `isr_calls` value —
  identical on every tick from that point on. Zero new interrupts arrived
  for over a minute even though `diag_thread` kept polling every second.
  Only 236,916 bytes reached the host's capture file over the full 100 s
  (a few KB/s — nowhere near the ~267 Mbit/s RTL ceiling estimated in the
  prior continuation, let alone the 400 Mbit/s target). Once frozen, the
  host's ARP entry for the board eventually flipped to `FAILED` and
  `orbtrace stats`/`stop` started returning `No route to host` — the board's
  Ethernet interface had stopped answering even ARP, a step further than
  just a stalled TX/RX descriptor ring.
- Reflashed (recovered immediately, confirmed responsive via `orbtrace
  stats`) and tried a second, shorter (20 s) capture attempt. This time the
  very first `orbtrace configure` call already came back `No route to
  host` — the freeze reproduced almost immediately rather than after ~40 s.
  (This attempt's driving shell command had also been killed by an outer
  30 s timeout mid-capture on its first try, which may itself have
  contributed by leaving an unclosed TCP connection on the board — not
  eliminated as a confound.)

Net result: **no valid maximum-throughput measurement was obtained.** The
only real data is the ~40 s partial window from the first attempt, and even
that moved only a few hundred KB — not a meaningful rate figure, just
confirmation that whatever this freeze is, it dominates achieved throughput
completely and happens well before the previously-estimated RTL/encoder
ceiling becomes the binding constraint.

This is evidence of a **third, distinct bug** in the GEM2 path — separate
from both the already-fixed header-mutation/retransmit bug and the
starvation bug fixed this session. `gem2_tx_poll_recover()`'s TXQBASE resync
and the ISR's RXUSED resync are both still running (confirmed: recovery
counters advanced during the first attempt), so this isn't simply "the
existing recovery never runs" — it's that the recovery mechanisms are
insufficient for whatever state the hardware actually reaches. The
ARP-level unresponsiveness after the freeze — not just a stuck ring, but no
response to anything at all, including frames the driver never touches
above the MAC/PHY — points toward something more fundamental (a PHY link
drop, a GEM-internal state the current recovery paths don't reset, or an
unhandled fault) rather than the previously-diagnosed ring/descriptor
desyncs.

The board was left in a freshly-reflashed, responsive state (verified via
`orbtrace stats`) at the end of this session.

### Recommended next steps

- Instrument PHY link status directly (MDIO BMSR bit 2, the same register
  `gem2_phy_enable_tx_delay()` already reads) in the diag thread, so the
  next freeze can distinguish "PHY link actually dropped" from "GEM/DMA
  internal state wedged but link still up."
- Run a host-side `tcpdump` concurrently with the next attempt (none was
  running this session) to see whether any frames at all cross the wire in
  the seconds immediately before and after `isr_calls` stops advancing.
- Consider whether `gem2_tx_stall_recover()`/the RXUSED resync path need a
  more drastic fallback (e.g. a full `gem2_disable()`/re-`gem2_enable()`
  cycle, or toggling the PHY) when repeated recovery attempts don't restore
  interrupt activity, since the current recovery clearly ran several times
  in the first attempt without preventing the eventual total freeze.
- Until this is root-caused, no throughput number in this document should
  be treated as representative — the 400 Mbit/s (and even the ~267 Mbit/s
  RTL-ceiling) questions are moot while the link can freeze solid within
  tens of seconds under real load.

## Continuation (same session): executed the recommended next steps — PHY-link diagnostics, ThreadX tracing, a full-reinit recovery fallback, and a concurrent tcpdump — found a concrete new lead (a stuck NetX retransmission-queue entry) and one likely-harmful new recovery path

This picked up the "Recommended next steps" list from the continuation
directly above and executed all four items on real hardware. Two positive
results, one negative result that was caught and reverted, and one new,
much more specific lead.

### PHY link status: added, and it rules out a PHY-level link drop

`gem2_diag_get_phy_link()` (`ThreadXGEM2Driver.c`) reads BMSR (MDIO register
1) live via the PHY address `gem2_phy_enable_tx_delay()` already resolves at
enable time (now cached in `sCtx.phy_addr`/`phy_found`), and prints as a new
`diag6:` line from `main.c`'s diag thread. Across every freeze reproduced
this session, `link_up` read `1` continuously, including during and after
the freeze. This rules out "the PHY dropped the physical link" as the
mechanism — whatever is failing is at the MAC/DMA/NetX level with the RGMII
link still up.

### ThreadX event tracing: enabled the infrastructure, but disabled the actual `tx_trace_enable()` call — it made things worse, not better

`third_party/threadx/BUILD.bazel`'s `threadx_a53` target now compiles
`common/src/tx_trace_*.c` (previously excluded) and sets
`TX_ENABLE_EVENT_TRACE` plus a `TX_TRACE_TIME_SOURCE` override via `defines`
(this single-core port's `tx_port.h` defaults that macro to
`_tx_thread_smp_time_get()`, an SMP-only symbol that doesn't exist in this
non-SMP build and fails to link/compile without the override; the override
had to be a public `defines` entry, not `copts`, because NetX Duo's own
`nx_trace_event_insert.c` — compiled into a separate `netxduo` library that
depends on `threadx_a53` — hits the identical undefined-symbol error
otherwise).

Calling `tx_trace_enable()` itself, however, produced a **new, more severe
failure** the first time it was tried on real hardware: the entire board —
not just GEM2 interrupts, but `diag_thread`'s own 1 Hz ticking — went
completely silent within seconds of the very first real client TCP
connection (a plain `orbtrace stats` control-port request), before any trace
capture was even attempted. This is worse than anything seen without
tracing enabled (which took tens of seconds of sustained *trace* load to
reproduce). A controlled A/B test confirmed it: reflashing with the
`tx_trace_enable()` call removed (kept commented out via `#if 0` in
`main.c`, with the build infrastructure otherwise unchanged) immediately
fixed it — the same kind of control connection completed normally
(`tcp_conns=1`, board stayed fully responsive). The call is disabled for
that reason; re-enabling it needs its own root-causing first (candidates:
the 16 KiB trace buffer or the `registry_entries=16` argument being
undersized or misaligned for what NetX's own trace instrumentation expects,
or a NetX-specific registration step this driver never calls). The compiled-
in-but-inert trace hooks (present because `TX_ENABLE_EVENT_TRACE` is still
defined) did not by themselves cause any observed problem — only calling
`tx_trace_enable()` did.

### A full MAC/PHY reinit recovery fallback: correctly triggers, but does not restore interrupt activity, and may itself be unsafe

Added `gem2_link_poll_recover()` + `gem2_full_reinit()` (`ThreadXGEM2Driver.c`),
gated by a new `gem2_set_trace_active()` flag that `main.c`'s `trace_thread`
sets only while a capture is actually running (so ordinary idle periods,
where `isr_calls` legitimately never advances, can't trigger a false-
positive reinit — confirmed: `link_recover_attempts` stayed `0` through a
30 s idle-only boot check). While active, if `isr_calls` doesn't advance for
3 consecutive 1 s polls, it escalates past the existing TXQBASE-only
`gem2_tx_stall_recover()` to a full `XEmacPs_Stop()`/re-arm-both-rings/
`XEmacPs_Start()`/PHY-autonegotiation-restart cycle.

Reproducing the sustained-load freeze with this in place confirmed the
detector fires correctly: `link_recover_attempts` advanced from `0` to `2`
while `isr_calls` sat frozen at `394`. But interrupt activity never resumed
after either attempt, and diag_thread's own logging stopped shortly after
the second attempt (no further `diag:`/`diag6:` lines at all, for the rest
of a 180 s UART capture) — a worse outcome than the pre-existing
`gem2_tx_poll_recover()`-only behavior from the prior continuation, where
`diag_thread` kept running (just ineffectively) for the entire freeze. The
leading suspect is `gem2_full_reinit()`'s unconditional
`nx_packet_transmit_release()` over every slot in `sCtx.tx_pkts[]`: if NetX
still has its own reference to one of those same `NX_PACKET`s (plausible
given the retransmission-queue bug found below), this doubly releases a
packet NetX believes is still in flight, which is exactly the shape of bug
that previously corrupted NetX-internal state elsewhere in this codebase
(see the ARP-table race documented above `gem2_arp_learn()`). **This
recovery path should be treated as unproven and possibly actively harmful,
not adopted as-is** — it needs either a way to reconcile with NetX's own
bookkeeping before releasing in-flight packets, or to avoid touching
`tx_pkts[]` at all and rely on completions draining naturally after the
MAC restarts.

### Concurrent tcpdump: found a specific, actionable new lead

Running `tcpdump host 192.168.1.50` on the host during the sustained-load
freeze captured the exact moment of failure. The board's last new data
(TCP seq up to 128452) was sent and cleanly ACKed by the host at
12:25:48.642284. Then, at 12:25:49.638367 — one second later, a classic TCP
RTO — **the board retransmitted TCP segment 122855:123887, a range the host
had already cumulatively acknowledged three exchanges earlier** (ack=128452,
which is past the end of that segment). The host correctly re-ACKed
`ack=128452` each time, telling the board it already has everything. The
board retransmitted the *same* already-acknowledged segment two more times
with growing backoff (at +1.39 s, then +5.66 s — textbook exponential RTO
backoff), and immediately after the third retransmission the host's ARP
requests for the board started going unanswered.

This is a materially different and more specific bug than anything
diagnosed so far: not the already-fixed `gem2_packet_send()` header-
mutation bug (that fix's own `diag5:` counters showed no prepend/length
drift in any of this session's runs), and not a PHY link drop (`diag6:`
showed link up throughout). It looks like **a stuck entry in NetX's own TCP
retransmission queue that never gets cleared despite the peer's cumulative
ACK superseding it**, and the board's retransmission timer keeps re-arming
for it every RTO until something breaks entirely. The most plausible
trigger, not yet confirmed: `gem2_packet_send()`'s ring-full path
(`sCtx.tx_count >= GEM2_TX_BD_COUNT`) calls `nx_packet_transmit_release(pkt)`
and returns `NX_NOT_SUCCESSFUL` immediately, with a comment saying "caller
must handle retransmit" — if NetX's TCP retransmission bookkeeping for that
segment doesn't get cleanly reconciled against a send that failed *after*
the packet was already released back to the pool, a later successful resend
of overlapping data could leave a stale, un-clearable queue entry pointing
at the wrong thing. This needs verifying against NetX Duo's own
`nx_tcp_socket_send`/retransmission-queue source, not just this driver.

### Recommended next steps

- Root-cause the stuck-retransmission-queue lead above first — it's the
  most specific, most directly evidenced lead produced so far, with an
  exact wire-level timestamp and sequence range to work from
  (`capture.pcap`-style capture, kept in this session's scratchpad, not
  committed anywhere).
- Do **not** adopt `gem2_full_reinit()` as a trusted recovery path without
  first addressing the double-release risk on `sCtx.tx_pkts[]` described
  above.
- Re-attempt `tx_trace_enable()` only after the retransmission-queue bug is
  fixed — it's possible the crash-on-first-connection this session saw was
  itself a symptom of the same underlying NetX state issue interacting badly
  with the trace hooks' extra bookkeeping, not a problem with the trace
  buffer sizing itself; re-test in isolation once the root NetX bug is gone.
- Still no valid maximum-throughput number: this session's runs moved
  10–20 KB/s before freezing, consistent with the prior continuation, not
  the RTL/encoder ceiling question.

## Process state at handoff

A `tcpdump host 192.168.1.50` process (PID 214766/214769, started this
session via passwordless `sudo`) is still running, writing to this session's
scratchpad `capture.pcap` — `sudo kill` was attempted and requires a
password this environment doesn't have, so it was left running rather than
force-killed another way; it is harmless (read-only capture) but should be
stopped manually if it's not wanted. All firmware changes described above
are uncommitted in the working tree, same as the rest of this document. The
board was left freshly reflashed and responsive (verified via `orbtrace
stats`) at the end of this session, matching every prior continuation's
end state.

The interrupted diagnostic processes started during this work were stopped. A
pre-existing unrelated tcpdump process remains running and was intentionally
left untouched.
