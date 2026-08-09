if {$argc != 2} { error "usage: export_psu_init.tcl DESIGN.xsa OUTPUT_DIR" }
set xsa [file normalize [lindex $argv 0]]
set output_dir [file normalize [lindex $argv 1]]
file mkdir $output_dir
hsi open_hw_design $xsa
hsi generate_app -hw [hsi current_hw_design] -os standalone \
    -proc psu_cortexa53_0 -app zynqmp_fsbl -sw fsbl \
    -dir [file join $output_dir psu_export]
if {![file exists [file join $output_dir psu_init.tcl]]} {
    error "HSI did not generate [file join $output_dir psu_init.tcl]"
}
hsi close_hw_design [hsi current_hw_design]
