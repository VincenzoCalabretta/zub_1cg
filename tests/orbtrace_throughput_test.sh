#!/usr/bin/env bash
set -euo pipefail
# The Orbtrace endpoint is the ZUBoard itself: its A53 service exposes the
# control and Orbflow TCP ports after the Orbtrace image has been deployed.
# A lab with a different static subnet can override this address.
ORBTRACE_BOARD_IP="${ORBTRACE_BOARD_IP:-192.168.1.50}"
ORBTRACE_CAPTURE_BYTES="${ORBTRACE_CAPTURE_BYTES:-3000000000}"
ORBTRACE_MIN_BITS_PER_SECOND="${ORBTRACE_MIN_BITS_PER_SECOND:-400000000}"
orbtrace="${TEST_SRCDIR}/${TEST_WORKSPACE}/applications/orbtrace/model/orbtrace"
capture="${TEST_TMPDIR}/orbflow.bin"

if ! before=$("${orbtrace}" stats "${ORBTRACE_BOARD_IP}"); then
    echo "FAIL: Orbtrace control service is unavailable at ${ORBTRACE_BOARD_IP}:3401" >&2
    echo "Load the Orbtrace PL bitstream and A53 service, and configure the host Ethernet interface on 192.168.1.0/24." >&2
    exit 1
fi

# Use the PL's deterministic byte source in raw mode. This exercises the real
# Orbflow encoder, AXI stream, AXI DMA SG ring, target TCP stack, GEM2, PHY,
# host NIC, and file sink without making the acceptance rate depend on which
# software workload happens to be running on a traced CPU.
"${orbtrace}" configure "${ORBTRACE_BOARD_IP}" test swo-nrz 2000000
"${orbtrace}" start "${ORBTRACE_BOARD_IP}"
start_ns=$(date +%s%N)
timeout 75 "${orbtrace}" capture "${ORBTRACE_BOARD_IP}" "${capture}" "${ORBTRACE_CAPTURE_BYTES}"
end_ns=$(date +%s%N)
after=$("${orbtrace}" stats "${ORBTRACE_BOARD_IP}")
"${orbtrace}" stop "${ORBTRACE_BOARD_IP}"

elapsed_ns=$((end_ns-start_ns))
bytes=$(stat -c %s "${capture}")
# Convert nanoseconds to microseconds before multiplying. The old
# bytes*8,000,000,000 expression overflowed signed 64-bit shell arithmetic at
# the required 3 GB acceptance size and could not report a valid rate.
elapsed_us=$((elapsed_ns/1000))
bits_per_second=$((bytes*8000000/elapsed_us))
if (( bits_per_second < ORBTRACE_MIN_BITS_PER_SECOND )); then
    echo "throughput ${bits_per_second} bit/s is below ${ORBTRACE_MIN_BITS_PER_SECOND}" >&2
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
