#!/usr/bin/env bash
# On-target test: boot blink on A53 via xsct, verify UART + AXI GPIO.
#
# What the test verifies:
#   1. JTAG ELF load succeeds (A53 runs from DDR 0x0)
#   2. UART0 output reaches ttyUSB1          (-> "UART OK")
#   3. AXI GPIO at 0xa0000000 is reachable   (-> "LED 0x0  readback 0x0")
#      zub_ctl watch-a53 programs the PL bitstream before loading the ELF.
#
# Workflow:
#   bazel test --config=host --config=onboard //tests:apu_blink_test
set -euo pipefail

TTY="${ZUB_TTY:-/dev/ttyUSB1}"
BAUD="${ZUB_BAUD:-115200}"
TIMEOUT_S="${ZUB_TIMEOUT:-60}"
XSCT="${XSCT:-/mnt/data/xilinx/Vitis/2023.2/bin/xsct}"

if [[ ! -e "$TTY" ]]; then
    echo "FAIL: $TTY not present — is the board plugged in?"
    exit 1
fi
if [[ ! -x "$XSCT" ]]; then
    echo "FAIL: xsct not found at $XSCT (set XSCT env var to override)"
    exit 1
fi

RUN="${RUNFILES_DIR:-$TEST_SRCDIR}/_main"
ZUB_CTL="$RUN/tooling/zub_ctl/zub_ctl"
BIT="$RUN/sdk/boards/zub_1cg/design_1_wrapper.bit"
PSI="${ZUB1CG_PSINIT:?set ZUB1CG_PSINIT to the locally generated psu_init.tcl}"
[[ -f "$PSI" ]] || { echo "FAIL: ZUB1CG_PSINIT does not exist: $PSI"; exit 1; }

# The test rule cross-compiles this ELF for A53 and supplies it in runfiles.
ELF="$RUN/zub_firmware"
if [[ ! -f "$ELF" ]]; then
    echo "FAIL: blink.elf is missing from test runfiles"
    exit 1
fi

exec "$ZUB_CTL" watch-a53 \
    --xsct      "$XSCT" \
    --elf       "$ELF" \
    --bitstream "$BIT" \
    --psinit    "$PSI" \
    --tty       "$TTY" \
    --baud      "$BAUD" \
    --timeout   "$TIMEOUT_S" \
    --expect    'UART OK' \
    --expect    'LED 0x0  readback 0x0'
