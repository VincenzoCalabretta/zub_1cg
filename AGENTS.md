# zub_1cg — Hardware notes

## Repository conventions

Merged in from the former `AGENT.md` (removed; this file is now the single
agent-guidance entry point).

### Host-side tooling

- Do not add Python tooling, Python runtime dependencies, or Python-based test
  runners to this repository. Host-side tools and their tests are Rust.
- Build, test, and run Rust tools through Bazel. Do not document or rely on
  standalone `cargo build`, `cargo test`, or checked-in `target/` outputs.
- Rust binaries belong behind Bazel `rust_binary` targets so their outputs are
  cached in Bazel's output tree. Use `bazel build`, `bazel test`, and
  `bazel run` with `--config=host` for host tools.

### Rust dependencies

- Keep each Rust tool's `Cargo.toml` and `Cargo.lock` checked in as dependency
  inputs, not as an alternative build path.
- Register every tool's crates with the `crate_universe` module extension in
  `MODULE.bazel` via `crate.from_cargo`, then consume the generated repository
  from its Bazel targets. This keeps dependency fetching and compilation under
  Bazel's repository rules and cache.
- When a manifest or lockfile changes, allow Bazel to update `MODULE.bazel.lock`
  and commit that lockfile update with the dependency change.

### PDF documentation

Regenerating the private engineering PDF-to-Markdown conversion is documented
in `internal/docs_tool/README.md` — see that file, not here, for the current
procedure and output layout.

## Board

- **Hardware:** Avnet AES-ZUB-1CG-ED-G (Zynq UltraScale+ ZU1CG)
- **Boot mode (bring-up):** JTAG — all DIP switches OFF (BOOT_MODE=0000)
- **USB:** FT2232H → `ttyUSB0` = JTAG (OpenOCD), `ttyUSB1` = UART console

## Two boot targets

This mainline covers both cores:

| Core | Cross-toolchain | Bazel platform | Config |
|---|---|---|---|
| APU (Cortex-A53) | aarch64-none-elf | `//sdk/platforms:apu_a53` | firmware macro transition |
| RPU (Cortex-R5F) | arm-none-eabi | `//sdk/platforms:rpu_r5_0` | firmware macro transition |

## R5 memory map (runtime)

| Region | R5 address | AXI address | Notes |
|---|---|---|---|
| OCM (code + data + BSS) | 0x00000000–0x000042C8 | 0xFFFC0000–0xFFFC42C8 | via OCM remap |
| Stack (grows down) | 0x00010000–0x00020000 | 0xFFFC0000+offset | all modes |
| UART1 | 0xFF010000 | 0xFF010000 | physical, not remapped |

ELF is linked at VMA `0x00000000` and written into OCM at AXI `0xFFFC0000`.
OCM remap register (`0xFF960000` bit 0 = 1) makes R5 address `0x00000000` hit OCM.

## Key hardware facts (R5)

- **UART1 ref clock:** IOPLL (FBDIV=50 × 33.333 MHz / DIV0=24) = 69.44 MHz
  → CD=43, BDIV=13 → 115200 baud (0.1% error).
- **RPU_GLBL_CNTL** (`0xFF9A0000`) power-on default = `0x00000050`.
  bit[3] SLSPLIT=0 = lock-step mode — **must write 0x8 (split) while R5 is
  in reset**.
- **XPPU rule:** CRL_APB and RPU registers are writable from JTAG only while
  R5 is held in module reset (`RST_LPD_TOP` bit0=1). After R5 is released
  they are locked. **Power-cycle is the only reset.**

## OpenOCD scripts

| Script | Purpose |
|---|---|
| `scripts/openocd/aes_zub.cfg` | FT2232H interface + ZU+ TAP chain + STICKYERR clear on init |
| `scripts/openocd/load_r5.tcl` | Full boot: OCM remap → RPU config → UART init → ELF load → R5 release |
| `scripts/openocd/scan_aps.tcl` | Safe AP scan (run before adding any cortex_r4 target) |
| `scripts/openocd/check_xmpu.tcl` | Dump XMPU_OCM registers |
| `scripts/openocd/check_stickyerr.tcl` | Read DAP CTRL/STAT to check STICKYERR bit |

**Never add a `cortex_r4` target to `aes_zub.cfg`** without first finding
the correct AP number via `scan_aps.tcl`. A wrong AP number sets
STICKYERR and silently corrupts all subsequent AXI reads/writes until the
next power cycle.

## Diagnosing a failed R5 run

