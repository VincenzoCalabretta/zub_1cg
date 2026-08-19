#!/usr/bin/env bash
# On-target integration test: boot R5 orbtrace_workload via openocd, capture UART.
#
# Confirms the deterministic ETM trace workload (PS_CORESIGHT_TRACE_PLAN.md
# Phase 4) boots and starts its branchy control-flow loop -- does not itself
# attempt an ETM capture, see the plan doc for that (Phase 6 onward).
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
#   bazel test --config=host --config=onboard //tests:rpu_orbtrace_workload_test
set -euo pipefail

TTY="${ZUB_TTY:-/dev/ttyUSB1}"
BAUD="${ZUB_BAUD:-115200}"
TIMEOUT_S="${ZUB_TIMEOUT:-30}"

RUN="${RUNFILES_DIR:-$TEST_SRCDIR}/_main"

ZUB_CTL="$RUN/tooling/zub_ctl/zub_ctl"
if [[ ! -x "$ZUB_CTL" ]]; then
    ZUB_CTL="$(command -v zub_ctl || true)"
fi
if [[ -z "$ZUB_CTL" || ! -x "$ZUB_CTL" ]]; then
    echo "[INFRA FAIL] zub_ctl binary not found" >&2
    exit 1
fi

OPENOCD_CFG="$RUN/tooling/openocd/aes_zub.cfg"
LOAD_R5="$RUN/tooling/openocd/load_r5.tcl"
PSU_INIT_RUN="$RUN/tooling/openocd/psu_init_run.tcl"

ELF="$RUN/zub_firmware"
if [[ ! -f "$ELF" ]]; then
    echo "[INFRA FAIL] R5 ELF is missing from test runfiles" >&2
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
echo "=== Booting R5 orbtrace_workload firmware ===" >&2

# ── Boot + assertion ─────────────────────────────────────────────────────────
"$ZUB_CTL" watch-r5 \
    --openocd-cfg    "$OPENOCD_CFG" \
    --openocd-script "$PSU_INIT_RUN" \
    --openocd-script "$LOAD_R5" \
    --elf            "$ELF" \
    --tty            "$TTY" \
    --baud           "$BAUD" \
    --timeout        "$TIMEOUT_S" \
    --expect         '\[TEST PASS\] orbtrace_workload' \
    --fail-on        '\[TEST FAIL\]'
