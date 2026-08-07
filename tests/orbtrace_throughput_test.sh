#!/usr/bin/env bash
set -euo pipefail
# The Orbtrace endpoint is the ZUBoard itself: its A53 service exposes the
# control and Orbflow TCP ports after the Orbtrace image has been deployed.
# A lab with a different static subnet can override this address.
ORBTRACE_BOARD_IP="${ORBTRACE_BOARD_IP:-192.168.1.50}"
orbtrace="${TEST_SRCDIR}/${TEST_WORKSPACE}/orbtrace/model/orbtrace"
capture="${TEST_TMPDIR}/orbflow.bin"

if ! timeout 3 bash -c "</dev/tcp/${ORBTRACE_BOARD_IP}/3401"; then
    echo "FAIL: Orbtrace control service is unavailable at ${ORBTRACE_BOARD_IP}:3401" >&2
    echo "Load the Orbtrace PL bitstream and A53 service, and configure the host Ethernet interface on 192.168.1.0/24." >&2
    exit 1
fi

before=$("${orbtrace}" stats "${ORBTRACE_BOARD_IP}")
"${orbtrace}" start "${ORBTRACE_BOARD_IP}"
start_ns=$(date +%s%N)
timeout 60 "${orbtrace}" capture "${ORBTRACE_BOARD_IP}" "${capture}" 3000000000
end_ns=$(date +%s%N)
after=$("${orbtrace}" stats "${ORBTRACE_BOARD_IP}")

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