1. Check OpenOCD log: `RPU_GLBL_CNTL = 0x00000008` and `UART1_SR` TX_EMPTY set.
2. If UART shows "BOOT" but no "Hello": R5 executing but stuck — check BSS:
   ```
   read_memory 0xFFFC2BCC 32 4   # should be all zeros once startup.S clears BSS
   ```
3. If nothing on UART: verify MIO routing and UART1 clock.
4. If all CRL_APB writes fail: power-cycle the board (XPPU lockout from
   previous run).

## Testing without picocom

`zub_ctl serial-watch --tty /dev/ttyUSB1 --expect '...' --timeout 30`
is a scriptable replacement for `picocom` that emits each line prefixed
with `[SERIAL] ` and exits 0 / 1 based on regex matches. Used by the
`//tests/…` sh_tests.

## Regenerating board artifacts

`board/zub_1cg/design_1_wrapper.bit` and `psu_init.tcl` are opaque blobs
regenerated in the Xilinx workflow:

1. Open the Vivado project (`../vivado_workspace/zub_hello_world_ethernet/`).
2. Generate bitstream → export hardware handoff (`.xsa`).
3. Vitis: platform from `.xsa` → extract `psu_init.tcl` and the `.bit`.
4. Copy both into `board/zub_1cg/`.

## Xilinx toolchain (xsct / vivado) for JTAG flashing

`nix develop`'s `xsct`/`vivado` are thin FHS wrappers (see `flake.nix`'s
`mkXilinxTool`) around a **separately installed, licensed** Vivado/Vitis
2023.2 tree — the wrapper itself ships no AMD software. They refuse to run
until `XILINX_ROOT` is set to the root containing `Vivado/2023.2` and
`Vitis/2023.2` (e.g. `export XILINX_ROOT=/path/to/your/xilinx/install`).
Without it, `tooling/xsct/jtag_flash.sh` and friends fail immediately with
`set XILINX_ROOT to the root containing Vivado/2023.2 and Vitis/2023.2`.
This is a per-machine/per-user setting — check `echo $XILINX_ROOT` and where
a real install actually lives on the current machine before assuming the
toolchain is missing entirely.

## Host network link to the board

