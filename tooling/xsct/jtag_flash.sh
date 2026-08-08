#!/usr/bin/env bash
# Usage (via bazel run //flash): jtag_flash.sh <elf_path>
#
# Environment variables (all have defaults):
#   XSCT      — path to the Xilinx xsct binary
#   PSINIT    — path to psu_init.tcl
#   BITSTREAM — path to the PL bitstream (.bit)
#
# The PSINIT and BITSTREAM defaults point to the in-repo copies under
# sdk/boards/zub_1cg/.  Override them if you have board-specific variants
# (e.g. a different PL design's bitstream, such as Orbtrace's).
set -euo pipefail

# Resolve the workspace root: when called via `bazel run`, $BUILD_WORKSPACE_DIRECTORY
# is set.  Fall back to the directory two levels above this script.
WORKSPACE_ROOT="${BUILD_WORKSPACE_DIRECTORY:-$(cd "$(dirname "$0")/.." && pwd)}"

XSCT="${XSCT:-/mnt/data/xilinx/Vitis/2023.2/bin/xsct}"
PSINIT="${PSINIT:-${WORKSPACE_ROOT}/sdk/boards/zub_1cg/psu_init.tcl}"
BITSTREAM="${BITSTREAM:-${WORKSPACE_ROOT}/sdk/boards/zub_1cg/design_1_wrapper.bit}"
ELF="${1:?Usage: jtag_flash.sh <path-to-elf>}"

if [[ ! -x "$XSCT" ]]; then
    echo "ERROR: xsct not found at '$XSCT'." >&2
    echo "  Install Xilinx Vitis 2023.2 and set XSCT to its xsct binary, e.g.:" >&2
    echo "    export XSCT=/path/to/Vitis/2023.2/bin/xsct" >&2
    exit 1
fi

# Resolve to absolute path so xsct can find the file regardless of CWD.
ELF="$(realpath "$ELF")"

TCL="$(mktemp /tmp/jtag_flash_XXXXXX.tcl)"
trap 'rm -f "$TCL"' EXIT

cat > "$TCL" <<EOF
connect

puts "Connected. Available targets:"
targets

puts "\\nResetting system..."
targets -set -nocase -filter {name =~ "PSU"}
rst -system
after 3000
mwr 0xffca0038 0x1ff

puts "Programming PL bitstream: $BITSTREAM"
fpga -file $BITSTREAM

puts "Running psu_init (clocks, MIO, DDR)..."
targets -set -nocase -filter {name =~ "APU*"}
source $PSINIT
psu_init
psu_ps_pl_isolation_removal
psu_ps_pl_reset_config
after 1000

puts "Loading ELF on A53 #0: $ELF"
targets -set -nocase -filter {name =~ "*A53*#0"}
rst -processor
dow $ELF
con

puts "\\nDone. Connect serial terminal at 115200 baud."
EOF

exec "$XSCT" -nodisp "$TCL"
