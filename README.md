# ZUBoard 1CG SDK and real-time trace-capture prototype

`zub_1cg` is a versioned, Nix/Bazel-based embedded SDK for the Avnet
AES-ZUB-1CG development board (`xczu1cg-sbva484-1-e`). It supports bare-metal
and ThreadX software on the Zynq UltraScale+ Cortex-A53 APU and Cortex-R5F RPU,
contains repeatable JTAG/UART test automation, and hosts a Rust/SystemVerilog
Orbtrace-compatible trace-capture prototype.

This is intentionally board-specific. The memory maps, linker scripts, PS
initialization, bitstreams, UART routing, Ethernet PHY setup, and boot scripts
are tied to the ZUBoard 1CG rather than presented as a generic ZynqMP HAL.

## Project status

| Area | Current evidence |
|---|---|
| A53 and R5 cross-builds | Bazel transition-aware firmware builds pass with pinned Nix cross compilers |
| Host/static verification | 18 unit, protocol, artifact-integrity, ELF-invariant, and memory-budget tests pass |
| Board execution | Six deployable A53/R5 integration tests passed on physical hardware on 2026-08-07 |
| R5 postmortem capture | Undefined-instruction injection survives reset with a CRC-checked exception record and host decoder |
| Orbtrace host model | Rust protocol/model tests pass; control, DAP, capture, replay, and statistics paths are covered |
| Orbtrace programmable logic | Vivado 2023.2 synthesis and implementation completed with positive setup/hold slack in the recorded 2026-08-08 build |
| 400 Mbit/s Orbflow goal | Encoded as an automated acceptance threshold, but **not yet demonstrated end-to-end on hardware** |

The last distinction matters: `tests/orbtrace_throughput_test.sh` rejects
payload rates below 400,000,000 bit/s and any increase in loss counters, but
the current board bring-up has not completed that acceptance run. The CV-facing
description should call this a target or prototype capability until a captured
hardware run proves it. See
[`ORBTRACE_TEST_REPORT_2026-08-08.md`](ORBTRACE_TEST_REPORT_2026-08-08.md) and
[`ON_TARGET_TEST_REPORT_2026-08-07.md`](ON_TARGET_TEST_REPORT_2026-08-07.md)
for dated evidence and open issues.

The repository-wide presubmit currently reaches and passes all 18 Bazel tests
and both firmware builds, but its formatting gates report pre-existing
`buildifier`/`rustfmt` differences. Run the presubmit before publishing and
clear those formatting findings rather than treating a test-only pass as a
fully green release.

## Hardware and execution domains

| Domain | Core/tool | Role |
|---|---|---|
| APU | Arm Cortex-A53, AArch64 | ThreadX applications, NetX Duo networking, trace-control service, high-bandwidth data movement |
| RPU | Arm Cortex-R5F, Armv7-R | Deterministic firmware, BSP validation, fault injection and persistent postmortem capture |
| PL | SystemVerilog on ZU1CG fabric | Trace/SWO decoding, framing, CDC, register interfaces, DMA-facing streaming and test sources |
| Host | Rust on x86_64 Linux | Serial/JTAG orchestration, ELF checks, crash decoding, protocol model and capture client |

The Nix shell pins Bazel 8, both GNU Embedded cross compilers, Rust, OpenOCD,
Bootgen, buildifier, Poppler, and the host libraries needed by the Rust tools.
Vivado/Vitis/XSCT remain proprietary external dependencies for PL generation
and A53 board loading.

## Technical highlights

### Transition-owned cross compilation

The public `a53_firmware` and `r5_firmware` Bazel macros apply their own
platform transitions, so a firmware target selects the correct architecture,
compiler, linker script, startup objects, and board libraries without asking a
consumer to pass `--config=apu` or `--config=rpu` manually.

- A53 firmware uses `aarch64-none-elf`, an AArch64 linker layout in DDR, and
  optional ThreadX/Xilinx BSP dependencies.
- R5 firmware uses `arm-none-eabi`, an Armv7-R linker layout in OCM, mode-aware
  exception startup, and optional ThreadX support.
- A manual `aarch64-unknown-none` Rust toolchain uses the pinned Rust 1.86
  bare-metal standard-library component for the Rust portions of the A53
  trace-control service.

Environment-backed repository rules capture the exact compiler binaries from
the Nix store. Third-party ThreadX, FileX, NetX Duo, and Xilinx BSP inputs are
consumed through Bazel rather than through an IDE-generated workspace.

### Reusable firmware and boot-image rules

The supported SDK surface lives under `//sdk`:

- `a53_firmware` and `r5_firmware` produce target ELFs with the board's startup
  and link contracts;
- `zub_boot_image` wraps the Xilinx Bootgen flow;
- `firmware_elf_test` checks architecture, entry point, load range, and required
  symbols; and
- `firmware_size_test` enforces `.text` and `.bss` budgets.

Committed board artifacts have a machine-readable SHA-256 manifest. The
artifact integrity test catches accidental mixing of a bitstream and
`psu_init.tcl` from different hardware handoffs. Compatibility expectations
and measured tool versions are in
[`sdk/boards/zub_1cg/COMPATIBILITY.md`](sdk/boards/zub_1cg/COMPATIBILITY.md).

### R5 startup and persistent fault capture

The R5 BSP supplies exception-vector assembly, per-mode stack setup, UART and
timer support, and linker sections for persistent state. Fault handlers save a
versioned `pm_record_t` containing the exception type, CPSR/SPSR, general
registers, fault-status/address registers, PC/LR, sequence number, and CRC.
The record lives in `.noinit`, allowing the next boot—or a debugger—to recover
it without trusting a damaged runtime heap.

`//applications/rpu/fault_test` deliberately executes an undefined instruction,
boots through the same JTAG path, validates the record, and emits a test-pass
sentinel. `//tooling/pm_decode` provides the host-side decoder.

### Hardware-aware test orchestration

`zub_ctl` is a Bazel-built Rust utility that opens UART before releasing the
target, matches ordered regular expressions, rejects configured failure
patterns, and runs OpenOCD or XSCT concurrently. Starting capture first avoids
losing the earliest boot output—the exact output most useful when startup code
fails.

On-board Bazel tests are tagged `manual`, `exclusive`, `local`, and
`requires-hardware`. They are serialized to prevent multiple tests from
contending for the FT2232H JTAG channel or `/dev/ttyUSB1`, and they disable the
Bazel sandbox only for the action that must access physical devices.

### Orbtrace-compatible data path

[`applications/orbtrace/`](applications/orbtrace/) divides the design into
independently testable layers:

- `model/`: Rust wire model and host CLI for control, capture, deterministic
  replay, statistics, CMSIS-DAP, and OpenOCD remote-bitbang;
- `rtl/`: SystemVerilog asynchronous FIFOs, TPIU demultiplexing, SWO NRZ and
  Manchester receive paths, channel packetization, COBS/Orbflow framing,
  checksums, counters, AXI4-Lite registers, and DDR/DMA capture paths;
- `firmware/common/`: allocation-free framing and DMA-ring logic;
- `firmware/a53/` and `firmware/a53_app/`: Rust protocol state machine behind
  a C FFI, ThreadX/NetX Duo TCP services, and the GEM2 Ethernet path;
- `firmware/vexriscv/`: the RV32IMAC trace workload model; and
- `vivado/`: batch-mode project creation, timing/methodology gates, bitstream,
  XSA, and PS-init export.

The current network contract uses TCP 3401 for versioned control, TCP 3402 for
Orbflow payloads, and TCP 3240 for length-prefixed CMSIS-DAP traffic. The PL's
CMSIS-DAP mailbox uses explicit last-byte/backpressure state and supports
deterministic WAIT, FAULT, and parity-error injection.

The 400 Mbit/s test captures three billion payload bytes over 60 seconds and
requires `dropped_bytes`, `sync_loss`, and `dma_faults` to remain unchanged.
That is the acceptance contract, not a current benchmark result.

## Repository layout

```text
.
├── sdk/
│   ├── boards/zub_1cg/       # bitstream, PS init, hashes and compatibility data
│   ├── bsp/{apu,rpu}/        # startup, link, timer, UART and postmortem support
│   ├── rules/                # firmware, boot-image and verification macros
│   ├── platforms/            # host, Cortex-A53 and Cortex-R5F constraints
│   ├── rtos/                 # RTOS-facing SDK targets
│   └── toolchains/           # AArch64, Arm and bare-metal Rust toolchains
├── applications/
│   ├── apu/                  # hello-world, blink and GEM2 loopback examples
│   ├── rpu/                  # BSP, ThreadX and fault-capture examples
│   └── orbtrace/             # model, firmware, RTL and Vivado project
├── tooling/
│   ├── zub_ctl/              # serial/JTAG test orchestrator
│   ├── elf_check/            # ELF architecture/load/symbol checker
│   ├── pm_decode/            # R5 postmortem decoder
│   ├── openocd/ and xsct/    # board load/debug scripts
│   └── flash/                # boot-image helpers
├── tests/                    # host/static and physical-board integration tests
├── third_party/              # Bazel integration for RTOS and vendor libraries
└── internal/                 # presubmit, lab diagnostics and private-doc tooling
```