The host side is typically a USB-Ethernet adapter. Its interface name can
change across USB re-enumeration (e.g. after a JTAG reflash power-cycles the
bus) even though its MAC address doesn't. A bare `ip addr add
192.168.1.1/24 dev <iface>` can get silently flushed by NetworkManager if
the device is tracked-but-unmanaged. Prefer a NetworkManager connection
profile bound to the adapter's MAC address instead of the (unstable)
interface name:

```
nmcli connection add type ethernet con-name <name> mac <adapter-mac> ip4 192.168.1.1/24
nmcli connection up <name>
```

This survives renumbering and (on typical desktop polkit setups) doesn't
need sudo, unlike `ip addr add`.

## Known gotcha: committed bitstream is the wrong design

As of this writing, `sdk/boards/zub_1cg/design_1_wrapper.bit` and
`sdk/boards/zub_1cg/psu_init.tcl` / `sdk/boards/zub_1cg/generated/psu_init.tcl`
are for the **Ethernet-loopback-capable design** — see
`sdk/boards/zub_1cg/artifacts.json`'s own `role` field — **not** the
Orbtrace application. Flashing them and then starting an Orbtrace trace
capture will correctly fail DMA init (`trace DMA initialization failed`)
because there's no real AXI DMA at that PL address in that image. The
corrected Orbtrace bitstream (matching the hash recorded in
`documentation/ORBTRACE_400MBPS_HANDOFF_2026-08-09.md`) has only ever existed as a
same-session Bazel-cache artifact (`bazel-out/orbtrace-vivado/zub_orbtrace.bit`,
produced by a manual/local Vivado build) — it has never been committed to
this repo, and a Bazel cache can be garbage-collected at any time. Before
assuming a hardware test failure is a firmware bug, confirm which bitstream
is actually flashed against `artifacts.json`.

Relatedly, `applications/orbtrace/vivado/create_bd.tcl` originally never
called `apply_bd_automation ... {apply_board_preset "1"}` on the PS block
(compare `sdk/boards/zub_1cg/board_preset.tcl`'s `zub1cg_apply_ps_preset`,
which exists specifically to assert the board's real LPDDR4 geometry),
leaving DDR at generic IP defaults so a `psu_init.tcl` exported directly
from that design's own XSA hung forever calibrating DDR against the wrong
parameters. **This is fixed as of a 2026-08-09 follow-up session** —
`create_bd.tcl`/`build.tcl` now call `zub1cg_apply_ps_preset` and select the
correct board part before `create_project` — see that day's handoff for the
exact fix and a hardware-verified fresh build (bitstream sha256
`30bed13e...`). The fix itself is still uncommitted, same as everything else
in this file's "known gotcha" above; a fresh Vivado build still only lives
in the Bazel cache, never in git (see the next section for how to
build/find it). Do not assume the DDR hang is still open before checking
whether that fix is present in the working tree.

## Orbtrace A53 hardware test workflow (end-to-end, verified 2026-08-09)

This section is the practical howto for actually driving the board on this
machine to test the Orbtrace A53 firmware — build, flash, talk to it over
Ethernet, and read diagnostics. It complements (doesn't replace) the
`documentation/ORBTRACE_400MBPS_HANDOFF_2026-08-09.md` series of documents, which track
the actual bug investigation and current findings; this section is the
tooling reference so the next session doesn't have to re-derive it.

### Topology

| | |
|---|---|
| Board IP | `192.168.1.50` |
| Host IP (on the point-to-point link) | `192.168.1.1` |
| Control port (TCP) | `3401` — device info / start / stop / status |
| Trace port (TCP) | `3402` — Orbflow payload stream |
| DAP port (TCP) | `3240` — CMSIS-DAP passthrough |
| Board MAC (fixed, firmware-hardcoded) | `00:0a:35:00:01:02` (`sMAC` in `ThreadXGEM2Driver.c`) |

**The firmware never calls `nx_icmp_enable()` — `ping` to `192.168.1.50`
always fails and does not indicate anything is wrong.** Use a raw TCP
connect or the `orbtrace` CLI (below) as the actual liveness check:

```bash
timeout 5 bash -c 'exec 3<>/dev/tcp/192.168.1.50/3401 && echo OK'
```

If that also fails (`No route to host` / `Connection timed out`), check
`ip neigh show | grep 192.168.1.50` — a `FAILED` entry (as opposed to
`REACHABLE` or no entry at all) means the board has stopped answering ARP,
which is the observed end state of the still-open sustained-load hang (see
the handoff) — it needs a JTAG reflash to recover, not a host-side fix.

### One-time host network setup

The host side is a USB-Ethernet adapter whose interface name
(`enp0s20f0uN`) can renumber across USB re-enumeration (e.g. a JTAG reflash
power-cycling the bus) even though its MAC doesn't. Bind a NetworkManager
profile to the MAC instead of the interface name (survives renumbering,
usually doesn't need sudo):

```bash
nmcli connection add type ethernet con-name zub_1cg-board mac <adapter-mac> ip4 192.168.1.1/24
nmcli connection up zub_1cg-board
```

On this machine the adapter's MAC is `00:e0:4c:75:87:68`; find yours via
`ip link show` (look for the interface with `192.168.1.0/24` context) if
different hardware is in use.

### Environment: `XILINX_ROOT`

`nix develop`'s `xsct`/`vivado`/`openocd`/`picocom` are all available in the
devShell. `xsct`/`vivado` additionally need `XILINX_ROOT` pointed at a real,
separately-licensed Vivado/Vitis 2023.2 install — on this machine that's
`/home/v/opt/vitis` (contains `Vivado/`, `Vitis/`, etc. as subdirectories).
Without it: `set XILINX_ROOT to the root containing Vivado/2023.2 and
Vitis/2023.2`.

### Build the firmware ELF and host CLI

```bash
nix develop -c bazel build //applications/orbtrace/firmware/a53_app:a53_app
nix develop -c bazel build //applications/orbtrace/model:orbtrace
# outputs:
#   bazel-bin/applications/orbtrace/firmware/a53_app/a53_app   (ELF, symlink into the cache)
#   bazel-bin/applications/orbtrace/model/orbtrace             (host CLI binary)
```

`bazel build //...` will fail on two **pre-existing, unrelated** things —
don't chase these, they're already known:
`third_party/{filex,threadx,netxduo}` are fetched by a Bazel module
extension (see `third_party/extensions.bzl`) and its glob patterns fail to
evaluate until Bazel actually materializes those external repos as part of
a real build graph (building any target that depends on them, e.g. the
a53_app target above, works fine); `//sdk/bsp/rpu/...` fails to compile with
`-mfpu=vfpv3-d16`/`-marm` unrecognized by the `gcc` picked up on `PATH` in
this environment — scope builds to `//applications/... //sdk/... //tooling/...`
(minus `//sdk/bsp/rpu/...`) or `//tests/...` to avoid both.

### Find (or build) the real Orbtrace bitstream + `psu_init.tcl`

