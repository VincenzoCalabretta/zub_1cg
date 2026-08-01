#!/usr/bin/env bash
# On-target integration test: boot R5 hello_world via openocd, capture UART.
#
# Preconditions:
#   - Board plugged in (USB-JTAG at FT2232H, boot switches all OFF = JTAG mode)
#   - /dev/ttyUSB1 present (UART0 console via MIO 10/11)
#   - //apps/rpu/hello_world built with --config=rpu
#
# Workflow:
#   bazel build --config=rpu //apps/rpu/hello_world
#   bazel test --config=host --config=onboard //tests:rpu_hello_world_test
set -euo pipefail

TTY="${ZUB_TTY:-/dev/ttyUSB1}"
BAUD="${ZUB_BAUD:-115200}"
TIMEOUT_S="${ZUB_TIMEOUT:-30}"
if [[ ! -e "$TTY" ]]; then
    echo "FAIL: $TTY not present — is the board plugged in?"
    exit 1
fi

RUN="${RUNFILES_DIR:-$TEST_SRCDIR}/_main"
ZUB_CTL="$RUN/tools/zub_ctl/zub_ctl"
if [[ ! -x "$ZUB_CTL" ]]; then
    ZUB_CTL="$(command -v zub_ctl || true)"
fi
if [[ -z "$ZUB_CTL" || ! -x "$ZUB_CTL" ]]; then
    echo "FAIL: zub_ctl binary not found" >&2
    exit 1
fi

OPENOCD_CFG="$RUN/scripts/openocd/aes_zub.cfg"
LOAD_R5="$RUN/scripts/openocd/load_r5.tcl"
PSU_INIT_RUN="$RUN/scripts/openocd/psu_init_run.tcl"

# ELF is cross-compiled (--config=rpu) so it cannot be a Bazel data dep of a
# host-platform test.  Look in runfiles first, then fall back to bazel-bin.
ELF="$RUN/apps/rpu/hello_world/hello_world"
if [[ ! -f "$ELF" ]]; then
    WSROOT="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
    while [[ "$WSROOT" != "/" && ! -f "$WSROOT/MODULE.bazel" ]]; do
        WSROOT="$(dirname "$WSROOT")"
    done
    ELF="$WSROOT/bazel-bin/apps/rpu/hello_world/hello_world"
fi
if [[ ! -f "$ELF" ]]; then
    echo "FAIL: R5 ELF not found — pre-build with:"
    echo "  bazel build --config=rpu //apps/rpu/hello_world"
    exit 1
fi

exec "$ZUB_CTL" watch-r5 \
    --openocd-cfg    "$OPENOCD_CFG" \
    --openocd-script "$PSU_INIT_RUN" \
    --openocd-script "$LOAD_R5" \
    --elf            "$ELF" \
    --tty            "$TTY" \
    --baud           "$BAUD" \
    --timeout        "$TIMEOUT_S" \
    --expect         'Hello, World!'
