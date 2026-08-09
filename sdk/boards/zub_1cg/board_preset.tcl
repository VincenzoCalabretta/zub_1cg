# Reusable ZUBoard PS configuration sourced by licensed Vivado builds.
# The board definition itself is the pinned Apache-2.0 Avnet BDF dependency.

proc zub1cg_select_board {bdf_root} {
    set normalized [file normalize $bdf_root]
    if {![file exists [file join $normalized zub1cg 1.2 board.xml]]} {
        error "ZUBoard BDF not found below $normalized"
    }
    set_param board.repoPaths [list $normalized]

    set matches {}
    foreach board [get_board_parts -quiet *] {
        set name [string tolower $board]
        set part [get_property PART_NAME $board]
        if {[string first "zuboard" $name] >= 0 && $part eq "xczu1cg-sbva484-1-e"} {
            lappend matches $board
        }
    }
    if {[llength $matches] != 1} {
        error "expected exactly one ZUBoard 1CG board part, found: $matches"
    }
    set selected [lindex $matches 0]
    set_property board_part $selected [current_project]
    puts "Using ZUBoard board part: $selected"
    return $selected
}

proc zub1cg_require_property {cell property expected} {
    set actual [get_property $property $cell]
    if {$actual ne $expected} {
        error "$property is '$actual', expected '$expected'"
    }
}

proc zub1cg_apply_ps_preset {ps} {
    apply_bd_automation -rule xilinx.com:bd_rule:zynq_ultra_ps_e \
        -config {apply_board_preset "1"} $ps

    zub1cg_require_property $ps CONFIG.PSU__PSS_REF_CLK__FREQMHZ 33.333333
    zub1cg_require_property $ps CONFIG.PSU__DDRC__MEMORY_TYPE {LPDDR 4}
    zub1cg_require_property $ps CONFIG.PSU__DDRC__BUS_WIDTH {32 Bit}
    zub1cg_require_property $ps CONFIG.PSU__DDRC__DEVICE_CAPACITY {8192 MBits}
    zub1cg_require_property $ps CONFIG.PSU__UART0__PERIPHERAL__ENABLE 1
    zub1cg_require_property $ps CONFIG.PSU__UART0__PERIPHERAL__IO {MIO 10 .. 11}
}

proc zub1cg_json_escape {value} {
    return [string map [list "\\" "\\\\" "\"" "\\\""] $value]
}

proc zub1cg_write_ps_fingerprint {ps output_file} {
    set properties [lsort {
        CONFIG.PSU__PSS_REF_CLK__FREQMHZ
        CONFIG.PSU__DDRC__MEMORY_TYPE
        CONFIG.PSU__DDRC__BUS_WIDTH
        CONFIG.PSU__DDRC__DRAM_WIDTH
        CONFIG.PSU__DDRC__DEVICE_CAPACITY
        CONFIG.PSU__DDRC__SPEED_BIN
        CONFIG.PSU__UART0__PERIPHERAL__ENABLE
        CONFIG.PSU__UART0__PERIPHERAL__IO
        CONFIG.PSU__ENET2__PERIPHERAL__ENABLE
        CONFIG.PSU__ENET2__PERIPHERAL__IO
        CONFIG.PSU__I2C1__PERIPHERAL__ENABLE
        CONFIG.PSU__I2C1__PERIPHERAL__IO
        CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ
        CONFIG.PSU__USE__M_AXI_GP0
        CONFIG.PSU__USE__S_AXI_GP2
    }]
    set handle [open $output_file w]
    puts $handle "{"
    puts $handle {  "schema": 1,}
    puts $handle {  "board": "Avnet ZUBoard 1CG",}
    puts $handle {  "bdf_version": "1.2",}
    puts $handle "  \"properties\": {"
    set last [expr {[llength $properties] - 1}]
    for {set index 0} {$index <= $last} {incr index} {
        set property [lindex $properties $index]
        set value [zub1cg_json_escape [get_property $property $ps]]
        set comma [expr {$index == $last ? "" : ","}]
        puts $handle "    \"$property\": \"$value\"$comma"
    }
    puts $handle "  }"
    puts $handle "}"
    close $handle
}