**The bitstream and `psu_init.tcl` committed in this repo
(`sdk/boards/zub_1cg/design_1_wrapper.bit`,
`sdk/boards/zub_1cg/psu_init.tcl`) are for the Ethernet-loopback design, not
Orbtrace** — see the "Known gotcha" section above. The real Orbtrace
bitstream only ever exists as a Bazel-cache build artifact, never committed:

```bash
find ~/.cache/bazel -iname "zub_orbtrace.bit" 2>/dev/null
```

If nothing is found, it needs a fresh Vivado build (see
`documentation/ORBTRACE_400MBPS_HANDOFF_2026-08-09.md`'s "Fresh Vivado build" continuation
for the exact `build.tcl` invocation and expected timing/WNS — this takes
real synthesis+implementation wall-clock time, not seconds).

**Use the Orbtrace-specific `psu_init.tcl`, not the generic board-level
one, for any hardware session touching the PS trace path** (R5/A53
CoreSight, `PS_CORESIGHT_TRACE_PLAN.md`, or the M3's own Parallel-mode
trace clock). `build.tcl` already exports one alongside every bitstream —
it sits right next to `zub_orbtrace.bit` in the same output directory
(`<build_dir>/psu_init.tcl`, produced via `write_hw_platform` +
`export_psu_init.tcl`). `PS_CORESIGHT_TRACE_PLAN.md`'s Phase 6 section 14
root-caused a real, previously-unnoticed bug from *not* using this file:
the generic `sdk/boards/zub_1cg/generated/psu_init.tcl` (DDR/PS config,
shared across applications, *not* Orbtrace-specific — regenerated on
demand and gitignored, see below) never programs `CRF_APB.DBG_TRACE_CTRL`
at all, leaving the PS trace-port output clock permanently disabled
(reset default) — every trace-related PL signal can be perfectly
configured and it still won't produce a single byte without this. Confirm
on real hardware with `grep -c DBG_TRACE_CTRL <psu_init.tcl>` before
trusting either file for trace work — `0` means it's the wrong one.

```bash
ls sdk/boards/zub_1cg/generated/psu_init.tcl   # generic board-level; present + valid if sha256 matches generated/psu_init.sha256
# if missing/stale:
nix develop -c bazel run //sdk/boards/zub_1cg:generate_psu_init
```

### Flash over JTAG

```bash
nix develop -c env \
  XILINX_ROOT=/home/v/opt/vitis \
  XSCT="$(nix develop -c which xsct)" \
  BITSTREAM=<path to zub_orbtrace.bit found/built above> \
  PSINIT=<path to the SAME build's own psu_init.tcl -- e.g. bazel-out/<build>/psu_init.tcl;
          use sdk/boards/zub_1cg/generated/psu_init.tcl ONLY for non-trace work> \
  bash tooling/xsct/jtag_flash.sh bazel-bin/applications/orbtrace/firmware/a53_app/a53_app
```

This auto-launches an `hw_server`, resets the system, programs the PL
bitstream, runs `psu_init`, and downloads+starts the ELF on A53 core 0.
Takes well under a minute. `Done. Connect serial terminal at 115200 baud.`
is the expected final line; anything else (especially the script exiting
non-zero) means the flash itself failed, not the firmware.

There is no separate "power cycle" step needed between reflashes for this
flow — `jtag_flash.sh`'s own `rst -system` is sufficient, including to
recover from the sustained-load hang described in the handoff (confirmed
repeatedly this session: reflash always brought the board back to a
responsive state, even from a fully ARP-unresponsive freeze).

### Read the UART console

