# psu_init_only.tcl — run PSU init via xsct, then exit.
#
# Sources the locally generated psu_init.tcl passed as argv[0] and
# invokes:
#   - psu_init                       — clocks, PLLs, MIO pin routing
#   - psu_post_config
#   - psu_ps_pl_isolation_removal
#   - psu_ps_pl_reset_config
#
# The BOOT.BIN / R5 boot path itself is handed off to OpenOCD afterwards
# (see zub_ctl watch-r5 with --pre-xsct).
connect

puts "=== psu_init_only.tcl ==="
puts "Available targets:"
targets

puts "\nResetting PS..."
targets -set -nocase -filter {name =~ "PSU"}
rst -system
after 3000
mwr 0xffca0038 0x1ff

puts "\nRunning psu_init on APU#0..."
targets -set -nocase -filter {name =~ "APU*"}
source [lindex $argv 0]
psu_init
psu_ps_pl_isolation_removal
psu_ps_pl_reset_config
after 500

puts "\npsu_init complete. Disconnecting."
disconnect
exit 0
