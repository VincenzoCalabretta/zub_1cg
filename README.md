# zub_1cg — Mainline for the Avnet AES-ZUB-1CG

Bare-metal + Eclipse ThreadX applications for the **Avnet AES-ZUB-1CG-ED-G**
board (Zynq UltraScale+ ZU1CG), spanning both the Cortex-A53 (APU) and
Cortex-R5F (RPU) cores.

Build system: **Bazel 8** (Bzlmod). Environment: **Nix flake** (hermetic
devShell). Test driver: **Rust** (`//tools/zub_ctl`).

---

## Quick start

```bash
nix develop                                 # enter hermetic devShell
bazel build --config=apu //apps/apu/...     # A53 apps
bazel build --config=rpu //apps/rpu/...     # R5 apps
bazel build --config=host //tools/...       # host-side tools + tests
```

## Repository layout

```
zub_1cg/
├── MODULE.bazel, .bazelrc, extensions.bzl
├── flake.nix                              # bazel_8, gcc-arm-embedded,
│                                          # aarch64-embedded, bootgen,
│                                          # openocd, picocom, rustc, cargo, …
├── platforms/                              # host, apu_a53, rpu_r5_0
├── toolchains/
│   ├── aarch64_none_elf/                   # A53 cross-toolchain
│   └── arm_none_eabi/                      # R5 cross-toolchain
├── board/
│   ├── apu/                                # A53 ThreadX BSP (timer, vectors)
│   ├── rpu/                                # R5 BSP (startup, UART, timer)
│   ├── a53_loader/                         # AArch64 shim: hands R5 off
│   └── zub_1cg/                            # PL bitstream + psu_init.tcl
├── apps/
│   ├── apu/hello_world/                    # A53 ThreadX RGB LED demo
│   ├── apu/eth_loopback/                   # A53 GEM2 internal loopback
│   └── rpu/hello_world/                    # R5 ThreadX Hello World
├── third_party/
│   ├── threadx/                            # BUILD overlay: :threadx_a53, :threadx_r5
│   ├── filex/, netxduo/                    # (A53 only)
│   ├── xilinx_bsp/                         # vendored A53 BSP libs + headers
│   └── os_abstraction_layer/               # ThreadXGEM2Driver + encore/OS
├── tools/zub_ctl/                          # Rust CLI: serial-watch, watch-r5,
│                                           # watch-a53 (see below)
├── scripts/
│   ├── openocd/                            # JTAG boot for R5 (aes_zub.cfg,
│   │                                       # load_r5.tcl, diag/*.tcl)
│   ├── xsct/                               # A53 flash via Vitis xsct
│   └── flash/                              # BOOT.BIN builders (boot_*.bif,
│                                           # build_boot_a53.sh)
└── tests/                                  # sh_test wrappers around zub_ctl
```

## Applications

| App | Core | Toolchain | Entry point | Test |
|---|---|---|---|---|
| `//apps/apu/blink` | A53 | aarch64-none-elf | 0x0 | `//tests:apu_blink_test` |
| `//apps/apu/hello_world` | A53 | aarch64-none-elf | 0x800 | `//tests:apu_hello_world_test` |
| `//apps/apu/eth_loopback` | A53 | aarch64-none-elf | 0x0 | `//tests:apu_eth_loopback_test` |
| `//apps/rpu/hello_world` | R5F | arm-none-eabi | 0xFFFF0128 (OCM) | `//tests:rpu_hello_world_test` |

## Testing on the board

Seven board tests are provided under `//tests/…`. Each is tagged
`manual + exclusive + requires-hardware`, so wildcard `bazel test //...`
does not run them. To run a single test:

```bash
bazel test --config=host --config=onboard //tests:rpu_hello_world_test    # R5 JTAG boot
XSCT=/path/to/xsct bazel test --config=host --config=onboard //tests:apu_hello_world_test  # A53 xsct flash
XSCT=/path/to/xsct bazel test --config=host --config=onboard //tests:apu_eth_loopback_test # A53 eth loopback
```

Each test wraps [`zub_ctl`](tools/zub_ctl/README.md) — a Rust CLI that:

1. Opens `/dev/ttyUSB1` at 115200 baud **before** the boot subprocess.
2. Streams every line as `[SERIAL] <line>`.
3. Runs `openocd` (R5) or `xsct` (A53) with output streamed as `[OCD] …`
   / `[XSCT] …`.
4. Matches `--expect` regexes in order; exits 0 when all match, 1 on
   timeout or `--fail-on` hit.

### Environment variables

