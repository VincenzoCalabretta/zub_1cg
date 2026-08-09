# psu_init_run.tcl — run Vitis-generated psu_init via OpenOCD.
#
# Provides xsct primitives via xsct_shim.tcl so a locally generated
# psu_init.tcl can be sourced verbatim. Runs psu_init + psu_post_config +
# psu_ps_pl_isolation_removal + psu_ps_pl_reset_config.
#
# The DDR bringup subphase is called by psu_init and may fail if the DDR
# chip on this board revision doesn't match psu_init's assumptions; we
# wrap the whole thing in `catch` so we still fall through to R5 boot.
#
# Usage:
#   openocd -f scripts/openocd/aes_zub.cfg \
#           -f scripts/openocd/psu_init_run.tcl
#
# Chain with -f scripts/openocd/load_r5.tcl to boot R5 afterwards.

init
poll off

# All PS registers are on AXI; select the mem_ap.
targets uscale.axi

# psu_init.tcl defines a helper called `poll`. Preserve OpenOCD's command so
# load_r5.tcl can use `poll off` after this script finishes.
rename poll zub_openocd_poll

# Provide xsct's mrd, mwr, mask_write, init_ps, configparams.
source [file dirname [info script]]/xsct_shim.tcl

# Load psu_init.tcl (defines procs psu_init, psu_post_config, mask_read,
# mask_poll, psu_mask_write, poll, init_serdes, init_peripheral, plus
# psu_pll_init_data, psu_clock_init_data, psu_mio_init_data, …).
if {![info exists ::env(ZUB1CG_PSINIT)] || $::env(ZUB1CG_PSINIT) eq ""} {
    error "set ZUB1CG_PSINIT to a locally generated psu_init.tcl"
}
set zub_psinit [file normalize $::env(ZUB1CG_PSINIT)]
if {![file exists $zub_psinit]} { error "ZUB1CG_PSINIT does not exist: $zub_psinit" }
puts "Loading locally generated psu_init.tcl from $zub_psinit"
source $zub_psinit
puts "psu_init.tcl sourced."

# The generated file's mask_poll helper accepts two arguments, while its data
# blocks also use XSCT's three-argument form (address, mask, expected value).
# Support both forms while running those blocks.
rename mask_poll zub_psu_mask_poll
proc mask_poll { addr mask args } {
    set expected [expr {[llength $args] ? [lindex $args 0] : 1}]
    for {set count 0} {$count < 1000} {incr count} {
        set value [lindex [read_memory $addr 32 1] 0]
        if { ($value & $mask) == $expected } { return }
        after 1
    }
    puts [format {WARNING: mask_poll timeout at 0x%08x (mask 0x%08x, expected 0x%08x)} \
        $addr $mask $expected]
}

if { [catch { psu_init } err] } {
    puts "WARNING: psu_init failed: $err"
    puts "(this is often OK if only PS peripherals — not DDR — are needed)"
}
if { [catch { psu_post_config } err] } {
    puts "WARNING: psu_post_config: $err"
}
if { [catch { psu_ps_pl_isolation_removal } err] } {
    puts "WARNING: psu_ps_pl_isolation_removal: $err"
}
if { [catch { psu_ps_pl_reset_config } err] } {
    puts "WARNING: psu_ps_pl_reset_config: $err"
}

puts "psu_init sequence complete."
# Its work is complete now, so remove the Vitis helper and restore OpenOCD's
# `poll on|off` command before the loader is sourced.
rename poll {}
rename zub_openocd_poll poll
set zub_psu_initialized 1