The board exposes a combo FTDI "JTAG+Serial" USB device; on this machine
its serial interface lands at `/dev/ttyUSB1` (`udevadm info -q property -n
/dev/ttyUSB1` shows `ID_MODEL=JTAG+Serial`, `ID_USB_INTERFACE_NUM=01` — the
JTAG side is a different USB interface entirely and is not a tty at all,
it's used directly by `hw_server`/`xsct`/`openocd` over libusb). No
interactive terminal is needed for scripted use:

```bash
stty -F /dev/ttyUSB1 115200 raw -echo
timeout 30 cat /dev/ttyUSB1 > uart.log 2>&1   # or any duration; blocks until timeout
```

(`zub_ctl serial-watch` — see below in this file — is the pattern the
`tests/…` sh_tests use when a regex-driven pass/fail check is wanted instead
of a raw capture; either works.) The board free-runs and prints
unconditionally once booted — nothing needs to be "started" on the UART side
to get diagnostic output; it starts as soon as `diag_thread` in `main.c`
begins ticking (~1 s after boot).

### Diagnostic line reference (`main.c`'s `diag_thread_entry()`, once per second)

| Prefix | Contents |
|---|---|
| `diag:` (1st) | Raw GEM2 `ISR`/`NWSR`, `isr_calls`/`rx_frames`/`tx_frames`, last frame's ethertype/length, TX completion/BD-ring state, TXQBASE resync recovery counters (`gem2_tx_poll_recover()`), last resolved TX destination, bad-destination drop count |
| `diag5:` | TX packet header-mutation/retransmit diagnostics: `tx_retransmit_count` (same `NX_PACKET` pointer seen twice = a real NetX retransmit), prepend/length before vs. after — should always show identical before/after (the already-fixed header-mutation bug's regression check) |
| `diag6:` | **Added 2026-08-09.** Live PHY link status (`gem2_diag_get_phy_link()`, MDIO BMSR bit 2) plus `link_recover_attempts` — how many times `gem2_link_poll_recover()`'s full MAC/PHY reinit fallback has fired. See the handoff: this fallback is confirmed to trigger correctly but not confirmed to actually fix anything, and its blanket `nx_packet_transmit_release()` over in-flight TX packets is flagged as a possible new bug — don't assume it's a safe recovery path |
| `diag:` (2nd) | This driver's own IP→MAC ARP cache (bypasses a documented race in NetX's own ARP table — see `gem2_arp_learn()`'s comment) |
| `diag2:` | NetX's own `NX_IP_INFO`/`NX_TCP_INFO` counters (invalid/checksum-error/dropped packets, connection counts, packet pool availability) |
| `diag:` (3rd, conditional) | Raw hex dump of the last received IP header, only if `last_etype==0x0800` |
| `diag:` (4th, conditional) | Raw hex dump of the last received ARP message, only if valid |
| `diag:` (5th) | Raw dump of the `NX_IP_DRIVER` request struct from the most recent `gem2_packet_send()` call |
| `diag3:` | Raw RX BD ring dump (all 4 descriptors' ADDR/STAT words + software `rx_tail` + hardware `RXQBASE`) |
| `diag4:` (conditional) | Raw TX BD ring dump, only printed once `tx_count > 0` |

ThreadX event tracing (`tx_trace_enable()`) is built into the toolchain
(`third_party/threadx/BUILD.bazel`'s `threadx_a53` target compiles the trace
sources and sets `TX_ENABLE_EVENT_TRACE`) but the actual enable call in
`main.c` is currently `#if 0`'d out — enabling it caused a *worse* freeze
than the bug it was meant to help diagnose (confirmed via A/B test on real
hardware). Don't re-enable without reading that finding in the handoff
first.

### Drive the board: the `orbtrace` CLI

```bash
ORBTRACE=bazel-bin/applications/orbtrace/model/orbtrace
$ORBTRACE info 192.168.1.50                                        # device identity string
$ORBTRACE stats 192.168.1.50                                        # rx_bytes/dropped_bytes/sync_loss/fifo_high_water/dma_faults
$ORBTRACE configure 192.168.1.50 test swo-nrz 2000000               # PL deterministic test source, no real probe needed
$ORBTRACE start 192.168.1.50
$ORBTRACE capture 192.168.1.50 out.bin <max-bytes>                  # blocks until max-bytes or connection ends; wrap in `timeout`
$ORBTRACE stop 192.168.1.50
```

`tests/orbtrace_throughput_test.sh` is the canonical example of chaining
these with timing/rate-limit checks. **Timeout-budgeting gotcha:** if you
wrap a whole `configure`/`stats`/`start`/`capture`/`stats`/`stop` sequence
(each individually wrapped in its own `timeout N`) in an *outer* `timeout`
or Bash-tool-level timeout, make sure the outer budget exceeds the sum of
every inner one — a premature outer kill mid-`capture` looks identical to a
real board hang (both leave the board's TCP connection in a torn-down state
and `orbtrace stats`/`stop` immediately after report `No route to host`)
and can send you chasing a phantom repro.

### Capturing wire-level traffic concurrently

`sudo -n tcpdump -i <iface> -w out.pcap host 192.168.1.50 &` worked
passwordlessly on this machine to *start* a capture but **not** to `kill`
it afterward (`sudo -n kill` prompts for a password) — if you start one,
expect to leave it running for the rest of the session, which is harmless
(read-only) but worth telling the user about explicitly. This concurrent
capture is what actually found the most specific lead in the 2026-08-09
sustained-load investigation (a spuriously-retransmitted, already-ACKed TCP
segment) — worth doing again for any further work on that bug.
