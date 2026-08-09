# Engineering documentation

This directory contains the dated hardware-validation records for the ZUBoard
SDK and Orbtrace implementation. The reports are retained chronologically so
that intermediate hypotheses, failed experiments, and artifact provenance are
auditable rather than being rewritten after the final result.

## Current authoritative result

The final engineering report is:

- [Orbtrace 400 Mbit/s trace logging: engineering and verification report](ORBTRACE_400MBPS_TECHNICAL_REPORT_2026-08-09.md)

It records the hardware-proven 3 GB acceptance capture at 641,059,166 bit/s
with unchanged loss/fault counters. The accepted bitstream and A53 ELF are
identified by SHA-256 in that report.

## Chronological evidence

- [On-target test report — 2026-08-07](ON_TARGET_TEST_REPORT_2026-08-07.md):
  A53/R5 board execution and foundational SDK validation.
- [Orbtrace analysis and test report — 2026-08-08](ORBTRACE_TEST_REPORT_2026-08-08.md):
  initial RTL, firmware, JTAG, Ethernet, GEM2, and NetX bring-up.
- [Orbtrace 400 Mbit/s handoff — 2026-08-09](ORBTRACE_400MBPS_HANDOFF_2026-08-09.md):
  session-by-session investigation history through final acceptance.
- [Orbtrace 400 Mbit/s technical report — 2026-08-09](ORBTRACE_400MBPS_TECHNICAL_REPORT_2026-08-09.md):
  consolidated root-cause, implementation, verification, and reproduction
  report. Prefer this document for release claims.

The project requirement is 400 Mbit/s, equivalent to 50 MB/s. It is not a
400 MB/s requirement.

## Release verification

The release-preparation rerun on 2026-08-09 used the same artifact hashes as
the final technical report and passed all of the following:

- repository presubmit: publication policy, buildifier, rustfmt, all 18 host
  tests, and transition-aware A53/R5 firmware builds;
- Vivado 2023.2 XSim suite: capture, CMSIS-DAP, 64-bit AXI stream packer, and
  randomized ping-pong pipeline tests;
- physical-board acceptance: 3,000,000,000 bytes in 37.433 seconds at
  641,056,632 bit/s with unchanged `dropped_bytes`, `sync_loss`, and
  `dma_faults` counters.

Vendor PDFs and private generated document conversions are intentionally not
stored here. See [`internal/reference_docs/`](../internal/reference_docs/) for
publisher links and the repository's documentation-publication policy.
