# Vivado 2023.2 design

`build.tcl` is the sole hardware-generation entry point. It creates an
`xczu1cg-sbva484-1-e` project, block design, wrapper, bitstream, XSA, reports,
and `psu_init.tcl` beneath `bazel-out/orbtrace-vivado`.
Set `XSCT` to the Vitis 2023.2 `xsct` executable if it is not on `PATH`.

The design also instantiates an on-chip Cortex-M3 (Arm DesignStart FPGA
edition) as Orbtrace's real CoreSight trace source and SW-DP debug target
(`create_bd.tcl`'s `m3_core`). That IP is licensed separately by Arm — create
an Arm account, accept the DesignStart FPGA EULA, and download the Xilinx
edition — and, like the Avnet board files below, is not committed to this
repo. Unpack it locally and set `ARM_DESIGNSTART_IP_ROOT` to that directory;
`build.tcl` adds it to `ip_repo_paths` and runs `update_ip_catalog` before
`create_bd.tcl` runs. The exact VLNV and pin names used in `create_bd.tcl`
are best-effort and marked `CONFIRM` — check them against the real IP-XACT
(`get_ipdefs`, `get_bd_pins -of [get_bd_cells m3_core]`) once it's unpacked.

The design uses `M_AXI_HPM0_FPD` for control and `S_AXI_HP0_FPD` for AXI DMA
payload/descriptor traffic. Vendor primitives are confined to the block design
and `orbtrace_ddr4_capture.sv`; framing and loss behavior remain explicit RTL.

The script rejects negative setup slack. CDC and utilization reports are
always emitted, but board release also requires review of those reports and the
60-second hardware throughput acceptance described in [`../TESTING.md`](../TESTING.md).
The generated bitstream remains a build output rather than a source-release
artifact; the hardware-tested output hash is recorded in the
[final technical report](../../../documentation/ORBTRACE_400MBPS_TECHNICAL_REPORT_2026-08-09.md).
