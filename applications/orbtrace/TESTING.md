# Verification and release gates

Host-verifiable targets:

```sh
bazel test --config=host //applications/orbtrace/model:orbtrace_model_test
bazel test --config=host //applications/orbtrace/model:register_schema_test
bazel test --config=host //applications/orbtrace/firmware/common:firmware_common_test
bazel test --config=host //applications/orbtrace/firmware/m3:trace_workload_test
bazel test //applications/orbtrace/firmware/a53:control_firmware_test
XILINX_ROOT=/opt/Xilinx \
  bazel test //applications/orbtrace/rtl:rtl_unit_test \
    --test_env=XVLOG --test_env=XELAB --test_env=XSIM \
    --test_env=XILINX_ROOT
```

In the Nix development shell, `XVLOG`, `XELAB`, and `XSIM` point to FHS
wrappers around the separately installed Vivado tree. On a conventional Linux
host, leave those variables unset and set `XILINX_VIVADO` to the Vivado 2023.2
installation directory instead.

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

1. The PL-hosted Cortex-M3's deterministic ITM/TPIU firmware
   (`//applications/orbtrace/firmware/m3_app`) boots and its trace is
   captured via source_select==0, and is debugged through TCP 3240 and the
   remote-bitbang bridge with `ORBTRACE_REG_M3_CONTROL` bit 1 set (routes
   DAP_JTAG_Sequence/DAP_SWJ_Pins to the M3's real JTAG-DP instead of
   `orbtrace_dap_engine.sv`'s synthetic responder — see its `use_real_target`
   path). DAP_Transfer (register-level DP/AP access, not used by
   remote-bitbang) remains synthetic-only; there's no ADIv5 DPACC/APACC
   implementation here.
2. R5-0 then A53-1 each traverse the common 4-bit DDR EMIO pipeline.
3. Orbuculum decodes TCP 3402 directly.
4. At least 3,000,000,000 Orbflow payload bytes arrive in 60 seconds (400
   Mbit/s) with unchanged PL, DMA, ring, and Ethernet loss counters.
5. Forced overload latches overrun, advances loss counters monotonically, and
   cleanly resumes after reset.
