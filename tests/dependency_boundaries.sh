#!/usr/bin/env bash
set -euo pipefail

bad=$(bazel query 'deps(//sdk/... + //tooling/...) intersect //applications/...' 2>/dev/null || true)
if [[ -n "$bad" ]]; then
  echo "SDK/tooling application dependency found:" >&2
  echo "$bad" >&2
  exit 1
fi

if rg -n '//applications/' sdk tooling --glob '*.bzl' --glob 'BUILD.bazel'; then
  echo "application label appears in a supported public package" >&2
  exit 1
fi
