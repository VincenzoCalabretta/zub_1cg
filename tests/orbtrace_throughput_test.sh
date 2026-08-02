#!/usr/bin/env bash
set -euo pipefail
: "${ORBTRACE_HOST:?set ORBTRACE_HOST to the board address}"
orbtrace="${TEST_SRCDIR}/${TEST_WORKSPACE}/orbtrace/model/orbtrace"
capture="${TEST_TMPDIR}/orbflow.bin"

before=$("${orbtrace}" stats "${ORBTRACE_HOST}")
start_ns=$(date +%s%N)
timeout 70 "${orbtrace}" capture "${ORBTRACE_HOST}" "${capture}" 3000000000
end_ns=$(date +%s%N)
after=$("${orbtrace}" stats "${ORBTRACE_HOST}")

elapsed_ns=$((end_ns-start_ns))
bytes=$(stat -c %s "${capture}")
bits_per_second=$((bytes*8000000000/elapsed_ns))
if (( bits_per_second < 400000000 )); then
    echo "throughput ${bits_per_second} bit/s is below 400000000" >&2
    exit 1
fi
for counter in dropped_bytes sync_loss dma_faults; do
    before_value=$(sed -n "s/.*${counter}=\([0-9]*\).*/\1/p" <<<"${before}")
    after_value=$(sed -n "s/.*${counter}=\([0-9]*\).*/\1/p" <<<"${after}")
    if [[ "${before_value}" != "${after_value}" ]]; then
        echo "${counter} changed: ${before_value} -> ${after_value}" >&2
        exit 1
    fi
done
echo "orbtrace throughput: ${bits_per_second} bit/s, zero reported loss"
