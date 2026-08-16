# M3 / Vivado hybrid-build handoff — 2026-08-16

## Current state

The Cortex-M3 integration needs a hybrid Xilinx flow:

- The supplied Arm DesignStart M3 FPGA IP release is encrypted for Vivado
  2019.1.  Vivado 2023.2 fails on its encrypted RTL with `[Synth 8-5809]`.
- Vivado 2019.1.3 can decrypt and synthesize the configured M3 core, but it
  does **not** know the board's `xczu1cg-sbva484-1-e` part, so it cannot build
  the full board design.
- Vivado 2019.1.3 therefore synthesizes only the M3 out of context and
  exports EDIF; Vivado 2023.2 builds the ZU1CG board around that netlist.

The 2019 M3 OOC build completed successfully.  Its outputs are:

```text
bazel-out/m3-ooc-2019/m3_core.edf       # symlink to m3_core_2019.edf
bazel-out/m3-ooc-2019/m3_core_2019.dcp  # 4.4 MiB, for inspection only
```

The latest full hybrid build is in
`bazel-out/orbtrace-vivado-hybrid-blackbox`.  It passed all IP/OOC syntheses
**and top-level synthesis**, then entered `impl_1` (`vrs_config_2.xml`).  It
was explicitly stopped for this session handoff, so resume by rerunning the
hybrid command below.  No finished bitstream was verified; require
`write_bitstream Complete!` before treating an output as usable.

## Installed toolchains

Both toolchains are installed below `/home/v/opt/vitis`:

```text
Vivado/2019.1     -> Vivado v2019.1.3
SDK/2019.1        -> xsct 2019.1.3
Vivado/2023.2     -> board synthesis/implementation
Vitis/2023.2      -> normal 2023 xsct
```

The repository flake exposes the 2019 tools as `.#vivado_2019` and the
standalone user flake is `/home/v/opt/vitis-2019-flake`.  Both expect:

```bash
export XILINX_ROOT=/home/v/opt/vitis
```

## Inputs and local repositories

```text
Arm M3 source repo: /home/v/projects/arm_designstart_m3
Arm IP root:        /home/v/projects/arm_designstart_m3/vivado/Arm_ipi_repository
Avnet BDF repo:     /home/v/projects/avnet_bdf
Arm archive:         /home/v/Downloads/AT426-r0p1-00rel0-1.tar.gz
```

The expected Arm VLNV is `Arm.com:CortexM:CORTEXM3_AXI:1.1`.

## Relevant repository changes

`applications/orbtrace/vivado/build_m3_ooc_2019.tcl`

- New 2019.1 OOC script.
- Targets `xczu3eg-sbva484-1-e`, a 2019-supported Zynq UltraScale+ part.
- Generates the real configured M3 and exports `m3_core.edf`.

`applications/orbtrace/vivado/build.tcl`

- Accepts `ORBTRACE_VIVADO_OUTPUT_DIR`.
- Uses `ARM_DESIGNSTART_IP_ROOT` for the real M3 IP.
- Optional `M3_OOC_EDIF` selects the hybrid route.
- Disables only the 2023-generated M3 XCI/wrapper, so 2023 no longer tries to
  decrypt Arm's 2019-encrypted RTL.
- Generates an adapter retaining the BD cell name
  `zub_orbtrace_m3_core_0` but instantiating the EDIF top `m3_core`.
- Because WIC is disabled, the 2019 EDIF prunes its internal-only
  `WAKEUP`, `WICENACK`, and `WICENREQ` ports.  The adapter removes those
  connections before use.
- Generates `(* black_box *) module m3_core (...)` from the adapter's actual
  port declaration.  This is important: `read_edif` introduces the netlist at
  link time, whereas RTL elaboration needs a module declaration earlier.
- Installs a `STEPS.SYNTH_DESIGN.TCL.PRE` hook to read, in order: the black-box
  declaration, the EDIF, and the adapter.  `default_lib` is set to `work` to
  keep the Verilog and EDIF in the same library.

`applications/orbtrace/M3_TRACE_VERIFICATION_PLAN.md`

- Status updated from the old 2023 encrypted-RTL block to the hybrid path.

## Commands

Build/rebuild the 2019 core netlist:

```bash
env XILINX_ROOT=/home/v/opt/vitis \
  ARM_DESIGNSTART_IP_ROOT=/home/v/projects/arm_designstart_m3/vivado/Arm_ipi_repository \
  M3_OOC_OUTPUT_DIR="$PWD/bazel-out/m3-ooc-2019" \
  nix develop .#vivado_2019 -c vivado -mode batch \
    -source applications/orbtrace/vivado/build_m3_ooc_2019.tcl
```

Run the hybrid full board build:

```bash
env XILINX_ROOT=/home/v/opt/vitis \
  ARM_DESIGNSTART_IP_ROOT=/home/v/projects/arm_designstart_m3/vivado/Arm_ipi_repository \
  AVNET_BDF_ROOT=/home/v/projects/avnet_bdf \
  M3_OOC_EDIF="$PWD/bazel-out/m3-ooc-2019/m3_core.edf" \
  ORBTRACE_VIVADO_OUTPUT_DIR="$PWD/bazel-out/orbtrace-vivado-hybrid" \
  nix develop -c vivado -mode batch -source applications/orbtrace/vivado/build.tcl
```

Useful status checks:

```bash
tail -100 vivado.log
rg -n 'ERROR:|synth_design completed successfully|write_bitstream Complete' \
  bazel-out/orbtrace-vivado-hybrid/zub_orbtrace.runs/*/runme.log
```

## What was proven before this run

- 2019.1.3 OOC synthesis completed with zero errors/critical warnings.
- Vivado 2023.2 accepted the EDIF as a ZU1-family netlist in a direct
  `read_edif` / `link_design` experiment.
- The first hybrid retries proved that all normal project IP syntheses work;
  their failure was specifically at the M3 adapter boundary.
- The final missing-module failure was resolved by the generated black-box
  declaration.  The active run has reached implementation, which proves its
  top-level synthesis passed.

## Next steps after implementation

1. Confirm `write_bitstream Complete!` and that these exist:
   `zub_orbtrace.bit`, `zub_orbtrace.xsa`, timing/CDC/methodology reports.
2. Inspect timing and CDC; do not flash if `build.tcl` rejects negative slack
   or critical CDC/methodology output.
3. Build the A53 firmware and flash with the fresh hybrid bitstream using the
   established Orbtrace JTAG workflow in `AGENTS.md`.
4. Complete Phase C/D hardware verification: preload the M3 BRAM, release the
   core, capture its trace, then exercise the real JTAG bridge.

## Important cautions

- `sdk/boards/zub_1cg/design_1_wrapper.bit` is the old Ethernet-loopback
  design, not the Orbtrace/M3 image; do not use it for this plan.
- The output under `bazel-out` is generated/cached and should not be committed.
- The repository was already dirty before this work.  Review only the files
  above when preparing a commit; do not accidentally stage unrelated changes.
- A prior config-generator invocation created a runaway 2.4 GiB log.  Its
  process was stopped and the file was moved recoverably to:
  `/home/v/.local/share/Trash/files/xinstall_1786827629389-configgen-stdin-loop.log`.
