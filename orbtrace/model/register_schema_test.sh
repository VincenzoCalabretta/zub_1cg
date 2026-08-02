#!/usr/bin/env bash
set -euo pipefail
binary="${TEST_SRCDIR}/${TEST_WORKSPACE}/orbtrace/model/orbtrace"
expected="${TEST_SRCDIR}/${TEST_WORKSPACE}/orbtrace/rtl/orbtrace_regs.svh"
generated="${TEST_TMPDIR}/orbtrace_regs.svh"
"${binary}" gen-registers > "${generated}"
diff -u "${expected}" "${generated}"

