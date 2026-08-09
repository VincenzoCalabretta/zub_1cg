#!/usr/bin/env bash
# One-command local presubmit: run this inside `nix develop` to reproduce CI.
# Exits 0 only if every check passes.
#
#   nix develop --command internal/presubmit/presubmit.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

FAIL=0
step() { echo; echo "── $* ──"; }
ok()   { echo "  [OK] $*"; }
fail() { echo "  [FAIL] $*"; FAIL=1; }

# ── Publication policy ─────────────────────────────────────────────────────
step "restricted generated sources"
TRACKED_PSINIT="$(git ls-files | while IFS= read -r path; do
    [ "${path##*/}" != "psu_init.tcl" ] || [ ! -e "$path" ] || printf '%s\n' "$path"
done)"
if [ -z "$TRACKED_PSINIT" ]; then
    ok "no generated PS-init source is tracked"
else
    printf '%s\n' "$TRACKED_PSINIT"
    fail "generated psu_init.tcl must remain local and untracked"
fi

# ── Buildifier (Starlark/BUILD formatting) ─────────────────────────────────
step "buildifier --mode=check"
if buildifier --mode=check -r . 2>&1; then
    ok "all BUILD/Starlark files are formatted"
else
    fail "run 'buildifier -r .' to fix"
fi

# ── Rust formatting ─────────────────────────────────────────────────────────
step "rustfmt --check"
RUST_SRCS=(
    tooling/elf_check/src/lib.rs
    tooling/elf_check/src/main.rs
    tooling/zub_ctl/src/lib.rs
    tooling/zub_ctl/src/main.rs
    tooling/pm_decode/src/main.rs
    sdk/boards/zub_1cg/artifact_integrity_test.rs
    applications/orbtrace/model/src/lib.rs
    applications/orbtrace/model/src/main.rs
    applications/orbtrace/firmware/common/src/lib.rs
    applications/orbtrace/firmware/a53/src/lib.rs
    applications/orbtrace/firmware/vexriscv/src/lib.rs
)
if rustfmt --edition 2021 --check "${RUST_SRCS[@]}" 2>&1; then
    ok "all Rust source files are formatted"
else
    fail "run 'rustfmt --edition 2021 ${RUST_SRCS[*]}' to fix"
fi

# ── Host tests ──────────────────────────────────────────────────────────────
step "bazel test --config=host (unit + ELF invariant + size budget)"
if bazel test --config=host \
    //tooling/... \
    //sdk/boards/zub_1cg:artifact_integrity_test \
    //tests:apu_blink_elf_test \
    //tests:apu_hello_world_elf_test \
    //tests:apu_eth_loopback_elf_test \
    //tests:rpu_hello_world_elf_test \
    //tests:rpu_bsp_test_elf_test \
    //tests:rpu_fault_test_elf_test \
    //tests:rpu_hello_world_size_test \
    //tests:rpu_bsp_test_size_test \
    //tests:rpu_fault_test_size_test \
    //applications/orbtrace/model:orbtrace_model_test \
    //applications/orbtrace/model:register_schema_test \
    //applications/orbtrace/firmware/common:firmware_common_test \
    //applications/orbtrace/firmware/a53:control_firmware_test \
    //applications/orbtrace/firmware/vexriscv:trace_workload_test; then
    ok "all host tests pass"
else
    fail "host tests failed"
fi

# ── Cross-compile links ─────────────────────────────────────────────────────
step "bazel build (transition-aware A53 firmware)"
if bazel build //applications/apu/...; then
    ok "APU build succeeded"
else
    fail "APU build failed"
fi

step "bazel build (transition-aware R5 firmware)"
if bazel build //applications/rpu/...; then
    ok "RPU build succeeded"
else
    fail "RPU build failed"
fi

# ── Summary ─────────────────────────────────────────────────────────────────
echo
if [ "$FAIL" -eq 0 ]; then
    echo "presubmit: all checks passed."
else
    echo "presubmit: FAILED — see [FAIL] lines above."
    exit 1
fi
