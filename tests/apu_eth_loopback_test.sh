#!/usr/bin/env bash
# On-target integration test: eth_loopback GEM2 internal-loopback test.
set -euo pipefail

TTY="${ZUB_TTY:-/dev/ttyUSB1}"
BAUD="${ZUB_BAUD:-115200}"
TIMEOUT_S="${ZUB_TIMEOUT:-30}"
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

ELF="$RUN/apps/apu/eth_loopback/eth_loopback_a53.elf"
if [[ ! -f "$ELF" ]]; then
    WSROOT="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
    while [[ "$WSROOT" != "/" && ! -f "$WSROOT/MODULE.bazel" ]]; do
        WSROOT="$(dirname "$WSROOT")"
    done
    ELF="$WSROOT/bazel-bin/apps/apu/eth_loopback/eth_loopback_a53.elf"
fi
if [[ ! -f "$ELF" ]]; then
    echo "FAIL: eth_loopback_a53.elf not found — pre-build with:"
    echo "  bazel build --config=apu //apps/apu/eth_loopback:eth_loopback_a53.elf"
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
    --expect    'GEM2 internal loopback test' \
    --expect    'frame sent, polling' \
    --expect    'RX len=60' \
    --expect    'PASS: loopback OK' \
    --fail-on   'FAIL:'
