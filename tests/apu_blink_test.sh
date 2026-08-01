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
#   bazel build --config=apu //apps/apu/blink:blink.elf   # cross-compile (pre-build)
#   bazel test --config=host //tests:apu_blink_test        # run on hardware
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
ZUB_CTL="$RUN/tools/zub_ctl/zub_ctl"
BIT="$RUN/board/zub_1cg/design_1_wrapper.bit"
PSI="$RUN/board/zub_1cg/psu_init.tcl"

# The ELF is cross-compiled (--config=apu) and cannot be a Bazel data dep of
# a host-platform test.  Look in runfiles first (works if a transition is ever
# added), then fall back to the workspace bazel-bin output.
ELF="$RUN/apps/apu/blink/blink.elf"
if [[ ! -f "$ELF" ]]; then
    # Walk up from the script to find the workspace root (contains bazel-bin/).
    WSROOT="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
    while [[ "$WSROOT" != "/" && ! -f "$WSROOT/MODULE.bazel" ]]; do
        WSROOT="$(dirname "$WSROOT")"
    done
    ELF="$WSROOT/bazel-bin/apps/apu/blink/blink.elf"
fi

if [[ ! -f "$ELF" ]]; then
    echo "SKIP: blink.elf not found — pre-build with:"
    echo "  bazel build --config=apu //apps/apu/blink:blink.elf"
    exit 0
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