## Prerequisites

For host builds and tests:

- x86_64 Linux;
- Nix with flakes enabled; and
- enough disk space for the pinned cross compilers and Bazel cache.

For physical-board tests:

- Avnet AES-ZUB-1CG-ED-G in JTAG boot mode (all boot-mode DIP switches off);
- the board's FT2232H USB connection (`ttyUSB0` JTAG, `ttyUSB1` UART on the
  tested host), with the user in the appropriate `dialout`/USB groups;
- Vitis/XSCT 2023.2 for A53 loading; and
- an Ethernet interface on `192.168.1.0/24` for the Orbtrace network tests.

Vivado 2023.2 with Zynq UltraScale+ device support is needed only when
regenerating PL artifacts. Prebuilt board artifacts are committed for the
normal firmware/test workflow.

## Quick start

```sh
git clone https://github.com/VincenzoCalabretta/zub_1cg.git
cd zub_1cg
nix develop

# Build all bundled A53 and R5 applications.
bazel build //applications/apu/...
bazel build //applications/rpu/...

# Run host-safe tests, ELF invariants, artifact hashes and size budgets.
bazel test --config=host \
  //applications/orbtrace/model:orbtrace_model_test \
  //applications/orbtrace/model:register_schema_test \
  //tooling/... \
  //sdk/boards/zub_1cg:artifact_integrity_test \
  //tests:dependency_boundaries_test \
  //tests:apu_blink_elf_test \
  //tests:apu_hello_world_elf_test \
  //tests:apu_eth_loopback_elf_test \
  //tests:rpu_hello_world_elf_test \
  //tests:rpu_bsp_test_elf_test \
  //tests:rpu_fault_test_elf_test \
  //tests:rpu_hello_world_size_test \
  //tests:rpu_bsp_test_size_test \
  //tests:rpu_fault_test_size_test
```

The authoritative repository gate also runs formatting and both architecture
builds:

```sh
bash internal/presubmit/presubmit.sh
```

If a wildcard target changes as packages are added, use `bazel query //...`
or inspect `internal/presubmit/presubmit.sh` for the release gate's exact
current target set.

## Building individual firmware

Firmware targets own their cross-platform transition:

```sh
bazel build //applications/apu/hello_world:hello_world
bazel build //applications/apu/blink:blink
bazel build //applications/apu/eth_loopback:eth_loopback

bazel build //applications/rpu/hello_world:hello_world
bazel build //applications/rpu/bsp_test:bsp_test
bazel build //applications/rpu/fault_test:fault_test
```

Bazel places the linked ELFs below `bazel-bin/applications/...`. Prefer the
test and flash targets over hard-coding output paths because transition-owned
artifacts can have configuration-specific paths.

## Running on the board

Enter `nix develop`, connect USB/UART, put the board in JTAG mode, and provide
XSCT when running an A53 test:

```sh
export XSCT=/path/to/Vitis/2023.2/bin/xsct

bazel test --config=host --config=onboard //tests:apu_hello_world_test
bazel test --config=host --config=onboard //tests:apu_blink_test
bazel test --config=host --config=onboard //tests:apu_eth_loopback_test

bazel test --config=host --config=onboard //tests:rpu_hello_world_test
bazel test --config=host --config=onboard //tests:rpu_bsp_test
bazel test --config=host --config=onboard //tests:rpu_fault_test
```

Optional test environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `ZUB_TTY` | `/dev/ttyUSB1` | UART device |
| `ZUB_BAUD` | `115200` | UART baud rate |
| `ZUB_TIMEOUT` | test-specific | Pattern-match timeout |
| `XSCT` | searched from environment | Vitis command used by A53 tests |
| `ORBTRACE_BOARD_IP` | `192.168.1.50` | Trace service address |

The R5 path uses OpenOCD to remap OCM, hold the RPU in reset while changing
split/lockstep state, initialize clocks and UART, load the ELF at AXI address
`0xFFFF0000`, verify the first instruction, and release R5-0. A full power
cycle may be required after an invalid debug access leaves the ZynqMP access
protection state latched.

## Orbtrace verification and use

Run the host/model and firmware logic tests without hardware:

```sh
bazel test --config=host \
  //applications/orbtrace/model:orbtrace_model_test \
  //applications/orbtrace/model:register_schema_test \
  //applications/orbtrace/firmware/common:firmware_common_test \
  //applications/orbtrace/firmware/a53:control_firmware_test \
  //applications/orbtrace/firmware/vexriscv:trace_workload_test
```

