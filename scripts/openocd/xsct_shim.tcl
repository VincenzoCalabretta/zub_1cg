# xsct_shim.tcl — provide the xsct primitives that Vitis-generated
# psu_init.tcl expects so it can be sourced verbatim from OpenOCD.
#
# Primitives xsct exposes that OpenOCD does not:
#   mrd -force $addr        → read 32-bit; returns "ADDR: HEXVAL" string;
#                             psu_init consumes only the last 8 hex chars
#   mwr -force $addr $val   → write 32-bit
#   configparams NAME VAL   → tuning knob, no-op for us
#   init_ps DATA            → eval DATA as tcl commands (a data block of
#                             mask_write statements)
#   mask_write $addr $m $v  → RMW helper (name distinct from psu_mask_write
#                             which psu_init.tcl defines internally)
#
# The `-force` flag is a no-op in xsct too; we accept and drop it.

# Read 32-bit from AXI space via OpenOCD.
#
# psu_init's mask_read / mask_poll extract the tail with
#   "0x[string range [mrd -force $addr] end-8 end]"
# and rely on that being a valid TCL integer literal. Since [end-8 end] is
# 9 characters (inclusive), we format so that those 9 chars are "0<8hex>":
#   "FF5E0074:001010F00"  ← 18 chars; end-8 = index 9
# Prefixed with "0x" → "0x001010F00" which TCL parses as the 32-bit value.
proc mrd { args } {
    set addr [lindex $args end]
    set val [lindex [read_memory $addr 32 1] 0]
    return [format "%08X:%09X" $addr $val]
}

# Write 32-bit via OpenOCD.
proc mwr { args } {
    if { [llength $args] == 3 } {
        # mwr -force ADDR VAL
        set addr [lindex $args 1]
        set val  [lindex $args 2]
    } else {
        # mwr ADDR VAL
        set addr [lindex $args 0]
        set val  [lindex $args 1]
    }
    mww $addr $val
}

# xsct's `mask_write` performs an RMW; psu_init.tcl data lists invoke this
# name. psu_init.tcl separately defines `psu_mask_write` (identical impl).
proc mask_write { addr mask value } {
    set cur [lindex [read_memory $addr 32 1] 0]
    set new [expr {($cur & ~$mask) | ($value & $mask)}]
    mww $addr $new
}

# xsct's `init_ps` receives a data block (multi-line TCL) and executes it
# in the caller's context. `uplevel #0` runs it at global scope so any
# `variable` refs still resolve.
proc init_ps { data } {
    if { [catch { uplevel #0 $data } err] } {
        puts "  init_ps: WARNING — $err"
    }
}

# `configparams force-mem-accesses N` — xsct tuning knob, no-op here.
proc configparams { args } {
    return ""
}

# Generated PSU data uses either `mask_delay ms` or `mask_delay addr ms`.
proc mask_delay { args } {
    after [lindex $args end]
}

# The data lists in psu_init.tcl use `after ms` too via OpenOCD's native
# `after`. If any list references `sleep`, alias here.
if { [info commands sleep] eq "" } {
    proc sleep { seconds } {
        after [expr { int($seconds * 1000) }]
    }
}
