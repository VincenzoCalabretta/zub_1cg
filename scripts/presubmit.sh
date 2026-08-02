#!/usr/bin/env bash
# One-command local presubmit: run this inside `nix develop` to reproduce CI.
# Exits 0 only if every check passes.
#
#   nix develop --command scripts/presubmit.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FAIL=0
step() { echo; echo "── $* ──"; }
ok()   { echo "  [OK] $*"; }
fail() { echo "  [FAIL] $*"; FAIL=1; }

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
    tools/elf_check/src/lib.rs
    tools/elf_check/src/main.rs
    tools/zub_ctl/src/lib.rs
    tools/zub_ctl/src/main.rs
    tools/pm_decode/src/main.rs
    board/zub_1cg/artifact_integrity_test.rs
)
if rustfmt --edition 2021 --check "${RUST_SRCS[@]}" 2>&1; then
    ok "all Rust source files are formatted"
else
    fail "run 'rustfmt --edition 2021 ${RUST_SRCS[*]}' to fix"
fi

# ── Host tests ──────────────────────────────────────────────────────────────
step "bazel test --config=host (unit + ELF invariant + size budget)"
if bazel test --config=host \
    //tools/... \
    //board/zub_1cg:artifact_integrity_test \
    //tests:apu_blink_elf_test \
    //tests:apu_hello_world_elf_test \
    //tests:apu_eth_loopback_elf_test \
    //tests:rpu_hello_world_elf_test \
    //tests:rpu_bsp_test_elf_test \
    //tests:rpu_fault_test_elf_test \
    //tests:rpu_hello_world_size_test \
    //tests:rpu_bsp_test_size_test \
    //tests:rpu_fault_test_size_test; then
    ok "all host tests pass"
else
    fail "host tests failed"
fi

# ── Cross-compile links ─────────────────────────────────────────────────────
step "bazel build --config=apu (A53 firmware)"
if bazel build --config=apu //apps/apu/...; then
    ok "APU build succeeded"
else
    fail "APU build failed"
fi

step "bazel build --config=rpu (R5 firmware)"
if bazel build --config=rpu //apps/rpu/...; then
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