The recorded Vivado build is driven in batch mode from
`applications/orbtrace/vivado/build.tcl`. See
[`applications/orbtrace/vivado/README.md`](applications/orbtrace/vivado/README.md)
for the tool invocation and artifact paths. The build script gates negative
setup/hold slack and critical methodology/CDC findings before publishing a
bitstream and XSA.

After deploying a matching trace PL design and A53 service, the host model can
query state and capture payloads. Discover its current CLI directly from the
Bazel-built binary:

```sh
bazel run --config=host //applications/orbtrace/model:orbtrace -- --help
```

The end-to-end acceptance test is:

```sh
bazel test --config=host --config=onboard //tests:orbtrace_a53_app_test
bazel test --config=host --config=onboard //tests:orbtrace_throughput_test
```

Do not interpret a host/model test pass as the 400 Mbit/s hardware result; only
the second command, with its byte-count and loss-counter checks satisfied,
establishes that claim.

## Consuming the SDK from another Bazel module

Until the module is published in a registry, pin a Git revision explicitly:

```starlark
bazel_dep(name = "zub_1cg", version = "0.1.0")

git_override(
    module_name = "zub_1cg",
    remote = "https://github.com/VincenzoCalabretta/zub_1cg.git",
    commit = "<FULL_COMMIT_SHA>",
)
```

Use the firmware macros from the supported SDK surface:

```starlark
load(
    "@zub_1cg//sdk/rules:defs.bzl",
    "a53_firmware",
    "r5_firmware",
    "zub_boot_image",
)

a53_firmware(
    name = "a53_app",
    srcs = ["a53_app.c"],
)

r5_firmware(
    name = "r5_app",
    srcs = ["r5_app.c"],
    deps = ["@zub_1cg//sdk/rtos:threadx_r5"],
)

zub_boot_image(
    name = "boot",
    r5_firmware = ":r5_app",
)
```

The standalone fixture under `tests/consumer/` demonstrates external-module
analysis with a `local_path_override` during SDK development.

The flake also exports a composable development shell:

```nix
inputs.zub_1cg.url = "github:VincenzoCalabretta/zub_1cg/<revision>";

devShells.x86_64-linux.default = zub_1cg.lib.mkDevShell {
  system = "x86_64-linux";
  extraPackages = [ ];
  extraShellHook = "";
};
```

Pin the same revision in Nix and Bazel so compiler discovery, board artifacts,
rules, and headers cannot drift independently.

## Regenerating board artifacts

The committed SDK bitstream and PS-init script are opaque vendor-tool outputs.
Replace them only as a pair from one Vivado/Vitis hardware handoff, then update
the manifest and run:

```sh
bazel test --config=host //sdk/boards/zub_1cg:artifact_integrity_test
bash internal/presubmit/presubmit.sh
```

For the Orbtrace design, use its batch scripts rather than the generic board
artifact procedure. Mixing the shared Ethernet bitstream, Orbtrace bitstream,
or a `psu_init.tcl` from another handoff can produce a firmware image that
loads successfully but drives the wrong clocks, MIO, or address map.

## Troubleshooting

### No early serial output

Confirm `/dev/ttyUSB1`, 115200 baud, JTAG boot mode, and that no terminal
program already owns the device. Use `zub_ctl`/the Bazel test so UART is opened
before the core is released.

### R5 register writes or OCM reads silently fail

An incorrect access-port selection can latch a DAP sticky error, and protected
RPU registers become unwritable after release. Power-cycle the board, do not
add a guessed `cortex_r4` target, and use `tooling/openocd/scan_aps.tcl` before
changing the OpenOCD target definition.

### A53 tests cannot find XSCT

Export `XSCT` to the real Vitis 2023.2 binary or wrapper. Nix supplies the open
tooling but cannot redistribute Xilinx's proprietary installation.

### Orbtrace service is unreachable

Verify the host address is in `192.168.1.0/24`, use TCP 3401 rather than ICMP
ping as the health probe, confirm the A53 service and matching PL bitstream are
loaded, and consult the latest follow-up section in the Orbtrace test report.
The Ethernet/GEM2 bring-up has had descriptor-ring and NetX Duo integration
issues; a TCP preflight failure is not a throughput measurement.

## License and third-party code

Repository-authored code is available under the [MIT License](LICENSE).
Vendored and fetched RTOS, Xilinx, Rust, and protocol sources retain their own
licenses; see [NOTICE](NOTICE) and the corresponding `third_party` metadata.