| Var | Default | Meaning |
|---|---|---|
| `ZUB_TTY` | `/dev/ttyUSB1` | Serial device |
| `ZUB_BAUD` | `115200` | Baud rate |
| `ZUB_TIMEOUT` | `30` | Serial-watch timeout in seconds |
| `XSCT` | `/mnt/data/xilinx/Vitis/2023.2/bin/xsct` | A53 tests only |
| `ORBTRACE_BOARD_IP` | `192.168.1.50` | Static IPv4 address of the ZUBoard's A53 Orbtrace service |

## Boot paths

| Path | What | Requirements |
|---|---|---|
| **R5 via JTAG** | `openocd -f scripts/openocd/aes_zub.cfg -f scripts/openocd/load_r5.tcl` | JTAG cable, board switches OFF (=JTAG mode) |
| **A53 via JTAG** | `scripts/xsct/jtag_flash.sh <elf>` | Vitis 2023.2 (for xsct) |
| **R5 via SD/QSPI (no FSBL)** | `scripts/flash/build_boot_a53.sh` — A53 loader shim copies R5 image into TCM and releases R5-0 | Nothing extra; entirely in-tree |
| **R5 via SD/QSPI (with FSBL)** | `bootgen -image scripts/flash/boot_r5.bif -arch zynqmp -o BOOT.BIN -w on` | Requires user-supplied `scripts/flash/build/fsbl_r5.elf` built in Vitis |

The **A53 loader shim** is the preferred R5 boot path because it is
self-contained — no Vitis toolchain required.

## Regenerating board artifacts

`board/zub_1cg/design_1_wrapper.bit` and `psu_init.tcl` are opaque blobs
built in Xilinx tools:

1. Open the Vivado project at `../vivado_workspace/zub_hello_world_ethernet/`.
2. Generate bitstream → export hardware handoff (`.xsa`).
3. In Vitis, create a platform from the `.xsa`; extract `psu_init.tcl`
   and copy both files into `board/zub_1cg/`.
4. Update [`board/zub_1cg/artifacts.json`](board/zub_1cg/artifacts.json) with
   the source handoff and SHA-256 values in the same commit.

See `CLAUDE.md` for hardware register offsets and the R5 boot sequence.

## Related documentation

- [`CLAUDE.md`](CLAUDE.md) — hardware notes: R5 boot sequence, XPPU rules,
  UART setup, OpenOCD scripts.
- [`BRINGUP_LOG.md`](BRINGUP_LOG.md) — chronological R5 bring-up log,
  every attempt and fix recorded.
- [`tools/zub_ctl/README.md`](tools/zub_ctl/README.md) — CLI reference.
- [`tools/docs/README.md`](tools/docs/README.md) — convert the PDFs in `docs/`
  into agent-ready Markdown and categorized table indexes.

## Known issue: on-board UART routing (2026-07-31)

Current status: everything builds; the R5 JTAG boot sequence runs to
completion; **PS UART TX doesn't reach `/dev/ttyUSB1` on this board+bit
combo**. Bytes drain the TX FIFO (TX_EMPTY sets) but never appear at the
FTDI channel-1 side.

What was tried:

- Original `scripts/openocd/load_r5.tcl` (UART1, minimal MIO writes).
- Direct-poke of UART0 (0xFF000000) with Vitis-derived pinmux + clock.
- Full-fat Vitis `psu_init.tcl` sourced from OpenOCD via
  [`scripts/openocd/xsct_shim.tcl`](scripts/openocd/xsct_shim.tcl) — this
  works and correctly programs MIO 10/11 for UART0 per Vitis intent, but
  the PS UART still writes to a pin the FTDI cable isn't wired to.
- xsct-based `psu_init` (which would use the proven Vitis path) — blocked
  because `hw_server` can't enumerate the FT2232H JTAG channel without
  the Digilent Adept USB driver installed.

Unblocking paths, from least to most work:

1. **Install Digilent Adept** (`digilent-adept-runtime` +
   `digilent-adept-utilities` in AUR). This restores xsct's `hw_server`
   JTAG enumeration and the fully-proven Vitis flow becomes usable via
   `zub_ctl watch-r5 --pre-xsct …` (already wired).
2. **Physical probe of MIO 10/11 vs FTDI TXD/RXD** to confirm/deny the
   routing hypothesis.
3. **Regenerate the Vivado bitstream** with explicit UART routing to the
   MIO pins this board actually wires the FTDI to.

The build/test infra is complete and ready to consume the fix as soon as
the physical routing is validated. See
[`BRINGUP_LOG.md`](BRINGUP_LOG.md) for the original working bring-up
sequence; that log was captured after a Vitis session had preconfigured
the PS, which may explain why the pared-down OpenOCD path worked then
but does not from cold-boot now.
