# load_m3.tcl — preload and start the PL-hosted Cortex-M3 via XSCT/JTAG.
#
# Usage (from repo root, after the Orbtrace PL image and A53 control
# firmware are already running):
#
#   xsct tooling/xsct/load_m3.tcl path/to/m3_app.bin
#
# The input must be a raw little-endian binary, for example:
#
#   nix develop -c bazel build //applications/orbtrace/firmware/m3_app:m3_app
#   arm-none-eabi-objcopy -O binary \
#     bazel-bin/applications/orbtrace/firmware/m3_app/m3_app m3_app.bin
#
# This script does not reset the PS or program the PL. It deliberately
# preserves the running A53 control firmware while it holds the M3 in reset,
# writes its 64 KiB BRAM through the PS-visible AXI window, verifies the first
# vector words, and then releases the M3.

set m3_bram_base 0xA0020000
set m3_bram_size 0x00010000
set m3_control   0xA00000A0

if {[llength $argv] != 1} {
    puts "Usage: xsct tooling/xsct/load_m3.tcl path/to/m3_app.bin"
    exit 1
}

set image [file normalize [lindex $argv 0]]
if {![file exists $image] || ![file isfile $image]} {
    puts "ERROR: M3 binary '$image' does not exist or is not a regular file"
    exit 1
}

set image_size [file size $image]
if {$image_size < 8} {
    puts "ERROR: M3 binary is only $image_size bytes; it lacks a reset vector"
    exit 1
}
if {$image_size > $m3_bram_size} {
    puts [format "ERROR: M3 binary is %d bytes; BRAM holds at most %d bytes" \
        $image_size $m3_bram_size]
    exit 1
}

# Return a 32-bit little-endian word from a raw firmware image. Read the
# bytes individually rather than relying on the host's integer byte order.
proc image_word_le {image_bytes offset} {
    binary scan [string range $image_bytes $offset [expr {$offset + 3}]] \
        cccc b0 b1 b2 b3
    return [expr {($b0 & 0xff) | (($b1 & 0xff) << 8) | \
                  (($b2 & 0xff) << 16) | (($b3 & 0xff) << 24)}]
}

set image_file [open $image rb]
fconfigure $image_file -translation binary -encoding binary
set image_bytes [read $image_file]
close $image_file

# The first two words are the initial MSP and reset PC. Verify up to four
# words after download, which catches wrong AXI mapping/byte order before the
# core leaves reset.
set check_words [expr {min(4, $image_size / 4)}]
set expected_words {}
for {set i 0} {$i < $check_words} {incr i} {
    lappend expected_words [image_word_le $image_bytes [expr {$i * 4}]]
}

connect
puts "=== load_m3.tcl — Orbtrace PL Cortex-M3 ==="
puts "Binary: [file tail $image] ($image_size bytes)"
puts [format "BRAM:   0x%08x..0x%08x" $m3_bram_base \
    [expr {$m3_bram_base + $m3_bram_size - 1}]]

# An APU target exposes the PS DAP/MEM-AP used for the PS-visible AXI windows.
targets -set -nocase -filter {name =~ "APU*"}

puts "\[1\] Holding M3 in reset..."
# -force is required on every mwr/dow/mrd below: this session's APU target
# has no associated hardware-design memory map (load_m3.tcl never loads an
# .xsa), so xsct's default access-protection blocks all PL AXI slave
# addresses ("This address has not been added to the memory map") even
# though the PS-PL AXI path is real and up (psu_ps_pl_isolation_removal ran
# during the earlier PL bitstream flash).
mwr -force $m3_control 0x00000000
after 10

puts "\[2\] Downloading binary to M3 BRAM..."
dow -force -data $image $m3_bram_base

puts "\[3\] Verifying reset-vector words..."
set actual_words [mrd -force -value $m3_bram_base $check_words]
for {set i 0} {$i < $check_words} {incr i} {
    set expected [expr {[lindex $expected_words $i] & 0xffffffff}]
    set actual [expr {[lindex $actual_words $i] & 0xffffffff}]
    if {$actual != $expected} {
        puts [format "ERROR: word %d readback mismatch: expected 0x%08x, got 0x%08x" \
            $i $expected $actual]
        exit 1
    }
    puts [format "  word %d = 0x%08x" $i $actual]
}

puts "\[4\] Releasing M3 reset..."
# Bit 0 is m3_release. Keep bit 1 clear here: Phase G enables the real DAP
# route separately after trace capture is established.
mwr -force $m3_control 0x00000001

puts "Done. M3 is running; configure source=m3 and start Orbtrace capture."
