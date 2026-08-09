# On-target test report — 2026-08-07

## Executive result

The connected AES-ZUB-1CG board passed every firmware-on-target integration
test currently deployable through JTAG: **6/6 passed**.  The separate Orbtrace
throughput acceptance test was executed but did not start because the board's
Orbtrace A53 service was unavailable at its configured address.  Its result is
therefore **blocked by deployment/network preconditions**, not a measured
throughput failure.

The nine static firmware ELF and memory-budget checks also passed (**9/9**).

## Snapshot and provenance

| Item | Value |
|---|---|
| Repository | `zub_1cg` |
| Commit tested | `76d8951 pre-nixos snapshot` |
| Date / timezone | 2026-08-07, Europe/Rome |
| Working tree | Dirty before testing; changes were preserved |
| Modified pre-existing files | `README.md`, `flake.lock`, `flake.nix`, `tests/BUILD.bazel`, `tests/orbtrace_throughput_test.sh`, `tests/rules.bzl` |
| Board USB device | FTDI FT2232H, USB ID `0403:6010` |
| Serial console | `/dev/ttyUSB1`, mode `crw-rw-rw-`, 115200 baud |
| Host network | `wlo1` was `192.168.0.131/24` |
| Orbtrace endpoint | default `192.168.1.50`; ports 3240, 3401, and 3402 were closed |

The tests were run from the repository development environment with a local
FHS wrapper around the separately installed proprietary XSCT tools.

| Tool | Tested version |
|---|---|
| Bazel | 8.7.0 |
| OpenOCD | 0.12.0 |
| Rust | 1.97.0 |
| Arm GCC | 15.2.Rel1 |
| AArch64 GCC | 15.3.0 |
| Vitis / XSCT | 2023.2 (`hw_server` banner) |

## On-target execution

All board tests were invoked with `bazel test --config=host --config=onboard`.
The `onboard` configuration forces local execution, allowing access to the
serial and JTAG devices.  Test scripts open the UART before boot so early
firmware output is captured.

| Target | Result | Evidence captured on this run |
|---|---:|---|
| `//tests:rpu_bsp_test` | PASS (6.3 s) | OpenOCD identified both ZynqMP JTAG TAPs, initialized PS, loaded OCM, then the R5 emitted `[TEST PASS] bsp_uart`. |
| `//tests:rpu_fault_test` | PASS (6.3 s) | R5 injected an undefined instruction; its postmortem recorded `exc=undef`, `pc=0xffff0064`, `lr=0xffff0068`, and `crc=OK`, then emitted `[TEST PASS] fault_capture`. |
| `//tests:rpu_hello_world_test` | PASS (6.5 s) | R5 ThreadX image booted from OCM and emitted `[TEST PASS] hello_world`. |
| `//tests:apu_blink_test` | PASS (11.7 s) | XSCT/hw_server connected, programmed the PL bitstream, ran `psu_init`, loaded the A53 ELF, then saw `UART OK` and `LED 0x0  readback 0x0`. |
| `//tests:apu_hello_world_test` | PASS (11.5 s) | A53 ThreadX image emitted `ThreadX RGB LED (AArch64 / A53)` and `led thread: running`. |
| `//tests:apu_eth_loopback_test` | PASS (11.6 s) | GEM2 internal loopback emitted `frame sent, polling`, `RX len=60`, and `PASS: loopback OK`. |
| `//tests:orbtrace_throughput_test` | BLOCKED (3.0 s) | Preflight failed at `192.168.1.50:3401`: the Orbtrace control service was unavailable. No capture or throughput measurement was attempted. |

Each R5 boot also verified `RST_LPD_TOP` transition from both R5 cores in
reset to R5-0 released, and verified the first OCM instruction at
`0xFFFF0000` before release.  This is a useful regression signal for the R5
JTAG path.

## Non-hardware firmware gates

The following command passed all nine checks:

```sh
nix develop -c bazel test --config=host \
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

The ELF gates confirm the expected target architecture, entry point, loadable
memory range, and required startup symbols.  The R5 size gates confirm the
configured text and BSS budgets.

## Important observations and follow-up

1. **The A53 XSCT route is working now.** `hw_server` connected and all A53
   board tests passed.  This supersedes the stale warning in `README.md` that
   describes XSCT/JTAG enumeration as blocked, unless it is intentionally
   retained as historical context.
2. **Orbtrace needs a deployed image and matching network.** Configure a host
   interface on `192.168.1.0/24`, deploy the Orbtrace PL bitstream plus A53
   service, verify TCP 3401/3402 (and 3240 for debug), then rerun:

   ```sh
   nix develop -c bazel test --config=host --config=onboard \
     //tests:orbtrace_throughput_test
   ```

   The acceptance threshold is 3,000,000,000 payload bytes in 60 seconds
   (at least 400 Mbit/s), with unchanged reported loss counters.
3. **OpenOCD's PSU script reports three `mask_poll timeout` warnings** at
   `0xff5e0040` and `0xfd1a0044` during every R5 test.  They did not prevent
   PS initialization or any R5 assertion from passing.  Treat them as a
   follow-up diagnostic item rather than a current test failure.
4. **`zub_ctl doctor` warned that no FT2232H was present under
   `/dev/serial/by-id`**, although `lsusb` identified the physical FT2232H and
   JTAG and UART access both worked.  The doctor check appears overly specific
   to a by-id naming convention; it should be made advisory or match the
   actual device topology.
5. **Repository-wide `bazel query //...` is not currently clean.** It fails
   when loading `third_party/threadx`, `third_party/filex`, and
   `third_party/netxduo` because their `common/src` glob directories are absent.
   The explicitly selected board and firmware test targets still analyze and
   run.  Restore/initialize those vendor source trees before treating a global
   query or wildcard test as a repository-wide gate.

## Reproduction checklist

1. Connect the board in JTAG mode and confirm `/dev/ttyUSB1` exists.
2. Enter `nix develop` and ensure `XSCT` names an executable Vitis 2023.2
   `xsct` binary for the A53 tests.
3. Run the six firmware targets listed above one at a time (they are tagged
   `exclusive` to avoid sharing UART/JTAG).
4. Deploy Orbtrace and configure the host subnet before running its throughput
   target.
5. Keep the serial console idle; residual output from the previously loaded
   image can appear before an XSCT reset, but only post-load ordered regex
   matches determine the test result.
