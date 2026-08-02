if {$argc != 2} { error "usage: xsct export_psu_init.tcl DESIGN.xsa OUTPUT_DIR" }
set xsa [lindex $argv 0]
set out [lindex $argv 1]
hsi open_hw_design $xsa
hsi generate_app -hw [hsi current_hw_design] -os standalone -proc psu_cortexa53_0 \
    -app zynqmp_fsbl -sw fsbl -dir [file join $out psu_export]
# In Vitis 2023.2, HSI writes the platform initialization files alongside the
# application directory rather than below its legacy src/ subdirectory.
if {![file exists [file join $out psu_init.tcl]]} {
    error "HSI did not generate [file join $out psu_init.tcl]"
}
hsi close_hw_design [hsi current_hw_design]
