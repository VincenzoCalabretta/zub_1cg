# Vivado 2023.2 design

`build.tcl` is the sole hardware-generation entry point. It creates an
`xczu1cg-sbva484-1-e` project, block design, wrapper, bitstream, XSA, reports,
and `psu_init.tcl` beneath `bazel-out/orbtrace-vivado`.
Set `XSCT` to the Vitis 2023.2 `xsct` executable if it is not on `PATH`.

The design uses `M_AXI_HPM0_FPD` for control and `S_AXI_HP0_FPD` for AXI DMA
payload/descriptor traffic. Vendor primitives are confined to the block design
and `orbtrace_ddr4_capture.sv`; framing and loss behavior remain explicit RTL.

The script rejects negative setup slack. CDC and utilization reports are
always emitted, but board release also requires review of those reports and the
60-second hardware throughput target described in `orbtrace/TESTING.md`.
