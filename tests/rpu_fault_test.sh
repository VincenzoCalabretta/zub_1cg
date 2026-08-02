#!/usr/bin/env bash
# On-target postmortem-capture test: boots R5 fault_test via OpenOCD,
# injects an undefined instruction, and verifies the exception handler
# captures the record and emits [TEST PASS] before halting.
#
# Failure categories emitted on stderr:
#   [PRECONDITION FAIL]  device/tool missing, permission denied, artifact corrupt
#   [BOOT FAIL]          openocd exited nonzero before UART matched
#   [ASSERTION FAIL]     expected pattern not found (reported by zub_ctl)
#   [TIMEOUT]            UART pattern not matched within --timeout seconds
#   [INFRA FAIL]         test infrastructure error (missing runfile, bad arg)
#
# Preconditions:
#   - Board plugged in (USB-JTAG at FT2232H, boot switches all OFF = JTAG mode)
#   - /dev/ttyUSB1 present (UART0 console via MIO 10/11)
#
# Workflow:
#   bazel test --config=host --config=onboard //tests:rpu_fault_test
set -euo pipefail

TTY="${ZUB_TTY:-/dev/ttyUSB1}"
BAUD="${ZUB_BAUD:-115200}"
TIMEOUT_S="${ZUB_TIMEOUT:-30}"

RUN="${RUNFILES_DIR:-$TEST_SRCDIR}/_main"

ZUB_CTL="$RUN/tools/zub_ctl/zub_ctl"
if [[ ! -x "$ZUB_CTL" ]]; then
    ZUB_CTL="$(command -v zub_ctl || true)"
fi
if [[ -z "$ZUB_CTL" || ! -x "$ZUB_CTL" ]]; then
    echo "[INFRA FAIL] zub_ctl binary not found" >&2
    exit 1
fi

OPENOCD_CFG="$RUN/scripts/openocd/aes_zub.cfg"
LOAD_R5="$RUN/scripts/openocd/load_r5.tcl"
PSU_INIT_RUN="$RUN/scripts/openocd/psu_init_run.tcl"

ELF="$RUN/apps/rpu/fault_test/fault_test"
if [[ ! -f "$ELF" ]]; then
    echo "[INFRA FAIL] R5 fault_test ELF is missing from test runfiles" >&2
    exit 1
fi
if [[ ! -f "$OPENOCD_CFG" ]]; then
    echo "[INFRA FAIL] OpenOCD config not in runfiles: $OPENOCD_CFG" >&2
    exit 1
fi

# ── Precondition: board presence and tool availability ───────────────────────
echo "=== Preflight check ===" >&2
if ! "$ZUB_CTL" doctor --tty "$TTY" --openocd openocd 2>&1; then
    echo "[PRECONDITION FAIL] zub_ctl doctor failed — fix the issues above" >&2
    exit 1
fi
echo "=== Booting R5 fault_test firmware ===" >&2

# ── Boot + assertion ─────────────────────────────────────────────────────────
# The UND exception handler emits [POSTMORTEM] lines then [TEST PASS].
"$ZUB_CTL" watch-r5 \
    --openocd-cfg    "$OPENOCD_CFG" \
    --openocd-script "$PSU_INIT_RUN" \
    --openocd-script "$LOAD_R5" \
    --elf            "$ELF" \
    --tty            "$TTY" \
    --baud           "$BAUD" \
    --timeout        "$TIMEOUT_S" \
    --expect         '\[TEST PASS\] fault_capture' \
    --fail-on        '\[TEST FAIL\]'
