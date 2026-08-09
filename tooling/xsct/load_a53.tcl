# load_a53.tcl — load and run a bare-metal A53 ELF via JTAG (xsct)
#
# Usage (from repo root):
#   ZUB1CG_PSINIT=sdk/boards/zub_1cg/generated/psu_init.tcl \
#     xsct tooling/xsct/load_a53.tcl [path/to/app.elf]
#
# If no ELF is given, defaults to bazel-bin/apps/apu/blink/blink.elf.
#
# Steps:
#   1. rst -system on PSU (full PS reset)
#   2. psu_init (PLLs, MIO, DDR) + PS/PL isolation removal
#   3. program PL bitstream  (best-effort — needed for RGB LED)
#   4. rst -processor on A53 #0, download ELF, run
#
# Mirrors the confirmed-working jtag_run.tcl from the Vitis workspace.

if {![info exists ::env(ZUB1CG_PSINIT)]} {
    puts "ERROR: set ZUB1CG_PSINIT to a locally generated psu_init.tcl"
    exit 1
}
set psinit   [file normalize $::env(ZUB1CG_PSINIT)]
set bitfile  "board/zub_1cg/design_1_wrapper.bit"
set app      [expr {[llength $argv] > 0 \
                    ? [lindex $argv 0] \
                    : "bazel-bin/apps/apu/blink/blink.elf"}]

foreach f [list $psinit $app] {
    if {![file exists $f]} {
        puts "ERROR: $f not found — run this script from the repo root"
        exit 1
    }
}

connect

puts "=== load_a53.tcl — ZUBoard 1CG A53 ==="
puts "ELF:      $app"
puts "psu_init: $psinit"
puts ""
puts "Available targets:"
targets

# Step 1: full PS reset
puts "\n\[1\] Resetting PS..."
targets -set -nocase -filter {name =~ "PSU"}
rst -system
after 3000
mwr 0xffca0038 0x1ff

# Step 2: PS init (clocks, MIO, DDR) + PS-PL interface
puts "\[2\] Running psu_init..."
targets -set -nocase -filter {name =~ "APU*"}
source $psinit
psu_init
psu_ps_pl_isolation_removal
psu_ps_pl_reset_config
after 1000

# Step 3: program PL (best-effort — skip gracefully if target not found)
if {[file exists $bitfile]} {
    puts "\[3\] Programming PL: $bitfile"
    if {[catch {
        targets -set -nocase -filter {name =~ "*PL*"}
        fpga -f $bitfile
        puts "    PL programmed OK."
    } err]} {
        puts "    WARNING: PL program failed ($err)"
        puts "    RGB LED will not work; UART test still valid."
        targets -set -nocase -filter {name =~ "APU*"}
    }
} else {
    puts "\[3\] Skipping PL: $bitfile not found"
    puts "    RGB LED will not work; UART test still valid."
}

# Step 4: load and run
puts "\[4\] Loading $app on A53 #0..."
targets -set -nocase -filter {name =~ "*A53*#0"}
rst -processor
dow $app
con

puts "\nDone. A53 is running."
puts "Connect serial terminal:  picocom -b 115200 /dev/ttyUSB1"
puts "Expected output:"
puts "  === ZUBoard 1CG — A53 bare-metal ==="
puts "  UART OK"
puts "  GPIO TRI (before): 0x00000007"
puts "  GPIO TRI (after):  0x00000000  (expect 0x0)"
puts "  Cycling RGB LED — 0..7 with 1 s hold each"
puts "  LED 0x0  readback 0x0"
puts "  ..."
