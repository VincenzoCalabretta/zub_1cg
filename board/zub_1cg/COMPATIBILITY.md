# Board artifact compatibility matrix

This document defines the compatibility requirements for artifacts committed to
`board/zub_1cg/`.  Replace any artifact only after verifying it is compatible
with all items in this matrix.

## Committed artifacts

| File | SHA-256 | Toolchain | Notes |
|------|---------|-----------|-------|
| `design_1_wrapper.bit` | `bbf9c42707fe2162e9c9482e3697f27de938c9d260224d4452b0cf121906e1da` | Vivado 2023.2 | Ethernet-loopback design |
| `psu_init.tcl` | `ee38a3b846798523c7278b0219dd20befdb868e4da40f5bba241f5772f56d2dc` | Vitis 2023.2 | PS init (clocks, MIO, DDR) |

`artifacts.json` is the machine-readable version of this table and is verified
by `//board/zub_1cg:artifact_integrity_test` on every presubmit run.

## Firmware ↔ artifact compatibility

Both artifacts must be regenerated together from the same Vivado project and
XSA; mixing different generations is not supported.

| Firmware | Requires bitstream | Requires psu_init | Notes |
|----------|-------------------|-------------------|-------|
| A53 apps (APU) | Yes | Yes (loaded by xsct) | XSCT loads both before the ELF |
| R5 apps (RPU) | No | Yes (loaded by OpenOCD load_r5.tcl) | Bitstream not needed for JTAG-only R5 boot |

## Toolchain compatibility

| Component | Minimum | Tested | Notes |
|-----------|---------|--------|-------|
| arm-none-eabi-gcc | 12.x | 15.2.Rel1 | From Nix devShell |
| aarch64-none-elf-gcc | 12.x | 15.3.0 | From Nix devShell |
| Bazel | 7.x | 8.7.0 | See .bazelversion |
| Vivado | 2023.2 | 2023.2 | For bitstream regeneration only |
| Vitis | 2023.2 | 2023.2 | For psu_init regeneration only |

## Flash/RAM budgets

Section sizes are checked by `//tests:*_size_test` targets during presubmit.
Budgets are intentionally generous; tighten them as the firmware matures.

| Firmware | .text budget | .bss budget | Notes |
|----------|-------------|-------------|-------|
| rpu/hello_world | 24 KB | 12 KB | ThreadX + drivers |
| rpu/bsp_test | 4 KB | 1 KB | Bare-metal, UART only |
| rpu/fault_test | 8 KB | 1 KB | Bare-metal + postmortem |

The OCM is 256 KB at 0xFFFF0000.  Even the largest current firmware
(hello_world at ~17 KB) uses less than 7% of available OCM.
