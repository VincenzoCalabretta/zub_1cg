# Verification and release gates

Host-verifiable targets:

```sh
bazel test --config=host //orbtrace/model:orbtrace_model_test
bazel test --config=host //orbtrace/model:register_schema_test
bazel test --config=host //orbtrace/firmware/common:firmware_common_test
bazel test --config=host //orbtrace/firmware/vexriscv:trace_workload_test
bazel test //orbtrace/firmware/a53:control_firmware_test
XILINX_VIVADO=/opt/Xilinx/Vivado/2023.2 \
  bazel test //orbtrace/rtl:rtl_unit_test --test_env=XILINX_VIVADO
```

The model tests are byte-exact. The XSim suite covers 1/2/4-bit DDR wire order,
NRZ, Manchester, and CMSIS-DAP Info/read/WAIT/FAULT/abort paths. An RTL release
additionally requires differential tests with randomized valid/ready stalls
and reset injection at every pipeline stage. Required cases are FIFO full/empty transitions, COBS
zero and 254-byte groups, checksum wrap, TPIU sync recovery/channel changes,
malformed SWO, and CMSIS-DAP OK/WAIT/FAULT/parity/abort behavior.

Firmware tests exercise arbitrary TCP length-prefix fragmentation, pipelined
control requests, unchanged CMSIS-DAP payload transport, AXI DMA descriptor
wrap/full/error behavior, bounded DMA reset, and 64-bit current/tail pointers.

Vivado acceptance uses `vivado -mode batch -source orbtrace/vivado/build.tcl`.
Negative setup or hold slack, critical CDC findings, and critical methodology
findings fail automatically. Release review must also reject resource-budget
regression.

Board tests are `manual`, `exclusive`, and `requires-hardware`. Acceptance is:

1. VexRiscv Rust workload boots and is debugged through TCP 3240 and the
   remote-bitbang bridge.
2. R5-0 then A53-1 each traverse the common 4-bit DDR EMIO pipeline.
3. Orbuculum decodes TCP 3402 directly.
4. At least 3,000,000,000 Orbflow payload bytes arrive in 60 seconds (400
   Mbit/s) with unchanged PL, DMA, ring, and Ethernet loss counters.
5. Forced overload latches overrun, advances loss counters monotonically, and
   cleanly resumes after reset.
