# flash.tcl — XSCT script: program BOOT.BIN to QSPI flash via USB-JTAG.
#
# Prerequisites:
#   - Board connected via USB-JTAG (on-board FTDI or equivalent)
#   - Board boot mode switches set to JTAG (see board schematic)
#   - Vitis / XSCT installed and in PATH
#   - BOOT.BIN already generated (see scripts/boot.bif)
#
# Usage:
#   xsct scripts/flash.tcl
#
# After programming, set boot mode switches to QSPI and power-cycle.

# ── Connect to hw_server ─────────────────────────────────────────────

connect

# Give the JTAG chain a moment to enumerate
after 500

# Print detected targets for diagnostic purposes
puts "Detected JTAG targets:"
targets

# ── Select the PSU (top-level Zynq UltraScale+ target) ───────────────

targets -set -filter {name =~ "PSU"}

# ── Program the QSPI flash ────────────────────────────────────────────
#
# flash_type options: qspi-x4-single, qspi-x8-dual, emmc
# Adjust to match the flash device on your board (check board schematic).

set boot_bin "BOOT.BIN"
if {![file exists $boot_bin]} {
    puts "ERROR: $boot_bin not found. Run bootgen first."
    exit 1
}

puts "Programming $boot_bin to QSPI flash..."
program_flash \
    -f        $boot_bin     \
    -offset   0             \
    -flash_type qspi-x4-single

puts "Flash programming complete."
puts "Now set boot mode to QSPI and power-cycle the board."

disconnect
